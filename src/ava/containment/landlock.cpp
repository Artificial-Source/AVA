#include "sys.h"
#include "ava/command/private_group.h"
#include "ava/containment/containment.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ava::containment {

// Landlock IOCTL_DEV was added in ABI 5 (Linux 6.2). The installed kernel
// headers may predate it, so define the stable constant when absent.
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif

#ifndef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
#define LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)
#endif
#ifndef LANDLOCK_SCOPE_SIGNAL
#define LANDLOCK_SCOPE_SIGNAL (1ULL << 1)
#endif

namespace {

// Stable ABI-6 layout, independent of the build host's kernel headers.
struct LandlockRulesetAttrV6
{
  std::uint64_t handled_access_fs = 0;
  std::uint64_t handled_access_net = 0;
  std::uint64_t scoped = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Rights available at each Landlock ABI version. The handled mask must never
// include bits the running kernel does not understand, or landlock_create_ruleset
// fails with EOPNOTSUPP/EINVAL.
//
// ABI 1: execute, write_file, read_file, read_dir, remove_dir, remove_file,
//        make_char, make_dir, make_reg, make_sock, make_fifo, make_block,
//        make_sym
// ABI 2: + refer (always implicitly handled when available)
// ABI 3: + truncate
// ABI 5: + ioctl_dev
[[nodiscard]] std::uint64_t abi_handled_access_fs(std::uint32_t abi_version) noexcept
{
  std::uint64_t mask = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                       LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                       LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                       LANDLOCK_ACCESS_FS_MAKE_SYM;
  if (abi_version >= 2)
    mask |= LANDLOCK_ACCESS_FS_REFER;
  if (abi_version >= 3)
    mask |= LANDLOCK_ACCESS_FS_TRUNCATE;
  if (abi_version >= 5)
    mask |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
  return mask;
}

// Full read/write/create/execute mask for writable roots (workspace, synthetic
// environment). The child can read, write, create, remove, and execute files
// beneath these roots. IOCTL_DEV is included only when the ABI supports it so
// writable roots can use ioctl on regular files (e.g. terminal devices opened
// inside the workspace).
[[nodiscard]] std::uint64_t read_write_execute_mask(std::uint32_t abi_version) noexcept
{
  return abi_handled_access_fs(abi_version);
}

// Read/execute mask for system roots and toolchain directories. The child can
// read files and list directories but not modify them.
[[nodiscard]] std::uint64_t read_execute_mask() noexcept
{
  return LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
}

// Read/execute mask for individual regular files. File rules use only
// file-valid rights (READ_FILE and EXECUTE), never READ_DIR, because
// Landlock rejects READ_DIR on a non-directory path.
[[nodiscard]] std::uint64_t read_execute_file_mask() noexcept
{
  return LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE;
}

// Read-only mask for device files.
[[nodiscard]] std::uint64_t read_only_mask() noexcept
{
  return LANDLOCK_ACCESS_FS_READ_FILE;
}

ava::core::Error landlock_error(std::string message)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, std::move(message));
  error.with_context("cause", std::strerror(errno));
  return error;
}

[[nodiscard]] int landlock_create_ruleset_fd(std::uint64_t handled_access_fs, std::uint32_t abi_version)
{
  LandlockRulesetAttrV6 attr{};
  attr.handled_access_fs = handled_access_fs;
  if (abi_version >= 6)
    attr.scoped = LANDLOCK_SCOPE_SIGNAL | LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET;
  // ABI 1–3 contain only the filesystem mask; ABI 4 adds the network mask.
  // Do not pass unsupported trailing fields to older kernels.
  auto const size =
      abi_version >= 6 ? sizeof(attr) : (abi_version >= 4 ? offsetof(LandlockRulesetAttrV6, scoped) : offsetof(LandlockRulesetAttrV6, handled_access_net));
  return static_cast<int>(::syscall(SYS_landlock_create_ruleset, &attr, size, 0));
}

[[nodiscard]] ava::core::VoidResult add_path_rule(int ruleset_fd, ava::core::AnchorSet const& anchors, ContainmentFilesystemRule const& rule)
{
  if (!rule.identity_bound)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Landlock rule has no sealed descriptor identity"));
  auto parent = ava::core::open_readable(anchors, rule.logical_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (!parent)
    return std::unexpected(landlock_error("failed to open descriptor-bound Landlock rule parent"));
  struct stat status{};
  if (::fstat(parent->fd(), &status) != 0 || static_cast<std::uint64_t>(status.st_dev) != rule.device ||
      static_cast<std::uint64_t>(status.st_ino) != rule.inode || static_cast<std::uint64_t>(status.st_mode) != rule.mode ||
      static_cast<std::uint64_t>(status.st_uid) != rule.owner || static_cast<std::uint64_t>(status.st_gid) != rule.group ||
      static_cast<std::uint64_t>(status.st_rdev) != rule.special_device)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Landlock rule descriptor changed after containment approval"));
  }

  struct landlock_path_beneath_attr path_attr{};
  path_attr.allowed_access = rule.access_mask;
  path_attr.parent_fd = parent->fd();

  int const result = static_cast<int>(::syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0));
  if (result < 0)
    return std::unexpected(landlock_error("failed to add Landlock filesystem rule"));
  return {};
}

}  // namespace

ava::core::VoidResult apply_landlock_in_child(DevelopmentContainmentPlan const& plan)
{
  if (plan.filesystem_rules.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "containment plan has no filesystem rules"));

  int const ruleset_fd = landlock_create_ruleset_fd(plan.handled_access_fs, plan.landlock_abi_version);
  if (ruleset_fd < 0)
  {
    if (errno == EINVAL)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                              "Landlock ruleset creation rejected the handled access mask; kernel ABI may be insufficient"));
    return std::unexpected(landlock_error("failed to create Landlock ruleset"));
  }

  // Add each validated rule. If any rule fails, the ruleset is closed and the
  // child must not exec: fail closed before user code starts.
  if (!plan.anchor_set)
  {
    ::close(ruleset_fd);
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "containment plan has no shared AnchorSet"));
  }
  for (auto const& rule : plan.filesystem_rules)
  {
    auto added = add_path_rule(ruleset_fd, *plan.anchor_set, rule);
    if (!added)
    {
      ::close(ruleset_fd);
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Landlock rule installation failed; containment is not active"));
    }
  }

  int const restrict_result = static_cast<int>(::syscall(SYS_landlock_restrict_self, ruleset_fd, 0));
  ::close(ruleset_fd);
  if (restrict_result < 0)
    return std::unexpected(landlock_error("failed to enforce Landlock ruleset"));

  return {};
}

namespace {

[[nodiscard]] ava::core::Result<struct stat> descriptor_status(std::filesystem::path const& path, ava::core::AnchorSet const& anchors, bool directory)
{
  auto opened = ava::core::open_readable(anchors, path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  struct stat status{};
  if (::fstat(opened->fd(), &status) != 0)
    return std::unexpected(landlock_error("failed to inspect descriptor-bound Landlock path"));
  return status;
}

// Validate that a path is an existing directory owned by the current user
// with no group/other permissions. Logical symlinks are accepted only when
// open_beneath keeps them inside their selected anchor.
[[nodiscard]] ava::core::VoidResult validate_synthetic_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors)
{
  auto captured = descriptor_status(path, anchors, true);
  if (!captured)
    return std::unexpected(std::move(captured.error()));
  auto const& status = *captured;
  if (!S_ISDIR(status.st_mode))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Landlock synthetic root is not a directory"));
  if (status.st_uid != ::geteuid())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Landlock synthetic root is not owned by the current user"));
  if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Landlock synthetic root is not owner-only (must be 0700)"));
  return {};
}

// Validate that a path is an existing non-symlink directory owned by the
// current user. Group/world read/execute (0755/0750) is permitted. Group write
// is permitted only for the current account's verified private primary group;
// world write, symlinks, special file types, and identity mismatches are
// rejected. This is used for the workspace root.
[[nodiscard]] ava::core::VoidResult validate_writable_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors)
{
  auto captured = descriptor_status(path, anchors, true);
  if (!captured)
    return std::unexpected(std::move(captured.error()));
  auto const& status = *captured;
  if (!S_ISDIR(status.st_mode))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Landlock writable root is not a directory"));
  if (status.st_uid != ::geteuid())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Landlock writable root is not owned by the current user"));
  if ((status.st_mode & S_IWOTH) != 0 || ((status.st_mode & S_IWGRP) != 0 && !ava::command::detail::is_current_user_private_primary_group_directory(status)))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                            "Landlock writable root has group/world write permissions outside the verified private-primary-group exception"));
  }
  return {};
}

// Validate that a path is an existing non-symlink file (regular or device).
[[nodiscard]] ava::core::VoidResult validate_device_file(std::filesystem::path const& path, ava::core::AnchorSet const& anchors)
{
  auto captured = descriptor_status(path, anchors, false);
  if (!captured)
    return std::unexpected(std::move(captured.error()));
  return {};
}

[[nodiscard]] bool is_within(std::filesystem::path const& child, std::filesystem::path const& parent)
{
  if (!child.is_absolute() || !parent.is_absolute())
    return false;
  auto const normalized_child = child.lexically_normal();
  auto const normalized_parent = parent.lexically_normal();
  auto const relative = normalized_child.lexically_relative(normalized_parent);
  if (normalized_child == normalized_parent)
    return true;
  return !relative.empty() && *relative.begin() != "..";
}

void add_rule(std::vector<ContainmentFilesystemRule>& rules, std::filesystem::path const& path, std::uint64_t mask)
{
  auto logical = path.lexically_normal();
  for (auto const& existing : rules)
  {
    if (existing.logical_path == logical)
      return;
  }
  rules.push_back({.logical_path = std::move(logical), .access_mask = mask});
}

}  // namespace

// These accessors are used by containment_plan.cpp to build the rule list.
// They are in an anonymous detail namespace within this translation unit.
namespace detail {

std::uint64_t handled_access_fs_mask(std::uint32_t abi_version) noexcept
{
  return abi_handled_access_fs(abi_version);
}

std::uint64_t writable_root_mask(std::uint32_t abi_version) noexcept
{
  return read_write_execute_mask(abi_version);
}

std::uint64_t read_execute_root_mask() noexcept
{
  return read_execute_mask();
}

std::uint64_t read_execute_file_root_mask() noexcept
{
  return read_execute_file_mask();
}

std::uint64_t read_only_root_mask() noexcept
{
  return read_only_mask();
}

std::uint64_t device_file_mask() noexcept
{
  // /dev/null needs WRITE_FILE for output redirection; /dev/zero and
  // /dev/urandom need READ_FILE. All device files get both. Device files are
  // deliberate: they do not receive IOCTL_DEV automatically.
  return LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
}

ava::core::VoidResult validate_synthetic_directory_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors)
{
  return validate_synthetic_root(path, anchors);
}

ava::core::VoidResult validate_writable_directory_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors)
{
  return validate_writable_root(path, anchors);
}

ava::core::VoidResult validate_device_root(std::filesystem::path const& path, ava::core::AnchorSet const& anchors)
{
  return validate_device_file(path, anchors);
}

[[nodiscard]] bool path_is_within(std::filesystem::path const& child, std::filesystem::path const& parent)
{
  return is_within(child, parent);
}

void add_filesystem_rule(std::vector<ContainmentFilesystemRule>& rules, std::filesystem::path const& path, std::uint64_t mask)
{
  add_rule(rules, path, mask);
}

}  // namespace detail

}  // namespace ava::containment
