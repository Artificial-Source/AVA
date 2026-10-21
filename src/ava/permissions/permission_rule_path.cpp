#include "sys.h"
#include "ava/command/private_group.h"
#include "ava/permissions/permission_rules_internal.h"
#include "ava/core/path.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ava::permissions::permission_rules_internal {

std::string errno_message(int error_number)
{
  return std::strerror(error_number);
}

ava::core::Error rule_file_error(ava::core::ErrorCategory category, std::string message, std::filesystem::path const& path)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("path", path.string());
  return error;
}

ava::core::Error rule_parse_error(std::string message, std::filesystem::path const& path, std::string_view field, std::optional<std::size_t> index)
{
  auto error = rule_file_error(ava::core::ErrorCategory::InvalidArgument, std::move(message), path);
  if (!field.empty())
    error.with_context("field", std::string(field));
  if (index)
    error.with_context("rule_index", std::to_string(*index));
  return error;
}

std::filesystem::path normalized_path(std::filesystem::path const& path)
{
  auto normalized = ava::core::normalized_absolute_path(path);
  // lexically_normal preserves a trailing empty component for directory forms
  // such as "/workspace/."; exact permission identity does not.
  while (normalized != normalized.root_path() && normalized.filename().empty())
  {
    normalized = normalized.parent_path();
  }
  return normalized;
}

// Compare resolved inode ancestry plus the still-missing lexical suffix. This
// detects existing directory aliases without canonicalizing or replacing either
// caller-visible path identity, and also protects not-yet-created rule files.
ava::core::Result<bool> paths_refer_to_same_file(std::filesystem::path const& a, std::filesystem::path const& b)
{
  struct Identity
  {
    dev_t device = 0;
    ino_t inode = 0;
    std::vector<std::string> missing_suffix;
  };
  auto const identity = [](std::filesystem::path path) -> ava::core::Result<Identity> {
    if (path.empty())
      return std::unexpected(rule_file_error(ava::core::ErrorCategory::InvalidArgument, "permission path identity requires a non-empty path", path));
    path = normalized_path(path);
    auto const original_path = path;
    std::vector<std::string> suffix;
    for (std::size_t depth = 0; depth < 256; ++depth)
    {
      struct stat status{};
      if (::stat(path.c_str(), &status) == 0)
        return Identity{.device = status.st_dev, .inode = status.st_ino, .missing_suffix = std::move(suffix)};
      int const error_number = errno;
      if (error_number != ENOENT && error_number != ENOTDIR)
      {
        auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to establish permission path identity", original_path);
        error.with_context("identity_component", path.string());
        error.with_context("cause", std::strerror(error_number));
        return std::unexpected(std::move(error));
      }
      auto const parent = path.parent_path();
      if (parent.empty() || parent == path)
      {
        return std::unexpected(rule_file_error(ava::core::ErrorCategory::Io, "failed to find an existing permission path ancestor", original_path));
      }
      suffix.push_back(path.filename().string());
      path = parent;
    }
    return std::unexpected(rule_file_error(ava::core::ErrorCategory::Io, "permission path identity exceeded the component limit", original_path));
  };

  auto const left = identity(a);
  if (!left)
    return std::unexpected(std::move(left.error()));
  auto const right = identity(b);
  if (!right)
    return std::unexpected(std::move(right.error()));
  return left->device == right->device && left->inode == right->inode && left->missing_suffix == right->missing_suffix;
}

bool contains_parent_reference(std::filesystem::path const& path)
{
  for (auto const& part : path.lexically_normal())
  {
    if (part == "..")
      return true;
  }
  return false;
}

std::string workspace_rules_key(std::filesystem::path const& workspace_dir)
{
  auto const normalized = normalized_path(workspace_dir).string();
  std::uint64_t hash = 14695981039346656037ULL;
  for (auto const ch : normalized)
  {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

std::mutex& protected_rule_paths_mutex()
{
  static std::mutex mutex;
  return mutex;
}

std::vector<std::filesystem::path>& protected_rule_paths()
{
  static std::vector<std::filesystem::path> paths;
  return paths;
}

void append_protected_rule_path(std::vector<std::filesystem::path>& paths, std::filesystem::path path)
{
  if (path.empty())
    return;
  path = normalized_path(path);
  if (std::ranges::find(paths, path) == paths.end())
  {
    paths.push_back(std::move(path));
  }
}

std::filesystem::path rules_file_path(PermissionRuleStore const& store, PermissionRuleScope scope)
{
  return enforceable_permission_rules_file(store, scope);
}

ava::core::VoidResult validate_rule_file_path(std::filesystem::path const& path)
{
  if (path.empty() || !path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == "..")
  {
    return std::unexpected(
        rule_file_error(ava::core::ErrorCategory::InvalidArgument, "permission rules file path must be an absolute path with a regular filename", path));
  }
  for (auto const& component : path)
  {
    if (component == "." || component == "..")
    {
      return std::unexpected(
          rule_file_error(ava::core::ErrorCategory::InvalidArgument, "permission rules file path must not contain traversal components", path));
    }
  }
  return {};
}

ava::core::VoidResult validate_rule_ancestor(int fd, std::filesystem::path const& path)
{
  struct stat st{};
  if (::fstat(fd, &st) != 0)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to inspect permission rules path component", path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (!S_ISDIR(st.st_mode))
    return std::unexpected(rule_file_error(ava::core::ErrorCategory::PermissionDenied, "permission rules path component is not a directory", path));
  if (st.st_uid != 0 && st.st_uid != ::geteuid())
    return std::unexpected(
        rule_file_error(ava::core::ErrorCategory::PermissionDenied, "permission rules path component is not owned by root or the current user", path));
  bool const shared_sticky_root_namespace = st.st_uid == 0 && (st.st_mode & S_ISVTX) != 0;
  bool const private_primary_group_directory = ava::command::detail::is_current_user_private_primary_group_directory(st);
  if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 && !shared_sticky_root_namespace && !private_primary_group_directory)
  {
    return std::unexpected(rule_file_error(ava::core::ErrorCategory::PermissionDenied, "permission rules path component is group- or world-writable", path));
  }
  return {};
}

ava::core::VoidResult validate_final_rule_directory(int fd, std::filesystem::path const& path)
{
  if (auto safe = validate_rule_ancestor(fd, path); !safe)
    return safe;
  struct stat st{};
  if (::fstat(fd, &st) != 0)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to inspect permission rules directory", path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (st.st_uid != ::geteuid() || (st.st_mode & 07777) != S_IRWXU)
  {
    auto error =
        rule_file_error(ava::core::ErrorCategory::PermissionDenied, "permission rules directory must be owned by the current user with exact mode 0700", path);
    error.with_context("expected_permissions", "0700");
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::Error rule_directory_open_error(std::filesystem::path const& path, int error_number)
{
  auto const category = error_number == ELOOP || error_number == ENOTDIR ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
  auto error = rule_file_error(category, "failed to securely open permission rules path component", path);
  error.with_context("cause", std::strerror(error_number));
  return error;
}

struct RuleWalkStart
{
  ava::core::UniqueFd fd;
  std::filesystem::path component_path;
  std::filesystem::path relative_parent;

  // Owns descriptor-relative path authority; never generate debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

ava::core::Result<RuleWalkStart> begin_rule_directory_walk(std::filesystem::path const& file_path, std::shared_ptr<ava::core::AnchorSet> const& anchors)
{
  if (anchors)
  {
    auto selected = anchors->find_anchor(file_path);
    if (selected)
    {
      int const duplicate = ::openat(selected->anchor().fd, ".", O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC);
      if (duplicate < 0)
      {
        auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to duplicate permission rules storage anchor", file_path);
        error.with_context("cause", errno_message());
        return std::unexpected(std::move(error));
      }
      ava::core::UniqueFd fd(duplicate);
      if (auto safe = validate_rule_ancestor(fd.get(), selected->anchor().root); !safe)
        return std::unexpected(std::move(safe.error()));
      return RuleWalkStart{.fd = std::move(fd), .component_path = selected->anchor().root, .relative_parent = selected->relative().parent_path()};
    }
  }

  ava::core::UniqueFd fd(::open("/", O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to open permission rules filesystem anchor", file_path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (auto safe = validate_rule_ancestor(fd.get(), "/"); !safe)
    return std::unexpected(std::move(safe.error()));
  return RuleWalkStart{.fd = std::move(fd), .component_path = "/", .relative_parent = file_path.parent_path().relative_path()};
}

ava::core::Result<std::optional<RuleDirectory>> open_rule_directory_for_read(std::filesystem::path const& file_path,
                                                                             std::shared_ptr<ava::core::AnchorSet> const& anchors)
{
  if (auto valid = validate_rule_file_path(file_path); !valid)
    return std::unexpected(std::move(valid.error()));

  auto walk = begin_rule_directory_walk(file_path, anchors);
  if (!walk)
    return std::unexpected(std::move(walk.error()));
  ava::core::UniqueFd current = std::move(walk->fd);
  auto component_path = std::move(walk->component_path);

  auto const parent = file_path.parent_path();
  for (auto const& component : walk->relative_parent)
  {
    auto const name = component.string();
    component_path /= name;
    int const next = ::openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0)
    {
      auto const error_number = errno;
      if (error_number == ENOENT)
        return std::optional<RuleDirectory>{};
      return std::unexpected(rule_directory_open_error(component_path, error_number));
    }
    ava::core::UniqueFd opened(next);
    if (auto safe = validate_rule_ancestor(opened.get(), component_path); !safe)
      return std::unexpected(std::move(safe.error()));
    current = std::move(opened);
  }
  if (auto safe = validate_final_rule_directory(current.get(), parent); !safe)
    return std::unexpected(std::move(safe.error()));
  return std::optional<RuleDirectory>{RuleDirectory{.fd = std::move(current), .path = parent, .file_name = file_path.filename().string()}};
}

ava::core::Result<RuleDirectory> open_rule_directory_for_write(std::filesystem::path const& file_path, std::shared_ptr<ava::core::AnchorSet> const& anchors)
{
  if (auto valid = validate_rule_file_path(file_path); !valid)
    return std::unexpected(std::move(valid.error()));

  auto walk = begin_rule_directory_walk(file_path, anchors);
  if (!walk)
    return std::unexpected(std::move(walk.error()));
  ava::core::UniqueFd current = std::move(walk->fd);
  auto component_path = std::move(walk->component_path);

  auto const parent = file_path.parent_path();
  for (auto const& component : walk->relative_parent)
  {
    auto const name = component.string();
    component_path /= name;
    bool created = false;
    int next = ::openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0 && errno == ENOENT)
    {
      if (::mkdirat(current.get(), name.c_str(), S_IRWXU) == 0)
      {
        created = true;
      }
      else if (errno != EEXIST)
      {
        auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to create permission rules path component", component_path);
        error.with_context("cause", errno_message());
        return std::unexpected(std::move(error));
      }
      next = ::openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    }
    if (next < 0)
      return std::unexpected(rule_directory_open_error(component_path, errno));
    ava::core::UniqueFd opened(next);
    if (auto safe = validate_rule_ancestor(opened.get(), component_path); !safe)
      return std::unexpected(std::move(safe.error()));
    if (created && ::fchmod(opened.get(), S_IRWXU) != 0)
    {
      auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to secure newly created permission rules directory", component_path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    current = std::move(opened);
  }
  if (auto safe = validate_final_rule_directory(current.get(), parent); !safe)
    return std::unexpected(std::move(safe.error()));
  return RuleDirectory{.fd = std::move(current), .path = parent, .file_name = file_path.filename().string()};
}

ava::core::VoidResult validate_unsafe_replace_target(RuleDirectory const& directory)
{
  struct stat st{};
  if (::fstatat(directory.fd.get(), directory.file_name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
  {
    if (errno == ENOENT)
      return {};
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to inspect permission rules file", directory.path / directory.file_name);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 07777) != (S_IRUSR | S_IWUSR))
  {
    return std::unexpected(rule_file_error(ava::core::ErrorCategory::PermissionDenied,
                                           "permission rules file must be a current-user-owned regular file with exact mode 0600",
                                           directory.path / directory.file_name));
  }
  return {};
}

ava::core::Result<RuleLock> acquire_rule_lock(std::filesystem::path const& file_path, std::shared_ptr<ava::core::AnchorSet> const& anchors)
{
  auto directory = open_rule_directory_for_write(file_path, anchors);
  if (!directory)
    return std::unexpected(std::move(directory.error()));

  auto const lock_name = directory->file_name + ".lock";
  ava::core::UniqueFd fd(::openat(directory->fd.get(), lock_name.c_str(), O_RDWR | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
  if (fd.get() < 0)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to open permission rules lock file", directory->path / lock_name);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }

  struct stat opened_st{};
  if (::fstat(fd.get(), &opened_st) != 0)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to inspect permission rules lock file", directory->path / lock_name);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (!S_ISREG(opened_st.st_mode) || opened_st.st_uid != ::geteuid() || (opened_st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
  {
    return std::unexpected(rule_file_error(ava::core::ErrorCategory::PermissionDenied, "permission rules lock file is not a private current-user regular file",
                                           directory->path / lock_name));
  }
  if (::fchmod(fd.get(), S_IRUSR | S_IWUSR) != 0)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to set permission rules lock permissions", directory->path / lock_name);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  while (::flock(fd.get(), LOCK_EX) != 0)
  {
    if (errno == EINTR)
      continue;
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to lock permission rules file", directory->path / lock_name);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  return RuleLock{.directory = std::move(*directory), .fd = std::move(fd)};
}

}  // namespace ava::permissions::permission_rules_internal

namespace ava::permissions {

using namespace permission_rules_internal;

std::filesystem::path enforceable_permission_rules_file(PermissionRuleStore const& store, PermissionRuleScope scope)
{
  if (scope == PermissionRuleScope::Global)
    return store.global_rules_file;

  auto base_dir = store.global_rules_file.parent_path();
  if (base_dir.empty())
    base_dir = store.workspace_rules_file.parent_path();
  return base_dir / "workspace-permission-rules" / workspace_rules_key(store.workspace_dir) / "permission-rules.json";
}

bool is_enforceable_permission_rules_file(PermissionRuleStore const& store, std::filesystem::path const& path)
{
  if (path.empty())
    return false;
  if (!store.global_rules_file.empty())
  {
    auto const global_match = paths_refer_to_same_file(path, enforceable_permission_rules_file(store, PermissionRuleScope::Global));
    if (global_match && *global_match)
      return true;
  }
  if (store.workspace_dir.empty())
    return false;
  auto const workspace_match = paths_refer_to_same_file(path, enforceable_permission_rules_file(store, PermissionRuleScope::Workspace));
  return workspace_match && *workspace_match;
}

void register_enforceable_permission_rule_files(PermissionRuleStore const& store)
{
  std::lock_guard lock(protected_rule_paths_mutex());
  auto& paths = protected_rule_paths();
  if (!store.global_rules_file.empty())
  {
    append_protected_rule_path(paths, enforceable_permission_rules_file(store, PermissionRuleScope::Global));
  }
  if (!store.workspace_dir.empty())
  {
    append_protected_rule_path(paths, enforceable_permission_rules_file(store, PermissionRuleScope::Workspace));
  }
}

bool is_registered_enforceable_permission_rules_file(std::filesystem::path const& path)
{
  if (path.empty())
    return false;
  std::lock_guard lock(protected_rule_paths_mutex());
  auto const& paths = protected_rule_paths();
  return std::ranges::any_of(paths, [&](std::filesystem::path const& registered) {
    auto const matched = paths_refer_to_same_file(path, registered);
    return matched && *matched;
  });
}

}  // namespace ava::permissions
