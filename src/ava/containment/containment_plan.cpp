#include "sys.h"
#include "ava/containment/containment.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/fd_io.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ava::containment {

// Landlock access masks and validation are implemented in landlock.cpp.
// Forward declarations for the detail helpers used here.
namespace detail {
[[nodiscard]] std::uint64_t handled_access_fs_mask(std::uint32_t abi_version) noexcept;
[[nodiscard]] std::uint64_t writable_root_mask(std::uint32_t abi_version) noexcept;
[[nodiscard]] std::uint64_t read_execute_root_mask() noexcept;
[[nodiscard]] std::uint64_t read_execute_file_root_mask() noexcept;
[[nodiscard]] std::uint64_t read_only_root_mask() noexcept;
std::uint64_t device_file_mask() noexcept;
[[nodiscard]] ava::core::VoidResult validate_synthetic_directory_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors);
[[nodiscard]] ava::core::VoidResult validate_writable_directory_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors);
[[nodiscard]] ava::core::VoidResult validate_device_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors);
[[nodiscard]] bool path_is_within(std::filesystem::path const& child, std::filesystem::path const& parent);
void add_filesystem_rule(std::vector<ContainmentFilesystemRule>& rules, std::filesystem::path const& path, std::uint64_t mask);
}  // namespace detail

// Landlock application is in landlock.cpp.
[[nodiscard]] ava::core::VoidResult apply_landlock_in_child(DevelopmentContainmentPlan const& plan);
// Seccomp application is in seccomp_network.cpp.
[[nodiscard]] ava::core::VoidResult apply_seccomp_network_filter(bool network_allowed);
[[nodiscard]] ava::core::VoidResult apply_no_new_privs();

namespace {

[[nodiscard]] std::optional<struct stat> stat_through_anchors(ava::core::AnchorSet const& anchors, std::filesystem::path const& path, int flags)
{
  auto opened = ava::core::open_readable(anchors, path, flags);
  if (!opened)
    return std::nullopt;
  struct stat status{};
  if (::fstat(opened->fd(), &status) != 0)
    return std::nullopt;
  return status;
}

[[nodiscard]] bool path_is_directory(std::filesystem::path const& path, ava::core::AnchorSet const& anchors) noexcept
{
  auto const status = stat_through_anchors(anchors, path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC);
  return status && S_ISDIR(status->st_mode);
}

[[nodiscard]] bool directory_covers_target(std::filesystem::path const& target, std::filesystem::path const& directory, ava::core::AnchorSet const& anchors)
{
  if (directory.empty())
    return false;
  auto opened = ava::core::open_readable(anchors, directory, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC);
  if (!opened)
    return false;

  // Executable canonical identities use descriptor-derived physical paths.
  // Compare against the same spelling of the granted directory, not its
  // logical alias. The descriptor (never this spelling) remains rule authority.
  auto const physical = ava::core::descriptor_physical_path(opened->fd());
  return physical && detail::path_is_within(target, *physical);
}

void add_system_roots(std::vector<ContainmentFilesystemRule>& rules, ava::core::AnchorSet const& anchors,
                      std::vector<std::filesystem::path> const& authority_roots)
{
  // System roots that provide executables, shared libraries, and headers.
  // These are read/execute only; the child cannot modify system files. Omit a
  // broad system root if it would expose an explicitly protected AVA root.
  for (auto const* root : {"/usr", "/lib", "/lib64", "/bin", "/sbin", "/etc"})
  {
    bool const overlaps_authority = std::ranges::any_of(authority_roots, [root](std::filesystem::path const& authority) {
      return detail::path_is_within(authority, root) || detail::path_is_within(root, authority);
    });
    if (!overlaps_authority && path_is_directory(root, anchors))
      detail::add_filesystem_rule(rules, root, detail::read_execute_root_mask());
  }
}

void add_device_files(std::vector<ContainmentFilesystemRule>& rules, ContainmentFilesystemScope& scope, ava::core::AnchorSet const& anchors)
{
  // Exact device files needed for normal process/file work. Read-only.
  for (auto const* dev : {"/dev/null", "/dev/zero", "/dev/urandom", "/dev/random"})
  {
    if (detail::validate_device_root(dev, anchors))
    {
      detail::add_filesystem_rule(rules, dev, detail::device_file_mask());
      ++scope.device_file_count;
    }
  }
}

ava::core::VoidResult seal_filesystem_rule_identities(std::vector<ContainmentFilesystemRule>& rules, ava::core::AnchorSet const& anchors)
{
  for (auto& rule : rules)
  {
    auto const status = stat_through_anchors(anchors, rule.logical_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (!status)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "failed to seal a containment filesystem rule"));
    rule.device = static_cast<std::uint64_t>(status->st_dev);
    rule.inode = static_cast<std::uint64_t>(status->st_ino);
    rule.mode = static_cast<std::uint64_t>(status->st_mode);
    rule.owner = static_cast<std::uint64_t>(status->st_uid);
    rule.group = static_cast<std::uint64_t>(status->st_gid);
    rule.special_device = static_cast<std::uint64_t>(status->st_rdev);
    rule.identity_bound = true;
  }
  return {};
}

void mark_unavailable(DevelopmentContainmentPlan& plan, std::string reason)
{
  plan.availability = ContainmentAvailability::Unavailable;
  plan.profile_id = "ava-landlock-seccomp-v1";
  plan.unavailable_reason = std::move(reason);
}

[[nodiscard]] bool requires_exact_file_rule(ava::command::CommandPlan const& command_plan, ava::core::AnchorSet const& anchors,
                                            std::filesystem::path const& directory)
{
  auto const& home = command_plan.trusted_home_metadata();
  if (detail::path_is_within(home.requested_path, directory) || detail::path_is_within(home.canonical_path, directory) ||
      std::ranges::any_of(command_plan.ava_authority_roots(), [&](std::filesystem::path const& authority) {
        return detail::path_is_within(authority, directory) || detail::path_is_within(directory, authority);
      }))
    return true;
  // Logical parents can alias the trusted home through a directory symlink.
  // An uninspectable parent must not gain broad directory authority either.
  auto const status = stat_through_anchors(anchors, directory, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC);
  return !status || (static_cast<std::uintmax_t>(status->st_dev) == home.device && static_cast<std::uintmax_t>(status->st_ino) == home.inode);
}

[[nodiscard]] bool covered_by_executable(std::filesystem::path const& target, ava::command::ExecutableMetadata const& executable,
                                         ava::command::CommandPlan const& command_plan, ava::core::AnchorSet const& anchors)
{
  auto const directory = executable.requested_path.parent_path();
  if (requires_exact_file_rule(command_plan, anchors, directory) && target != executable.canonical_path)
    return false;
  return directory_covers_target(target, directory, anchors);
}

[[nodiscard]] bool target_is_covered(std::filesystem::path const& target, ava::command::CommandPlan const& command_plan, ava::core::AnchorSet const& anchors,
                                     std::vector<ContainmentFilesystemRule> const& rules)
{
  if (target.empty())
    return false;
  for (auto const& rule : rules)
  {
    if (directory_covers_target(target, rule.logical_path, anchors))
      return true;
  }
  // Account for the executable/interpreter grants below without treating
  // an exact file rule as authority over other targets in its directory.
  for (auto const& entry : command_plan.path_entries())
  {
    if (!requires_exact_file_rule(command_plan, anchors, entry.directory) && directory_covers_target(target, entry.directory, anchors))
      return true;
  }
  if (auto const& resolved = command_plan.resolved_executable(); resolved)
  {
    if (covered_by_executable(target, resolved->executable, command_plan, anchors))
      return true;
    for (auto const& interpreter : resolved->shebang_interpreters)
    {
      if (covered_by_executable(target, interpreter.interpreter, command_plan, anchors))
        return true;
    }
  }
  return false;
}

[[nodiscard]] bool require_target_coverage(DevelopmentContainmentPlan& plan, ava::command::CommandPlan const& command_plan,
                                           std::vector<ContainmentFilesystemRule> const& rules, ava::command::ExecutableMetadata const& executable,
                                           char const* role)
{
  if (target_is_covered(executable.canonical_path, command_plan, *plan.anchor_set, rules))
    return true;
  mark_unavailable(plan, std::string("canonical ") + role + " target is outside containment roots; explicit approval is required for this toolchain");
  return false;
}

void add_executable_rule(std::vector<ContainmentFilesystemRule>& rules, ContainmentFilesystemScope& scope, ava::command::CommandPlan const& command_plan,
                         ava::core::AnchorSet const& anchors, std::filesystem::path const& path)
{
  auto const directory = path.parent_path();
  if (path.empty() || directory.empty() || !path_is_directory(directory, anchors))
    return;
  if (requires_exact_file_rule(command_plan, anchors, directory))
    detail::add_filesystem_rule(rules, path, detail::read_execute_file_root_mask());
  else
    detail::add_filesystem_rule(rules, directory, detail::read_execute_root_mask());
  ++scope.read_only_root_count;
}

}  // namespace

std::uint32_t probe_landlock_abi_version() noexcept
{
  errno = 0;
  long const result = ::syscall(SYS_landlock_create_ruleset, nullptr, 0u, LANDLOCK_CREATE_RULESET_VERSION);
  if (result < 0)
    return 0;
  return static_cast<std::uint32_t>(result);
}

DevelopmentContainmentPlan prepare_development_containment(ava::command::CommandPreparation const& preparation, bool network_enabled)
{
  DevelopmentContainmentPlan plan;
  plan.network_allowed = network_enabled;
  plan.anchor_set = preparation.plan().anchor_set();
  if (!plan.anchor_set)
  {
    mark_unavailable(plan, "sealed command has no shared AnchorSet");
    return plan;
  }

  // Probe kernel features without mutating parent state.
  plan.landlock_abi_version = probe_landlock_abi_version();
  if (plan.landlock_abi_version < kRequiredLandlockAbiVersion)
  {
    mark_unavailable(plan, "Landlock ABI " + std::to_string(plan.landlock_abi_version) + " is below the required version " +
                               std::to_string(kRequiredLandlockAbiVersion) + " (truncate/refer not mediated)");
    return plan;
  }

  if (!seccomp_network_filter_supported())
  {
    mark_unavailable(plan, "seccomp network filter is not supported on this architecture or kernel");
    return plan;
  }

  auto const& command_plan = preparation.plan();

  // Build the filesystem rule list from the sealed preparation.
  std::vector<ContainmentFilesystemRule> rules;
  ContainmentFilesystemScope scope;

  // 1. Workspace: read/write/execute (the child builds and tests here).
  //    Group write is accepted only for the verified private-primary-group
  //    exception; world write, unsafe shared groups, and identity mismatch fail.
  auto const workspace = command_plan.workspace();
  if (auto valid = detail::validate_writable_directory_root(workspace, *plan.anchor_set); !valid)
  {
    mark_unavailable(plan, "workspace root failed containment validation");
    return plan;
  }
  detail::add_filesystem_rule(rules, workspace, detail::writable_root_mask(plan.landlock_abi_version));
  scope.workspace_writable = true;

  // 2. Synthetic environment roots: read/write (HOME, XDG, TMP). These must
  //    be exact current-user 0700 (synthetic, private, owner-only).
  auto const& env = preparation.environment();
  for (auto const* root : {&env.synthetic_roots().home, &env.synthetic_roots().xdg_config_home, &env.synthetic_roots().xdg_cache_home,
                           &env.synthetic_roots().xdg_data_home, &env.synthetic_roots().xdg_state_home, &env.synthetic_roots().tmpdir})
  {
    auto const logical = root->canonical_path;
    if (logical.empty())
      continue;
    if (auto valid = detail::validate_synthetic_directory_root(logical, *plan.anchor_set); !valid)
    {
      mark_unavailable(plan, "synthetic environment root failed containment validation");
      return plan;
    }
    detail::add_filesystem_rule(rules, logical, detail::writable_root_mask(plan.landlock_abi_version));
  }
  scope.synthetic_environment_writable = true;

  // 3. System roots: read/execute (executables, libraries, headers, config).
  add_system_roots(rules, *plan.anchor_set, command_plan.ava_authority_roots());

  // A symlinked launcher/interpreter may resolve outside every root we will
  // grant (for example bin/npm -> ../lib/.../npm-cli.js). Reject that layout
  // before promising containment; do not widen authority to its target.
  if (auto const& resolved = command_plan.resolved_executable(); resolved)
  {
    if (!require_target_coverage(plan, command_plan, rules, resolved->executable, "executable"))
      return plan;
    for (auto const& interpreter : resolved->shebang_interpreters)
    {
      if (!require_target_coverage(plan, command_plan, rules, interpreter.interpreter, "interpreter"))
        return plan;
    }
  }

  // 4. Sealed PATH entries: read/execute (toolchain directories).
  for (auto const& entry : command_plan.path_entries())
  {
    auto const logical = entry.directory;
    if (logical.empty() || !path_is_directory(logical, *plan.anchor_set))
      continue;
    detail::add_filesystem_rule(rules, logical, detail::read_execute_root_mask());
    ++scope.read_only_root_count;
  }

  // 5–6. Executable/interpreter directories normally allow sibling resources.
  // A parent covering the real home or overlapping AVA authority must instead
  // grant only the exact executable file, never directory traversal/read rights.
  if (auto const& resolved = command_plan.resolved_executable(); resolved)
  {
    add_executable_rule(rules, scope, command_plan, *plan.anchor_set, resolved->executable.requested_path);
    for (auto const& interpreter : resolved->shebang_interpreters)
      add_executable_rule(rules, scope, command_plan, *plan.anchor_set, interpreter.interpreter.requested_path);
  }

  // 8. Recipe path arguments: directories get writable access; regular files
  //    get file-valid read/execute only (never READ_DIR). Redundant rules
  //    already covered by the writable workspace are omitted to avoid
  //    stale or conflicting masks.
  if (command_plan.classification().recipe)
  {
    for (auto const& path_arg : command_plan.classification().recipe->path_arguments)
    {
      if (path_arg.canonical_path.empty())
        continue;
      // Omit recipe paths already beneath the writable workspace.
      if (detail::path_is_within(path_arg.canonical_path, workspace))
        continue;
      if (path_is_directory(path_arg.canonical_path, *plan.anchor_set))
      {
        if (auto valid = detail::validate_writable_directory_root(path_arg.canonical_path, *plan.anchor_set); valid)
          detail::add_filesystem_rule(rules, path_arg.canonical_path, detail::writable_root_mask(plan.landlock_abi_version));
      }
      else
      {
        // Regular file: use file-valid rights only (READ_FILE | EXECUTE),
        // never READ_DIR which Landlock rejects on non-directory paths.
        detail::add_filesystem_rule(rules, path_arg.canonical_path, detail::read_execute_file_root_mask());
      }
    }
  }

  // 9. Device files: read-only (/dev/null, /dev/zero, etc.).
  add_device_files(rules, scope, *plan.anchor_set);

  // Bind every rule to the descriptor identity validated in the parent. The
  // child reopens and verifies this identity before adding that same descriptor
  // to Landlock, closing pathname-replacement races across fork.
  if (auto sealed = seal_filesystem_rule_identities(rules, *plan.anchor_set); !sealed)
  {
    mark_unavailable(plan, "containment filesystem rule identity could not be sealed");
    return plan;
  }

  // Assemble the plan.
  plan.availability = ContainmentAvailability::Available;
  plan.profile_id = "ava-landlock-seccomp-v1";
  plan.handled_access_fs = detail::handled_access_fs_mask(plan.landlock_abi_version);
  plan.filesystem_rules = std::move(rules);
  plan.filesystem_scope = scope;
  // Both network modes require a filter; even network-enabled commands must
  // not acquire UNIX service sockets or bypass the policy through io_uring.
  // Installation happens in the child and is verified before exec.
  plan.network_filter_planned = true;
  return plan;
}

ava::core::VoidResult apply_containment_in_child(DevelopmentContainmentPlan const& plan)
{
  if (plan.availability != ContainmentAvailability::Available)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "containment plan is not available"));

  // Set no_new_privs first. The kernel requires PR_SET_NO_NEW_PRIVS before
  // both landlock_restrict_self and seccomp filter installation. It is
  // inherited across execve and prevents privilege escalation through setuid.
  if (auto result = apply_no_new_privs(); !result)
    return result;

  // Install Landlock filesystem rules. Landlock is enforced as long as the
  // ruleset is properly installed and persists across execve.
  if (auto result = apply_landlock_in_child(plan); !result)
    return result;

  // Both modes restrict socket domains and io_uring; network-denied commands
  // additionally reject socket() and connect(), retaining private pair IPC.
  if (auto result = apply_seccomp_network_filter(plan.network_allowed); !result)
    return result;

  return {};
}

void close_inherited_fds_except(int keep_fd_a, int keep_fd_b, int keep_fd_c) noexcept
{
  // Use close_range(2) where available for O(1) bulk closure, then fall back
  // to a bounded loop. The handshake fds and the approved executable
  // descriptor are never closed before descriptor exec.
  int const lo = STDERR_FILENO + 1;
  auto const close_range_available = [](unsigned int low, unsigned int high) { return ::syscall(SYS_close_range, low, high, 0u) == 0; };

  std::array<int, 3> keep_fds{keep_fd_a, keep_fd_b, keep_fd_c};
  std::ranges::sort(keep_fds);
  auto const unique_end = std::unique(keep_fds.begin(), keep_fds.end());
  unsigned int next = static_cast<unsigned int>(lo);
  for (auto keep_iterator = keep_fds.begin(); keep_iterator != unique_end; ++keep_iterator)
  {
    auto const keep_fd = *keep_iterator;
    if (keep_fd < lo)
      continue;
    auto const keep = static_cast<unsigned int>(keep_fd);
    if (next < keep)
      static_cast<void>(close_range_available(next, keep - 1));
    next = keep + 1;
  }
  // Close everything above the final kept descriptor (including very high
  // descriptors beyond the sysconf bound). keep_fd is an int, so next cannot
  // wrap here.
  static_cast<void>(close_range_available(next, ~0u));

  // Bounded fallback: iterate and close any remaining non-keep fds. This
  // catches cases where close_range is unavailable (ENOSYS) or a segment
  // failed. The bound prevents scanning millions of entries.
  long const open_max = ::sysconf(_SC_OPEN_MAX);
  int const max_fd = open_max > 0 ? static_cast<int>(open_max) : 1024;
  for (int fd = lo; fd < max_fd; ++fd)
  {
    if (fd == keep_fd_a || fd == keep_fd_b || fd == keep_fd_c)
      continue;
    static_cast<void>(::close(fd));
  }
}

bool containment_is_available(DevelopmentContainmentPlan const& plan) noexcept
{
  return plan.availability == ContainmentAvailability::Available;
}

std::string containment_summary(DevelopmentContainmentPlan const& plan)
{
  std::string summary = "{\"profile\":\"";
  summary += plan.profile_id;
  summary += "\",\"availability\":\"";
  summary += plan.availability == ContainmentAvailability::Available ? "available" : "unavailable";
  summary += "\",\"landlock_abi\":";
  summary += std::to_string(plan.landlock_abi_version);
  summary += ",\"network_allowed\":";
  summary += plan.network_allowed ? "true" : "false";
  // Before fork, the filter is "planned", not "installed". The parent only
  // reports "installed" after verifying the child's successful handshake.
  summary += ",\"network_filter\":\"";
  summary += plan.network_filter_planned ? "planned" : "not_required";
  summary += "\"";
  summary += ",\"filesystem\":{\"workspace_writable\":";
  summary += plan.filesystem_scope.workspace_writable ? "true" : "false";
  summary += ",\"synthetic_environment_writable\":";
  summary += plan.filesystem_scope.synthetic_environment_writable ? "true" : "false";
  summary += ",\"read_only_roots\":";
  summary += std::to_string(plan.filesystem_scope.read_only_root_count);
  summary += ",\"device_files\":";
  summary += std::to_string(plan.filesystem_scope.device_file_count);
  summary += "}}";
  return summary;
}

}  // namespace ava::containment
