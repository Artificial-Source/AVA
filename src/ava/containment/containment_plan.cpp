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
std::uint64_t device_file_mask() noexcept;
[[nodiscard]] ava::core::VoidResult validate_synthetic_directory_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors);
[[nodiscard]] ava::core::VoidResult validate_writable_directory_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors);
[[nodiscard]] ava::core::VoidResult validate_device_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors);
[[nodiscard]] bool path_is_within(std::filesystem::path const& child, std::filesystem::path const& parent);
void add_filesystem_rule(std::vector<ContainmentFilesystemRule>& rules, std::filesystem::path const& path, std::uint64_t mask);
}  // namespace detail

// The installer returns only a fixed frame; diagnostics are parent-only.
[[nodiscard]] ContainmentChildStatus apply_seccomp_network_filter(bool network_allowed) noexcept;
[[nodiscard]] ContainmentChildStatus apply_no_new_privs() noexcept;

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
  // Exact device files needed for normal process/file work, including writable /dev/null.
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

  // 9. Exact device files: read/write without device-ioctl authority.
  add_device_files(rules, scope, *plan.anchor_set);

  // Bind approved identity now; application preparation reopens and validates
  // it in the parent immediately before fork, retaining exact rule descriptors.
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

ContainmentChildStatus apply_containment_in_child(PreparedContainment& prepared) noexcept
{
  if (!prepared.ruleset || prepared.rules.empty())
    return {ContainmentFailureStage::DescriptorIdentity, EINVAL};
  for (auto const& rule : prepared.rules)
  {
    struct stat status{};
    if (::fstat(rule.descriptor.get(), &status) != 0)
      return {ContainmentFailureStage::DescriptorIdentity, errno};
    auto const& approved = rule.identity;
    if (status.st_dev != approved.st_dev || status.st_ino != approved.st_ino || status.st_mode != approved.st_mode || status.st_uid != approved.st_uid ||
        status.st_gid != approved.st_gid || status.st_rdev != approved.st_rdev)
      return {ContainmentFailureStage::DescriptorIdentity, ESTALE};
  }
  if (auto status = apply_no_new_privs(); !status)
    return status;
  if (::syscall(SYS_landlock_restrict_self, prepared.ruleset.get(), 0) != 0)
    return {ContainmentFailureStage::Landlock, errno};
  // Close authority before user code; reset only calls close and scalar stores.
  prepared.ruleset.reset();
  for (auto& rule : prepared.rules)
    rule.descriptor.reset();
  return apply_seccomp_network_filter(prepared.network_allowed);
}

ava::core::Error containment_child_error(ContainmentChildStatus status)
{
  char const* message = "invalid containment child status";
  switch (status.stage)
  {
    case ContainmentFailureStage::None:
      break;
    case ContainmentFailureStage::DescriptorIdentity:
      message = "Landlock rule descriptor changed after containment approval";
      break;
    case ContainmentFailureStage::NoNewPrivileges:
      message = "failed to set PR_SET_NO_NEW_PRIVS";
      break;
    case ContainmentFailureStage::Landlock:
      message = "failed to enforce Landlock ruleset";
      break;
    case ContainmentFailureStage::Seccomp:
      message = "failed to install seccomp network filter";
      break;
  }
  return ava::core::Error(ava::core::ErrorCategory::PermissionDenied, message).with_context("cause", std::strerror(status.error_number));
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
