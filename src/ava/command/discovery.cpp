#include "sys.h"
#include "ava/command/discovery.h"
#include "ava/command/intent_internal.h"
#include "ava/command/private_group.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/fd_io.h"
#include "ava/core/open_beneath.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::command::detail {
namespace {

#ifndef O_PATH
#define O_PATH O_RDONLY
#endif

class UniqueFd final
{
 public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) { }
  ~UniqueFd()
  {
    if (fd_ >= 0)
      ::close(fd_);
  }
  UniqueFd(UniqueFd const&) = delete;
  UniqueFd& operator=(UniqueFd const&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }
  UniqueFd& operator=(UniqueFd&& other) noexcept
  {
    if (this != &other)
    {
      if (fd_ >= 0)
        ::close(fd_);
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  int fd_ = -1;
};

enum class ExpectedNode
{
  Any,
  Directory,
  Regular,
};

bool is_within(std::filesystem::path const& child, std::filesystem::path const& parent)
{
  if (!child.is_absolute() || !parent.is_absolute())
    return false;
  auto const relative = child.lexically_normal().lexically_relative(parent.lexically_normal());
  auto const begin = relative.begin();
  return begin != relative.end() && *begin != "..";
}

bool mode_is_safe(struct stat const& status)
{
  return (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool owner_is_safe(struct stat const& status, bool require_current_user)
{
  if (require_current_user)
    return status.st_uid == geteuid();
  return status.st_uid == geteuid() || status.st_uid == 0;
}

bool is_sticky_directory(struct stat const& status)
{
  return S_ISDIR(status.st_mode) && (status.st_mode & S_ISVTX) != 0;
}

bool directory_mode_is_safe(struct stat const& status, bool allow_private_primary_group_write)
{
  if ((status.st_mode & S_IWOTH) != 0)
    return false;
  if ((status.st_mode & S_IWGRP) == 0)
    return true;
  return allow_private_primary_group_write && is_current_user_private_primary_group_directory(status);
}

bool ancestor_directory_mode_is_safe(struct stat const& status)
{
  if (mode_is_safe(status))
    return true;
  // Root-owned sticky namespaces such as /tmp remain accepted. Their volatile
  // contents are handled separately by the unbound-identity policy below.
  if (is_sticky_directory(status) && status.st_uid == 0)
    return true;
  return directory_mode_is_safe(status, true);
}

ava::core::Result<std::filesystem::path> normalized_absolute(std::filesystem::path const& input, std::string_view label)
{
  if (input.empty() || !input.is_absolute() || has_forbidden_path_byte(input))
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument,
                                         std::string(label) + " must be an absolute logical path without control bytes", "path", input.string()));
  }
  return input.lexically_normal();
}

ava::core::Result<struct stat> metadata_stat(std::filesystem::path const& path, std::string_view label)
{
  // O_PATH|O_NOFOLLOW obtains only metadata and cannot block on a FIFO. It is
  // deliberately used for every untrusted pathname inspection in this module.
  UniqueFd fd(::open(path.c_str(), O_PATH | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (fd.get() < 0)
  {
    auto result = command_error(errno == ENOENT ? ava::core::ErrorCategory::NotFound : ava::core::ErrorCategory::Io, "failed to inspect " + std::string(label),
                                "path", path.string());
    result.with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(result));
  }
  struct stat status{};
  if (::fstat(fd.get(), &status) != 0)
  {
    auto result = command_error(ava::core::ErrorCategory::Io, "failed to read metadata for " + std::string(label), "path", path.string());
    result.with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(result));
  }
  return status;
}

PathAncestorMetadata ancestor_metadata_from(std::filesystem::path path, struct stat const& status)
{
  return PathAncestorMetadata{.path = std::move(path),
                              .device = static_cast<std::uintmax_t>(status.st_dev),
                              .inode = static_cast<std::uintmax_t>(status.st_ino),
                              .mode = static_cast<std::uintmax_t>(status.st_mode),
                              .owner = static_cast<std::uintmax_t>(status.st_uid),
                              .group = static_cast<std::uintmax_t>(status.st_gid),
                              .link_count = static_cast<std::uintmax_t>(status.st_nlink),
                              .changed_seconds = static_cast<std::int64_t>(status.st_ctim.tv_sec),
                              .changed_nanoseconds = static_cast<std::int64_t>(status.st_ctim.tv_nsec),
                              .is_symlink = S_ISLNK(status.st_mode),
                              // Root-owned sticky namespaces such as /tmp are
                              // shared by unrelated processes, so bind their
                              // safety properties but not volatile identity
                              // metadata. Other sticky directories stay bound.
                              .identity_bound = !(is_sticky_directory(status) && status.st_uid == 0)};
}

ava::core::Result<std::vector<PathAncestorMetadata>> capture_ancestor_metadata(std::filesystem::path const& requested, std::filesystem::path const& canonical,
                                                                               std::string_view label)
{
  std::vector<PathAncestorMetadata> result;
  std::set<std::filesystem::path> seen;
  auto const capture_parents = [&result, &seen, label](std::filesystem::path const& path) -> ava::core::VoidResult {
    std::filesystem::path current("/");
    for (auto const& component : path.parent_path().relative_path())
    {
      current /= component;
      if (!seen.insert(current).second)
        continue;
      auto status = metadata_stat(current, label);
      if (!status)
        return std::unexpected(std::move(status.error()));
      result.push_back(ancestor_metadata_from(current, *status));
    }
    return {};
  };
  if (auto captured = capture_parents(requested); !captured)
    return std::unexpected(std::move(captured.error()));
  if (auto captured = capture_parents(canonical); !captured)
    return std::unexpected(std::move(captured.error()));
  return result;
}

PathMetadata metadata_from(std::filesystem::path requested, struct stat const& requested_status, struct stat const& target_status,
                           std::vector<PathAncestorMetadata> ancestors)
{
  auto logical_path = requested;
  return PathMetadata{.requested_path = std::move(requested),
                      .canonical_path = std::move(logical_path),
                      .device = static_cast<std::uintmax_t>(target_status.st_dev),
                      .inode = static_cast<std::uintmax_t>(target_status.st_ino),
                      .mode = static_cast<std::uintmax_t>(target_status.st_mode),
                      .size = static_cast<std::uintmax_t>(target_status.st_size),
                      .owner = static_cast<std::uintmax_t>(target_status.st_uid),
                      .group = static_cast<std::uintmax_t>(target_status.st_gid),
                      .link_count = static_cast<std::uintmax_t>(target_status.st_nlink),
                      .changed_seconds = static_cast<std::int64_t>(target_status.st_ctim.tv_sec),
                      .changed_nanoseconds = static_cast<std::int64_t>(target_status.st_ctim.tv_nsec),
                      .requested_path_is_symlink = S_ISLNK(requested_status.st_mode),
                      .requested_device = static_cast<std::uintmax_t>(requested_status.st_dev),
                      .requested_inode = static_cast<std::uintmax_t>(requested_status.st_ino),
                      .requested_mode = static_cast<std::uintmax_t>(requested_status.st_mode),
                      .requested_owner = static_cast<std::uintmax_t>(requested_status.st_uid),
                      .requested_group = static_cast<std::uintmax_t>(requested_status.st_gid),
                      .requested_link_count = static_cast<std::uintmax_t>(requested_status.st_nlink),
                      .requested_changed_seconds = static_cast<std::int64_t>(requested_status.st_ctim.tv_sec),
                      .requested_changed_nanoseconds = static_cast<std::int64_t>(requested_status.st_ctim.tv_nsec),
                      .ancestor_metadata = std::move(ancestors)};
}

ava::core::Result<struct stat> anchored_final_status(ava::core::AnchorSet::AnchorRef const& anchor, std::string_view label)
{
  if (anchor.relative().empty() || anchor.relative() == ".")
  {
    struct stat status{};
    if (::fstat(anchor.anchor().fd, &status) != 0)
    {
      auto error = command_error(ava::core::ErrorCategory::Io, "failed to inspect " + std::string(label));
      error.with_context("cause", std::strerror(errno));
      return std::unexpected(std::move(error));
    }
    return status;
  }

  int parent_fd = anchor.anchor().fd;
  UniqueFd parent;
  auto const parent_relative = anchor.relative().parent_path();
  if (!parent_relative.empty() && parent_relative != ".")
  {
    int const opened_parent = ava::core::open_beneath(anchor.anchor().fd, parent_relative, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (opened_parent < 0)
    {
      auto error =
          command_error(ava::core::ErrorCategory::PermissionDenied, "failed to inspect descriptor-bound path parent", "path", anchor.absolute().string());
      error.with_context("cause", std::strerror(errno));
      return std::unexpected(std::move(error));
    }
    parent = UniqueFd(opened_parent);
    parent_fd = parent.get();
  }
  struct stat status{};
  auto const filename = anchor.relative().filename().string();
  if (::fstatat(parent_fd, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
  {
    auto error = command_error(errno == ENOENT ? ava::core::ErrorCategory::NotFound : ava::core::ErrorCategory::Io, "failed to inspect " + std::string(label),
                               "path", anchor.absolute().string());
    error.with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(error));
  }
  return status;
}

ava::core::Result<PathMetadata> inspect_path_metadata(std::filesystem::path const& input, ExpectedNode expected, bool reject_final_symlink,
                                                      std::string_view label, ava::core::AnchorSet const* anchors)
{
  auto requested = normalized_absolute(input, label);
  if (!requested)
    return std::unexpected(std::move(requested.error()));

  bool descriptor_anchored = false;
  std::optional<ava::core::AnchorSet::AnchorRef> anchor;
  if (anchors)
  {
    auto found = anchors->find_anchor(*requested);
    if (found)
    {
      descriptor_anchored = true;
      anchor.emplace(*found);
    }
  }

  struct stat requested_status{};
  if (anchor)
  {
    auto status = anchored_final_status(*anchor, label);
    if (!status)
      return std::unexpected(std::move(status.error()));
    requested_status = *status;
  }
  else
  {
    auto status = metadata_stat(*requested, label);
    if (!status)
      return std::unexpected(std::move(status.error()));
    requested_status = *status;
  }
  if (reject_final_symlink && !descriptor_anchored && S_ISLNK(requested_status.st_mode))
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::PermissionDenied, std::string(label) + " must not be a final-component symlink", "path", requested->string()));
  }

  int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
  if (expected == ExpectedNode::Directory)
    flags |= O_DIRECTORY;
  if (!anchors)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command planning requires the shared AnchorSet"));
  auto opened = ava::core::open_readable(*anchors, *requested, flags);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  struct stat target_status{};
  if (::fstat(opened->fd(), &target_status) != 0)
  {
    auto error = command_error(ava::core::ErrorCategory::Io, "failed to read descriptor metadata for " + std::string(label), "path", requested->string());
    error.with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(error));
  }
  if (anchor)
  {
    auto observed = anchored_final_status(*anchor, label);
    if (!observed || observed->st_dev != requested_status.st_dev || observed->st_ino != requested_status.st_ino ||
        observed->st_mode != requested_status.st_mode || observed->st_uid != requested_status.st_uid || observed->st_gid != requested_status.st_gid ||
        observed->st_nlink != requested_status.st_nlink || observed->st_ctim.tv_sec != requested_status.st_ctim.tv_sec ||
        observed->st_ctim.tv_nsec != requested_status.st_ctim.tv_nsec)
    {
      return std::unexpected(
          command_error(ava::core::ErrorCategory::Io, std::string(label) + " changed during descriptor inspection", "path", requested->string()));
    }
  }
  if ((expected == ExpectedNode::Directory && !S_ISDIR(target_status.st_mode)) || (expected == ExpectedNode::Regular && !S_ISREG(target_status.st_mode)))
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::InvalidArgument, std::string(label) + " has an unexpected file type", "path", requested->string()));
  }

  std::vector<PathAncestorMetadata> ancestors;
  if (!descriptor_anchored)
  {
    auto captured = capture_ancestor_metadata(*requested, *requested, "path ancestor");
    if (!captured)
      return std::unexpected(std::move(captured.error()));
    ancestors = std::move(*captured);
  }
  return metadata_from(std::move(*requested), requested_status, target_status, std::move(ancestors));
}

ava::core::VoidResult validate_safe_ancestors(std::vector<PathAncestorMetadata> const& ancestors)
{
  for (auto const& ancestor : ancestors)
  {
    struct stat status{};
    status.st_mode = static_cast<mode_t>(ancestor.mode);
    status.st_uid = static_cast<uid_t>(ancestor.owner);
    status.st_gid = static_cast<gid_t>(ancestor.group);
    status.st_nlink = static_cast<nlink_t>(ancestor.link_count);
    if (ancestor.is_symlink)
    {
      if (!S_ISLNK(status.st_mode) || !owner_is_safe(status, false) || status.st_nlink != 1)
      {
        return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied, "command path symlink ancestor has an unsafe owner or link count",
                                             "path", ancestor.path.string()));
      }
      continue;
    }
    if (!S_ISDIR(status.st_mode))
      return std::unexpected(
          command_error(ava::core::ErrorCategory::PermissionDenied, "command path ancestor is not a directory", "path", ancestor.path.string()));
    if (!owner_is_safe(status, false))
      return std::unexpected(
          command_error(ava::core::ErrorCategory::PermissionDenied, "command path ancestor has an unsafe owner", "path", ancestor.path.string()));
    if (!ancestor_directory_mode_is_safe(status))
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied,
                                           "command path ancestor is group/world writable outside the verified private-primary-group exception", "path",
                                           ancestor.path.string()));
    }
  }
  return {};
}

ava::core::VoidResult validate_safe_final_symlink_identity(std::filesystem::path const& path, bool is_symlink, std::uintmax_t mode, std::uintmax_t owner,
                                                           std::uintmax_t link_count, std::string_view label)
{
  if (!is_symlink)
    return {};
  struct stat status{};
  status.st_mode = static_cast<mode_t>(mode);
  status.st_uid = static_cast<uid_t>(owner);
  status.st_nlink = static_cast<nlink_t>(link_count);
  if (!S_ISLNK(status.st_mode) || !owner_is_safe(status, false) || status.st_nlink != 1)
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::PermissionDenied, std::string(label) + " symlink has an unsafe owner or link count", "path", path.string()));
  }
  return {};
}

ava::core::VoidResult validate_safe_final_symlink(PathMetadata const& metadata, std::string_view label)
{
  return validate_safe_final_symlink_identity(metadata.requested_path, metadata.requested_path_is_symlink, metadata.requested_mode, metadata.requested_owner,
                                              metadata.requested_link_count, label);
}

ava::core::VoidResult validate_safe_directory(PathMetadata const& metadata, bool require_current_user, bool allow_private_primary_group_write,
                                              std::string_view label)
{
  struct stat status{};
  status.st_mode = static_cast<mode_t>(metadata.mode);
  status.st_uid = static_cast<uid_t>(metadata.owner);
  status.st_gid = static_cast<gid_t>(metadata.group);
  if (!S_ISDIR(status.st_mode) || !directory_mode_is_safe(status, allow_private_primary_group_write) || !owner_is_safe(status, require_current_user))
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::PermissionDenied,
                      std::string(label) + " has unsafe ownership or group/world-writable permissions outside the verified private-primary-group exception",
                      "path", metadata.canonical_path.string()));
  }
  if (auto valid = validate_safe_final_symlink(metadata, label); !valid)
    return std::unexpected(std::move(valid.error()));
  return validate_safe_ancestors(metadata.ancestor_metadata);
}

ava::core::VoidResult validate_safe_authority_node(PathMetadata const& metadata)
{
  if (S_ISDIR(static_cast<mode_t>(metadata.mode)))
    return validate_safe_directory(metadata, true, true, "AVA authority root");

  struct stat status{};
  status.st_mode = static_cast<mode_t>(metadata.mode);
  status.st_uid = static_cast<uid_t>(metadata.owner);
  if (!S_ISREG(status.st_mode) || !mode_is_safe(status) || !owner_is_safe(status, true) || metadata.link_count != 1)
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied, "AVA authority node must be a safely owned directory or regular file",
                                         "path", metadata.canonical_path.string()));
  }
  if (auto valid = validate_safe_final_symlink(metadata, "AVA authority node"); !valid)
    return std::unexpected(std::move(valid.error()));
  return validate_safe_ancestors(metadata.ancestor_metadata);
}

ava::core::VoidResult validate_synthetic_environment_directory(PathMetadata const& metadata, std::string_view label)
{
  struct stat status{};
  status.st_mode = static_cast<mode_t>(metadata.mode);
  status.st_uid = static_cast<uid_t>(metadata.owner);
  if (!S_ISDIR(status.st_mode) || !owner_is_safe(status, true) || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied, std::string(label) + " must be an owner-owned 0700-style directory",
                                         "path", metadata.canonical_path.string()));
  }
  return validate_safe_ancestors(metadata.ancestor_metadata);
}

ava::core::VoidResult validate_safe_recipe_path(PathMetadata const& metadata)
{
  struct stat status{};
  status.st_mode = static_cast<mode_t>(metadata.mode);
  status.st_uid = static_cast<uid_t>(metadata.owner);
  status.st_nlink = static_cast<nlink_t>(metadata.link_count);
  if ((!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) || !mode_is_safe(status) || !owner_is_safe(status, false) ||
      (S_ISREG(status.st_mode) && status.st_nlink != 1))
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied,
                                         "recipe path argument has unsafe ownership, mode, link count, or symlink provenance", "path",
                                         metadata.canonical_path.string()));
  }
  return validate_safe_ancestors(metadata.ancestor_metadata);
}

ava::core::VoidResult validate_safe_executable(ExecutableMetadata const& metadata)
{
  struct stat status{};
  status.st_mode = static_cast<mode_t>(metadata.mode);
  status.st_uid = static_cast<uid_t>(metadata.owner);
  if (!S_ISREG(status.st_mode) || (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "resolved command is not a regular executable file", "path",
                                         metadata.canonical_path.string()));
  }
  if (!mode_is_safe(status) || !owner_is_safe(status, false) || metadata.link_count != 1)
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied, "resolved command has unsafe owner, writable mode, or link count", "path",
                                         metadata.canonical_path.string()));
  }
  if (auto valid = validate_safe_final_symlink_identity(metadata.requested_path, metadata.requested_path_is_symlink, metadata.requested_mode,
                                                        metadata.requested_owner, metadata.requested_link_count, "executable");
      !valid)
  {
    return std::unexpected(std::move(valid.error()));
  }
  return validate_safe_ancestors(metadata.ancestor_metadata);
}

ExecutableMetadata executable_from(PathMetadata const& metadata)
{
  return ExecutableMetadata{.requested_path = metadata.requested_path,
                            .canonical_path = metadata.canonical_path,
                            .device = metadata.device,
                            .inode = metadata.inode,
                            .mode = metadata.mode,
                            .size = metadata.size,
                            .owner = metadata.owner,
                            .group = metadata.group,
                            .link_count = metadata.link_count,
                            .changed_seconds = metadata.changed_seconds,
                            .changed_nanoseconds = metadata.changed_nanoseconds,
                            .requested_path_is_symlink = metadata.requested_path_is_symlink,
                            .requested_device = metadata.requested_device,
                            .requested_inode = metadata.requested_inode,
                            .requested_mode = metadata.requested_mode,
                            .requested_owner = metadata.requested_owner,
                            .requested_group = metadata.requested_group,
                            .requested_link_count = metadata.requested_link_count,
                            .requested_changed_seconds = metadata.requested_changed_seconds,
                            .requested_changed_nanoseconds = metadata.requested_changed_nanoseconds,
                            .ancestor_metadata = metadata.ancestor_metadata};
}

ava::core::Result<std::filesystem::path> descriptor_executable_path(int fd, std::filesystem::path const& logical)
{
  auto physical = ava::core::descriptor_physical_path_checked(fd);
  if (physical)
    return physical->lexically_normal();
  switch (physical.error().failure)
  {
    case ava::core::DescriptorPathFailure::Readlink:
      break;
    case ava::core::DescriptorPathFailure::Deleted:
      return std::unexpected(command_error(ava::core::ErrorCategory::Io, "executable changed during descriptor identity capture", "path", logical.string()));
    case ava::core::DescriptorPathFailure::NotAbsolute:
      return std::unexpected(command_error(ava::core::ErrorCategory::Io, "executable descriptor identity is not absolute", "path", logical.string()));
  }
  auto error = command_error(ava::core::ErrorCategory::Io, "failed to derive narrow executable identity from its descriptor", "path", logical.string());
  error.with_context("cause", std::strerror(physical.error().error_number));
  return std::unexpected(std::move(error));
}

ava::core::Result<ExecutableMetadata> executable_metadata(std::filesystem::path const& requested, ava::core::AnchorSet const& anchors)
{
  auto metadata = inspect_path_metadata(requested, ExpectedNode::Regular, false, "executable", &anchors);
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));
  auto opened = ava::core::open_readable(anchors, metadata->requested_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  struct stat opened_status{};
  if (::fstat(opened->fd(), &opened_status) != 0 || static_cast<std::uintmax_t>(opened_status.st_dev) != metadata->device ||
      static_cast<std::uintmax_t>(opened_status.st_ino) != metadata->inode ||
      static_cast<std::int64_t>(opened_status.st_ctim.tv_sec) != metadata->changed_seconds ||
      static_cast<std::int64_t>(opened_status.st_ctim.tv_nsec) != metadata->changed_nanoseconds)
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::Io, "executable changed during descriptor identity capture", "path", metadata->requested_path.string()));
  }
  auto physical = descriptor_executable_path(opened->fd(), metadata->requested_path);
  if (!physical)
    return std::unexpected(std::move(physical.error()));
  auto executable = executable_from(*metadata);
  executable.canonical_path = std::move(*physical);
  if (auto valid = validate_safe_executable(executable); !valid)
    return std::unexpected(std::move(valid.error()));
  return executable;
}

std::vector<std::string_view> split_path(std::string_view path)
{
  std::vector<std::string_view> entries;
  std::size_t start = 0;
  while (true)
  {
    auto const separator = path.find(':', start);
    entries.push_back(path.substr(start, separator == std::string_view::npos ? std::string_view::npos : separator - start));
    if (separator == std::string_view::npos)
      break;
    start = separator + 1;
  }
  return entries;
}

ava::core::Result<CommandPathEntry> safe_path_directory(std::filesystem::path const& requested, PathProvenance provenance, ava::core::AnchorSet const& anchors)
{
  if (requested.empty() || !requested.is_absolute())
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::InvalidArgument, "PATH entry must be an absolute non-empty directory", "path", requested.string()));
  }
  if (has_forbidden_path_byte(requested) || requested.string().find(':') != std::string::npos)
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "PATH entry contains unsafe bytes", "path", requested.string()));
  }
  bool const startup = provenance == PathProvenance::StartupPath;
  auto metadata = inspect_path_metadata(requested, ExpectedNode::Directory, !startup, "PATH entry", &anchors);
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));
  bool const require_current_user = !startup;
  if (auto valid = validate_safe_directory(*metadata, require_current_user, true, "PATH directory"); !valid)
    return std::unexpected(std::move(valid.error()));
  return CommandPathEntry{.directory = metadata->canonical_path, .provenance = provenance, .metadata = std::move(*metadata)};
}

void append_unique_path(std::vector<CommandPathEntry>& entries, CommandPathEntry entry)
{
  auto const found = std::ranges::find_if(entries, [&entry](CommandPathEntry const& existing) { return existing.directory == entry.directory; });
  if (found == entries.end())
    entries.push_back(std::move(entry));
}

ava::core::Result<std::vector<CommandPathEntry>> discover_path(CommandBuildOptions const& options, std::filesystem::path const& workspace,
                                                               PathMetadata const& trusted_home_metadata, ava::core::AnchorSet const& anchors)
{
  auto const& trusted_home = trusted_home_metadata.canonical_path;
  std::optional<std::string> startup_path = options.startup_path;
  if (!startup_path)
  {
    if (char const* current = std::getenv("PATH"))
      startup_path = current;
  }
  if (startup_path && startup_path->size() > options.limits.max_path_bytes)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "startup PATH exceeds the bounded input size"));
  if (startup_path && has_forbidden_byte(*startup_path))
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "startup PATH contains a forbidden control byte"));

  std::vector<CommandPathEntry> entries;
  if (startup_path)
  {
    auto const startup_entries = split_path(*startup_path);
    if (startup_entries.size() > options.limits.max_path_entries)
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "startup PATH has too many entries"));
    for (auto const entry : startup_entries)
    {
      if (entry.empty())
        return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "startup PATH contains an empty entry"));
      auto safe = safe_path_directory(std::filesystem::path(entry), PathProvenance::StartupPath, anchors);
      // A startup PATH commonly contains optional tool locations. Keep
      // only directories that pass the sealed-path checks. Missing, symlinked,
      // or writable absolute entries add no executable authority and are
      // omitted; empty or relative entries are ambiguous and fail closed.
      if (!safe)
      {
        if (safe.error().category() == ava::core::ErrorCategory::InvalidArgument)
          return std::unexpected(std::move(safe.error()));
        continue;
      }
      // Startup PATH is not authority to expose the real home (or all of /)
      // as a read/execute toolchain root in the containment plan. Compare the
      // sealed target identity too, so a symlink alias cannot expose the home.
      if (safe->directory == "/" || is_within(trusted_home, safe->directory) ||
          (safe->metadata.device == trusted_home_metadata.device && safe->metadata.inode == trusted_home_metadata.inode))
        continue;
      append_unique_path(entries, std::move(*safe));
    }
  }

  auto const add_optional_candidate = [&entries, &anchors](std::filesystem::path const& candidate, PathProvenance provenance) {
    auto safe = safe_path_directory(candidate, provenance, anchors);
    if (safe)
      append_unique_path(entries, std::move(*safe));
  };
  // Host discovery uses only this trusted root; these paths never flow into the
  // child HOME/XDG/TMP environment.
  if (options.discover_host_user_tools)
  {
    add_optional_candidate(trusted_home / ".local" / "bin", PathProvenance::UserLocal);
  }
  add_optional_candidate(workspace / ".venv" / "bin", PathProvenance::WorkspaceVenv);
  add_optional_candidate(workspace / "node_modules" / ".bin", PathProvenance::WorkspaceNodeModules);

  if (entries.size() > options.limits.max_path_entries)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "derived PATH has too many safe entries"));
  std::size_t total_bytes = 0;
  for (auto const& entry : entries)
  {
    auto const bytes = entry.directory.string().size();
    if (total_bytes > options.limits.max_path_bytes || bytes > options.limits.max_path_bytes - total_bytes)
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "derived PATH exceeds the bounded size"));
    total_bytes += bytes;
  }
  return entries;
}

ExecutableOrigin executable_origin(ExecutableMetadata const& executable, std::filesystem::path const& workspace, std::filesystem::path const& trusted_home)
{
  if (is_within(executable.requested_path, workspace))
    return ExecutableOrigin::Workspace;
  if (is_within(executable.requested_path, trusted_home))
    return ExecutableOrigin::User;
  // Startup PATH is host input, not a declaration that an executable is
  // system-managed. Any user-owned executable, final symlink, or ancestor
  // remains mutable user code even when it lives outside HOME/workspace.
  if (executable.owner != 0 || (executable.requested_path_is_symlink && executable.requested_owner != 0) ||
      std::ranges::any_of(executable.ancestor_metadata, [](PathAncestorMetadata const& ancestor) { return ancestor.identity_bound && ancestor.owner != 0; }))
  {
    return ExecutableOrigin::User;
  }
  return ExecutableOrigin::System;
}

struct ShebangParse
{
  std::filesystem::path interpreter_path;
  std::string argument;
  bool is_env = false;
};

ava::core::Result<std::optional<ShebangParse>> shebang_interpreter_path(ExecutableMetadata const& executable, CommandLimits const& limits,
                                                                        ava::core::AnchorSet const& anchors)
{
  // Every attacker-influenced spelling is resolved through the shared anchor
  // set; canonical_path remains display-free executable identity only.
  auto opened = ava::core::open_readable(anchors, executable.requested_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  struct stat status{};
  if (::fstat(opened->fd(), &status) != 0 || !S_ISREG(status.st_mode) || static_cast<std::uintmax_t>(status.st_dev) != executable.device ||
      static_cast<std::uintmax_t>(status.st_ino) != executable.inode || static_cast<std::int64_t>(status.st_ctim.tv_sec) != executable.changed_seconds ||
      static_cast<std::int64_t>(status.st_ctim.tv_nsec) != executable.changed_nanoseconds)
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::Io, "executable changed during shebang inspection", "path", executable.canonical_path.string()));
  }

  // Linux examines a fixed 256-byte buffer for script shebangs. Do not bind
  // bytes the kernel will truncate differently. A caller may impose a lower
  // planning bound, but never a larger effective shebang window.
  constexpr std::size_t kLinuxShebangBufferBytes = 256;
  auto const read_limit = std::min(limits.max_shebang_bytes + 1, kLinuxShebangBufferBytes);
  std::string line(read_limit, '\0');
  ssize_t const read_count = ::read(opened->fd(), line.data(), line.size());
  if (read_count < 0)
  {
    auto result = command_error(ava::core::ErrorCategory::Io, "failed to read executable shebang", "path", executable.canonical_path.string());
    result.with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(result));
  }
  line.resize(static_cast<std::size_t>(read_count));
  if (!line.starts_with("#!"))
    return std::optional<ShebangParse>{};
  auto const newline = line.find('\n');
  if (newline == std::string::npos && line.size() == read_limit)
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument,
                                         "shebang line exceeds the sealed Linux interpreter buffer; shorten the shebang before execution", "path",
                                         executable.canonical_path.string()));
  }
  auto body = std::string_view(line).substr(2, newline == std::string_view::npos ? std::string_view::npos : newline - 2);
  auto const is_separator = [](char ch) { return ch == ' ' || ch == '\t'; };
  std::size_t first = 0;
  while (first < body.size() && is_separator(body[first])) ++first;
  std::size_t end = first;
  while (end < body.size() && !is_separator(body[end])) ++end;
  auto const interpreter = body.substr(first, end - first);
  while (end < body.size() && is_separator(body[end])) ++end;
  // Linux passes at most one argument from the shebang to the interpreter;
  // everything after the first whitespace-delimited interpreter token up to
  // the newline is that single argument.
  auto const argument = body.substr(end);
  if (interpreter.empty() || has_forbidden_byte(interpreter) || !std::filesystem::path(interpreter).is_absolute())
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::InvalidArgument, "shebang must name an absolute interpreter path", "path", executable.canonical_path.string()));
  }
  ShebangParse parse{.interpreter_path = std::filesystem::path(interpreter), .argument = std::string(argument), .is_env = interpreter == "/usr/bin/env"};
  return parse;
}

std::optional<ExecutableMetadata> resolve_env_name(std::string_view name, std::vector<CommandPathEntry> const& path_entries,
                                                   ava::core::AnchorSet const& anchors)
{
  if (name.empty() || name.find('/') != std::string_view::npos || has_forbidden_byte(name))
    return std::nullopt;
  for (auto const& entry : path_entries)
  {
    auto const candidate = entry.directory / name;
    std::error_code exists_error;
    bool const exists = std::filesystem::exists(candidate, exists_error);
    if (!exists && !exists_error)
      continue;
    if (exists_error)
      continue;
    auto metadata = executable_metadata(candidate, anchors);
    if (metadata)
      return *metadata;
    // /usr/bin/env uses the first matching PATH candidate. If that existing
    // candidate cannot be sealed under AVA's executable policy, do not bind a
    // later candidate that the kernel/env chain will never select.
    return std::nullopt;
  }
  return std::nullopt;
}

ava::core::Result<std::pair<std::vector<ShebangInterpreter>, bool>> inspect_shebang_chain(ExecutableMetadata const& executable, CommandLimits const& limits,
                                                                                          std::vector<CommandPathEntry> const& path_entries,
                                                                                          ava::core::AnchorSet const& anchors)
{
  std::vector<ShebangInterpreter> interpreters;
  std::set<std::filesystem::path> seen;
  seen.insert(executable.canonical_path);
  auto current = executable;
  bool fully_resolved = true;
  while (true)
  {
    auto parsed = shebang_interpreter_path(current, limits, anchors);
    if (!parsed)
      return std::unexpected(std::move(parsed.error()));
    if (!*parsed)
      return std::make_pair(std::move(interpreters), fully_resolved);
    if (interpreters.size() >= limits.max_shebang_depth)
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "shebang interpreter chain exceeds the bounded depth", "path",
                                           executable.canonical_path.string()));
    }
    auto const& parse = **parsed;
    if (parse.is_env)
    {
      // #!/usr/bin/env <name> resolves the single name through the sealed
      // PATH. A missing or multi-token argument is safely representable as
      // a one-shot critical prompt but cannot be fully bound.
      auto env_meta = executable_metadata(parse.interpreter_path, anchors);
      if (!env_meta)
        return std::unexpected(std::move(env_meta.error()));
      if (!seen.insert(env_meta->canonical_path).second)
      {
        return std::unexpected(
            command_error(ava::core::ErrorCategory::InvalidArgument, "shebang interpreter chain contains a cycle", "path", env_meta->canonical_path.string()));
      }
      interpreters.push_back(ShebangInterpreter{.interpreter = *env_meta, .argument = parse.argument, .resolved_via_env = false});
      // Determine whether the env argument is a single resolvable name.
      auto const is_separator = [](char ch) { return ch == ' ' || ch == '\t'; };
      std::size_t token_start = 0;
      while (token_start < parse.argument.size() && is_separator(parse.argument[token_start])) ++token_start;
      std::size_t token_end = token_start;
      while (token_end < parse.argument.size() && !is_separator(parse.argument[token_end])) ++token_end;
      std::size_t rest = token_end;
      while (rest < parse.argument.size() && is_separator(parse.argument[rest])) ++rest;
      if (token_end == token_start || rest != parse.argument.size())
      {
        // Empty argument or trailing tokens: not a simple single-name env
        // invocation. The plan still succeeds with a one-shot critical prompt.
        fully_resolved = false;
        return std::make_pair(std::move(interpreters), fully_resolved);
      }
      auto const name = std::string_view(parse.argument).substr(token_start, token_end - token_start);
      auto resolved = resolve_env_name(name, path_entries, anchors);
      if (!resolved)
      {
        fully_resolved = false;
        return std::make_pair(std::move(interpreters), fully_resolved);
      }
      if (!seen.insert(resolved->canonical_path).second)
      {
        return std::unexpected(
            command_error(ava::core::ErrorCategory::InvalidArgument, "shebang interpreter chain contains a cycle", "path", resolved->canonical_path.string()));
      }
      interpreters.push_back(ShebangInterpreter{.interpreter = *resolved, .argument = {}, .resolved_via_env = true});
      current = std::move(*resolved);
      continue;
    }
    auto interpreter = executable_metadata(parse.interpreter_path, anchors);
    if (!interpreter)
      return std::unexpected(std::move(interpreter.error()));
    if (!seen.insert(interpreter->canonical_path).second)
    {
      return std::unexpected(
          command_error(ava::core::ErrorCategory::InvalidArgument, "shebang interpreter chain contains a cycle", "path", interpreter->canonical_path.string()));
    }
    interpreters.push_back(ShebangInterpreter{.interpreter = *interpreter, .argument = parse.argument, .resolved_via_env = false});
    current = std::move(*interpreter);
  }
}

}  // namespace

ava::core::Result<SealedCommandContext> discover_command_context(CommandIntent const& intent, CommandBuildOptions const& options)
{
  if (auto valid = validate_limits(options.limits); !valid)
    return std::unexpected(std::move(valid.error()));
  if (!options.anchor_set)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command planning requires the shared AnchorSet"));
  auto workspace = normalized_absolute(options.workspace, "command workspace");
  if (!workspace)
    return std::unexpected(std::move(workspace.error()));
  if (*workspace != options.anchor_set->launch_workspace_root())
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument,
                                         "command workspace must exactly match the shared AnchorSet launch workspace", "workspace", workspace->string()));
  }
  auto workspace_metadata = inspect_path_metadata(*workspace, ExpectedNode::Directory, false, "command workspace", options.anchor_set.get());
  if (!workspace_metadata)
    return std::unexpected(std::move(workspace_metadata.error()));
  if (auto valid = validate_safe_directory(*workspace_metadata, false, true, "command workspace"); !valid)
    return std::unexpected(std::move(valid.error()));

  PathMetadata cwd_metadata = *workspace_metadata;
  if (intent.requested_cwd())
  {
    auto candidate = intent.requested_cwd()->is_absolute() ? *intent.requested_cwd() : workspace_metadata->canonical_path / *intent.requested_cwd();
    auto captured = inspect_path_metadata(candidate.lexically_normal(), ExpectedNode::Directory, false, "command cwd", options.anchor_set.get());
    if (!captured)
      return std::unexpected(std::move(captured.error()));
    auto cwd_anchor = options.anchor_set->find_anchor(captured->requested_path);
    if (!is_within(captured->canonical_path, workspace_metadata->canonical_path) || !cwd_anchor ||
        cwd_anchor->anchor().root != options.anchor_set->launch_workspace_root())
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied, "command cwd must remain inside the descriptor-bound logical workspace",
                                           "cwd", captured->canonical_path.string()));
    }
    if (auto valid = validate_safe_directory(*captured, false, true, "command cwd"); !valid)
      return std::unexpected(std::move(valid.error()));
    cwd_metadata = std::move(*captured);
  }

  auto trusted_home = inspect_path_metadata(options.trusted_home, ExpectedNode::Directory, true, "trusted command discovery home", options.anchor_set.get());
  if (!trusted_home)
    return std::unexpected(std::move(trusted_home.error()));
  if (auto valid = validate_safe_directory(*trusted_home, true, false, "trusted command discovery home"); !valid)
    return std::unexpected(std::move(valid.error()));
  // Reject a workspace that equals or is an ancestor of the trusted real
  // home. Ordinary projects nested under home are allowed, but making the
  // entire home the workspace would expose personal directories to contained
  // mutable commands and bypass the synthetic-environment boundary.
  if (is_within(trusted_home->canonical_path, workspace_metadata->canonical_path))
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied,
                                         "command workspace must not equal or contain the trusted real home directory", "workspace",
                                         workspace_metadata->canonical_path.string()));
  }
  if (options.ava_authority_roots.size() > options.limits.max_path_entries)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "AVA authority root list has too many entries"));
  std::vector<std::filesystem::path> ava_authority_paths;
  std::vector<PathMetadata> ava_authority_roots;
  ava_authority_paths.reserve(options.ava_authority_roots.size());
  ava_authority_roots.reserve(options.ava_authority_roots.size());
  for (auto const& root : options.ava_authority_roots)
  {
    auto normalized = normalized_absolute(root, "AVA authority root");
    if (!normalized)
      return std::unexpected(std::move(normalized.error()));
    // Even an authority directory that does not exist yet reserves its
    // intended location. A mutable workspace must not be able to create AVA's
    // future configuration or state boundary for itself.
    if (is_within(*normalized, workspace_metadata->canonical_path) || is_within(workspace_metadata->canonical_path, *normalized))
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::PermissionDenied, "command workspace must not overlap with any AVA authority root",
                                           "workspace", workspace_metadata->canonical_path.string()));
    }
    ava_authority_paths.push_back(*normalized);

    std::error_code status_error;
    auto const status = std::filesystem::symlink_status(*normalized, status_error);
    if (status.type() == std::filesystem::file_type::not_found)
      continue;
    if (status_error)
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::Io, "failed to inspect AVA authority root", "path", normalized->string()));
    }
    auto authority = inspect_path_metadata(*normalized, ExpectedNode::Any, true, "AVA authority root", options.anchor_set.get());
    if (!authority)
      return std::unexpected(std::move(authority.error()));
    if (auto valid = validate_safe_authority_node(*authority); !valid)
      return std::unexpected(std::move(valid.error()));
    ava_authority_roots.push_back(std::move(*authority));
  }

  auto const overlaps_synthetic_host_root = [&workspace_metadata, &trusted_home](std::filesystem::path const& candidate) {
    auto const overlaps = [&candidate](std::filesystem::path const& protected_root) {
      return is_within(candidate, protected_root) || is_within(protected_root, candidate);
    };
    // Synthetic roots live under the pre-opened spill anchor, which can itself
    // be nested beneath a broader AVA session authority root. Granting the
    // exact synthetic descriptor does not expose that parent authority.
    return overlaps(workspace_metadata->canonical_path) || overlaps(trusted_home->canonical_path);
  };
  auto const capture_synthetic_root = [&options, &overlaps_synthetic_host_root](std::filesystem::path const& path,
                                                                                std::string_view name) -> ava::core::Result<PathMetadata> {
    auto synthetic = normalized_absolute(path, name);
    if (!synthetic)
      return std::unexpected(std::move(synthetic.error()));
    if (synthetic->string().size() > options.limits.max_path_bytes)
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument,
                                           std::string(name) + " exceeds the bounded synthetic environment path size", "path", synthetic->string()));
    }
    auto selected_anchor = options.anchor_set->find_anchor(*synthetic);
    if (!selected_anchor || selected_anchor->anchor().root == options.anchor_set->launch_workspace_root())
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument,
                                           std::string(name) + " must be beneath a pre-opened non-workspace writable anchor", "path", synthetic->string()));
    }
    auto metadata = inspect_path_metadata(*synthetic, ExpectedNode::Directory, false, name, options.anchor_set.get());
    if (!metadata)
      return std::unexpected(std::move(metadata.error()));
    if (auto valid = validate_synthetic_environment_directory(*metadata, name); !valid)
      return std::unexpected(std::move(valid.error()));
    if (overlaps_synthetic_host_root(metadata->canonical_path))
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument,
                                           std::string(name) + " must be disjoint from workspace and trusted AVA host roots", "path",
                                           metadata->canonical_path.string()));
    }
    return metadata;
  };
  auto home = capture_synthetic_root(options.environment.home, "HOME");
  if (!home)
    return std::unexpected(std::move(home.error()));
  auto xdg_config_home = capture_synthetic_root(options.environment.xdg_config_home, "XDG_CONFIG_HOME");
  if (!xdg_config_home)
    return std::unexpected(std::move(xdg_config_home.error()));
  auto xdg_cache_home = capture_synthetic_root(options.environment.xdg_cache_home, "XDG_CACHE_HOME");
  if (!xdg_cache_home)
    return std::unexpected(std::move(xdg_cache_home.error()));
  auto xdg_data_home = capture_synthetic_root(options.environment.xdg_data_home, "XDG_DATA_HOME");
  if (!xdg_data_home)
    return std::unexpected(std::move(xdg_data_home.error()));
  auto xdg_state_home = capture_synthetic_root(options.environment.xdg_state_home, "XDG_STATE_HOME");
  if (!xdg_state_home)
    return std::unexpected(std::move(xdg_state_home.error()));
  auto tmpdir = capture_synthetic_root(options.environment.tmpdir, "TMPDIR");
  if (!tmpdir)
    return std::unexpected(std::move(tmpdir.error()));
  SyntheticEnvironmentRoots synthetic_environment_roots{.home = std::move(*home),
                                                        .xdg_config_home = std::move(*xdg_config_home),
                                                        .xdg_cache_home = std::move(*xdg_cache_home),
                                                        .xdg_data_home = std::move(*xdg_data_home),
                                                        .xdg_state_home = std::move(*xdg_state_home),
                                                        .tmpdir = std::move(*tmpdir)};

  auto entries = discover_path(options, workspace_metadata->canonical_path, *trusted_home, *options.anchor_set);
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  for (auto const& entry : *entries)
  {
    bool const overlaps_authority = std::ranges::any_of(ava_authority_paths, [&entry](std::filesystem::path const& authority) {
      return is_within(entry.directory, authority) || is_within(authority, entry.directory);
    });
    if (overlaps_authority)
    {
      return std::unexpected(
          command_error(ava::core::ErrorCategory::PermissionDenied, "sealed command PATH entries must remain disjoint from AVA authority roots"));
    }
  }
  return SealedCommandContext{.workspace = workspace_metadata->canonical_path,
                              .cwd = cwd_metadata.canonical_path,
                              .workspace_metadata = std::move(*workspace_metadata),
                              .cwd_metadata = std::move(cwd_metadata),
                              .trusted_home_metadata = std::move(*trusted_home),
                              .ava_authority_roots = std::move(ava_authority_paths),
                              .ava_authority_root_metadata = std::move(ava_authority_roots),
                              .synthetic_environment_roots = std::move(synthetic_environment_roots),
                              .path_entries = std::move(*entries)};
}

ava::core::Result<ResolvedExecutable> resolve_executable(std::vector<std::string> const& argv, std::vector<CommandPathEntry> const& path_entries,
                                                         std::filesystem::path const& cwd, std::filesystem::path const& workspace,
                                                         std::filesystem::path const& trusted_home, CommandLimits const& limits,
                                                         std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  if (argv.empty() || argv.front().empty())
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command argv has no executable identity"));

  if (!anchor_set)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "executable resolution requires the shared AnchorSet"));
  std::filesystem::path requested(argv.front());
  ava::core::Result<ExecutableMetadata> metadata = std::unexpected(command_error(ava::core::ErrorCategory::NotFound, "executable was not found"));
  if (requested.is_absolute() || requested.string().find('/') != std::string::npos)
  {
    metadata = executable_metadata((requested.is_absolute() ? requested : cwd / requested).lexically_normal(), *anchor_set);
  }
  else
  {
    for (auto const& entry : path_entries)
    {
      auto const candidate = entry.directory / requested;
      std::error_code exists_error;
      bool const exists = std::filesystem::exists(candidate, exists_error);
      if (!exists && !exists_error)
        continue;
      if (exists_error)
      {
        auto error = command_error(ava::core::ErrorCategory::Io, "failed to inspect executable candidate", "path", candidate.string());
        error.with_context("cause", exists_error.message());
        return std::unexpected(std::move(error));
      }
      metadata = executable_metadata(candidate, *anchor_set);
      break;
    }
  }
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));

  auto interpreters = inspect_shebang_chain(*metadata, limits, path_entries, *anchor_set);
  if (!interpreters)
    return std::unexpected(std::move(interpreters.error()));
  auto origin = executable_origin(*metadata, workspace, trusted_home);
  for (auto const& interpreter : interpreters->first)
  {
    auto const interpreter_origin = executable_origin(interpreter.interpreter, workspace, trusted_home);
    if (interpreter_origin == ExecutableOrigin::Workspace)
      origin = ExecutableOrigin::Workspace;
    else if (interpreter_origin == ExecutableOrigin::User && origin == ExecutableOrigin::System)
      origin = ExecutableOrigin::User;
  }
  return ResolvedExecutable{
      .executable = *metadata, .origin = origin, .shebang_interpreters = std::move(interpreters->first), .shebang_fully_resolved = interpreters->second};
}

std::optional<PathMetadata> seal_recipe_path_argument(std::string_view value, std::filesystem::path const& cwd, std::filesystem::path const& workspace,
                                                      std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  if (value.empty() || has_forbidden_byte(value))
    return std::nullopt;
  std::filesystem::path raw(value);
  auto candidate = raw.is_absolute() ? raw : cwd / raw;
  auto normalized = normalized_absolute(candidate, "recipe path argument");
  if (!normalized || !is_within(*normalized, workspace))
    return std::nullopt;
  if (!anchor_set)
    return std::nullopt;
  auto selected_anchor = anchor_set->find_anchor(*normalized);
  if (!selected_anchor || selected_anchor->anchor().root != anchor_set->launch_workspace_root())
    return std::nullopt;
  auto metadata = inspect_path_metadata(*normalized, ExpectedNode::Any, false, "recipe path argument", anchor_set.get());
  if (!metadata || !is_within(metadata->canonical_path, workspace))
    return std::nullopt;
  // Logical aliases remain public identity. open_beneath rejects any symlink
  // target that escapes the selected workspace anchor; freshness binds the
  // exact target inode without rewriting the logical path.
  if (auto valid = validate_safe_recipe_path(*metadata); !valid)
    return std::nullopt;
  return std::move(*metadata);
}

bool root_sticky_ancestor_is_fresh(PathAncestorMetadata const& expected, PathAncestorMetadata const& observed)
{
  struct stat status{};
  status.st_mode = static_cast<mode_t>(observed.mode);
  status.st_uid = static_cast<uid_t>(observed.owner);
  status.st_gid = static_cast<gid_t>(observed.group);
  return expected.group == observed.group && S_ISDIR(status.st_mode) && status.st_uid == 0 && is_sticky_directory(status);
}

bool ancestors_are_fresh(std::vector<PathAncestorMetadata> const& recorded, std::vector<PathAncestorMetadata> const& current)
{
  if (recorded.size() != current.size())
    return false;
  for (std::size_t index = 0; index < recorded.size(); ++index)
  {
    auto const& expected = recorded[index];
    auto const& observed = current[index];
    if (expected.path != observed.path || expected.is_symlink != observed.is_symlink || expected.identity_bound != observed.identity_bound)
      return false;
    if (expected.identity_bound)
    {
      if (expected != observed)
        return false;
      continue;
    }
    if (!root_sticky_ancestor_is_fresh(expected, observed))
      return false;
  }
  return true;
}

bool trusted_home_matches_ancestor(PathMetadata const& trusted_home, PathAncestorMetadata const& ancestor)
{
  return trusted_home.requested_path == trusted_home.canonical_path && trusted_home.requested_path == ancestor.path &&
         !trusted_home.requested_path_is_symlink && !ancestor.is_symlink && ancestor.identity_bound && S_ISDIR(static_cast<mode_t>(trusted_home.mode)) &&
         trusted_home.device == trusted_home.requested_device && trusted_home.device == ancestor.device && trusted_home.inode == trusted_home.requested_inode &&
         trusted_home.inode == ancestor.inode && trusted_home.mode == trusted_home.requested_mode && trusted_home.mode == ancestor.mode &&
         trusted_home.owner == trusted_home.requested_owner && trusted_home.owner == ancestor.owner && trusted_home.group == trusted_home.requested_group &&
         trusted_home.group == ancestor.group;
}

bool user_tool_ancestors_are_fresh(std::vector<PathAncestorMetadata> const& recorded, std::vector<PathAncestorMetadata> const& current,
                                   PathMetadata const& trusted_home)
{
  if (recorded.size() != current.size())
    return false;
  bool relaxed_trusted_home = false;
  for (std::size_t index = 0; index < recorded.size(); ++index)
  {
    auto const& expected = recorded[index];
    auto const& observed = current[index];
    if (expected.path != observed.path || expected.is_symlink != observed.is_symlink || expected.identity_bound != observed.identity_bound)
      return false;
    if (expected.path == trusted_home.requested_path)
    {
      if (relaxed_trusted_home || !trusted_home_matches_ancestor(trusted_home, expected) || expected.device != observed.device ||
          expected.inode != observed.inode || expected.mode != observed.mode || expected.owner != observed.owner || expected.group != observed.group ||
          !S_ISDIR(static_cast<mode_t>(observed.mode)))
      {
        return false;
      }
      relaxed_trusted_home = true;
      continue;
    }
    if (expected.identity_bound)
    {
      if (expected != observed)
        return false;
      continue;
    }
    if (!root_sticky_ancestor_is_fresh(expected, observed))
      return false;
  }
  return relaxed_trusted_home;
}

bool trusted_home_directory_identity_is_fresh(PathMetadata const& recorded, PathMetadata const& current)
{
  if (recorded.requested_path != current.requested_path || recorded.canonical_path != current.canonical_path || recorded.device != current.device ||
      recorded.inode != current.inode || recorded.mode != current.mode || recorded.owner != current.owner || recorded.group != current.group ||
      recorded.requested_path_is_symlink != current.requested_path_is_symlink || recorded.requested_device != current.requested_device ||
      recorded.requested_inode != current.requested_inode || recorded.requested_mode != current.requested_mode ||
      recorded.requested_owner != current.requested_owner || recorded.requested_group != current.requested_group)
  {
    return false;
  }
  // A requested symlink is a distinct namespace boundary whose complete
  // identity remains strict. Trusted-home discovery currently rejects this,
  // but keeping the comparison explicit prevents a future anchored caller from
  // silently weakening symlink retarget detection.
  if (recorded.requested_path_is_symlink &&
      (recorded.requested_link_count != current.requested_link_count || recorded.requested_changed_seconds != current.requested_changed_seconds ||
       recorded.requested_changed_nanoseconds != current.requested_changed_nanoseconds))
  {
    return false;
  }
  return S_ISDIR(static_cast<mode_t>(current.mode));
}

bool is_sealed_user_tool_path(std::filesystem::path const& path, PathMetadata const& trusted_home)
{
  if (path.empty() || !path.is_absolute() || path != path.lexically_normal() || trusted_home.requested_path.empty() ||
      !trusted_home.requested_path.is_absolute() || trusted_home.requested_path != trusted_home.requested_path.lexically_normal() ||
      trusted_home.requested_path != trusted_home.canonical_path || trusted_home.requested_path_is_symlink ||
      !S_ISDIR(static_cast<mode_t>(trusted_home.mode)) || is_within(trusted_home.requested_path, path))
  {
    return false;
  }
  return is_within(path, trusted_home.requested_path);
}

ava::core::Result<bool> path_metadata_is_fresh_with_ancestors(PathMetadata const& recorded, PathMetadata const* trusted_home,
                                                              std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  if (!anchor_set)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "path freshness requires the shared AnchorSet"));
  auto current = inspect_path_metadata(recorded.requested_path, ExpectedNode::Any, false, "sealed path", anchor_set.get());
  if (!current)
    return std::unexpected(std::move(current.error()));
  bool const ancestors_fresh = trusted_home ? user_tool_ancestors_are_fresh(recorded.ancestor_metadata, current->ancestor_metadata, *trusted_home)
                                            : ancestors_are_fresh(recorded.ancestor_metadata, current->ancestor_metadata);
  if (!ancestors_fresh)
    return false;
  // Revalidate the dynamic private-primary-group predicate as well as the
  // sealed stat identity: NSS membership/name changes do not necessarily alter
  // a directory's inode metadata.
  if (auto valid = validate_safe_ancestors(current->ancestor_metadata); !valid)
    return false;
  struct stat status{};
  status.st_mode = static_cast<mode_t>(current->mode);
  status.st_uid = static_cast<uid_t>(current->owner);
  status.st_gid = static_cast<gid_t>(current->group);
  if (S_ISDIR(status.st_mode) && !directory_mode_is_safe(status, true))
    return false;
  current->ancestor_metadata = recorded.ancestor_metadata;
  return *current == recorded;
}

ava::core::Result<bool> path_metadata_is_fresh(PathMetadata const& recorded, std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  return path_metadata_is_fresh_with_ancestors(recorded, nullptr, anchor_set);
}

ava::core::Result<bool> trusted_home_metadata_is_fresh(PathMetadata const& recorded, std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  if (!anchor_set)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "trusted-home freshness requires the shared AnchorSet"));
  auto current = inspect_path_metadata(recorded.requested_path, ExpectedNode::Directory, true, "trusted command discovery home", anchor_set.get());
  if (!current)
    return std::unexpected(std::move(current.error()));
  if (!ancestors_are_fresh(recorded.ancestor_metadata, current->ancestor_metadata))
    return false;
  // Re-run ownership, mode, and the dynamic private-primary-group checks. The
  // trusted home itself does not permit the private-group write exception.
  if (auto valid = validate_safe_directory(*current, true, false, "trusted command discovery home"); !valid)
    return false;
  return trusted_home_directory_identity_is_fresh(recorded, *current);
}

ava::core::Result<bool> user_tool_path_metadata_is_fresh(PathMetadata const& recorded, PathMetadata const& trusted_home,
                                                         std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  if (!is_sealed_user_tool_path(recorded.requested_path, trusted_home))
    return false;
  auto home_fresh = trusted_home_metadata_is_fresh(trusted_home, anchor_set);
  if (!home_fresh || !*home_fresh)
    return home_fresh;
  return path_metadata_is_fresh_with_ancestors(recorded, &trusted_home, anchor_set);
}

ava::core::Result<bool> executable_metadata_is_fresh_with_ancestors(ExecutableMetadata const& recorded, PathMetadata const* trusted_home,
                                                                    std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  if (!anchor_set)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "executable freshness requires the shared AnchorSet"));
  auto current = executable_metadata(recorded.requested_path, *anchor_set);
  if (!current)
    return std::unexpected(std::move(current.error()));
  bool const ancestors_fresh = trusted_home ? user_tool_ancestors_are_fresh(recorded.ancestor_metadata, current->ancestor_metadata, *trusted_home)
                                            : ancestors_are_fresh(recorded.ancestor_metadata, current->ancestor_metadata);
  if (!ancestors_fresh)
    return false;
  if (auto valid = validate_safe_ancestors(current->ancestor_metadata); !valid)
    return false;
  current->ancestor_metadata = recorded.ancestor_metadata;
  return *current == recorded;
}

ava::core::Result<bool> executable_metadata_is_fresh(ExecutableMetadata const& recorded, std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  return executable_metadata_is_fresh_with_ancestors(recorded, nullptr, anchor_set);
}

ava::core::Result<bool> user_tool_executable_metadata_is_fresh(ExecutableMetadata const& recorded, PathMetadata const& trusted_home,
                                                               std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  if (!is_sealed_user_tool_path(recorded.requested_path, trusted_home))
    return false;
  auto home_fresh = trusted_home_metadata_is_fresh(trusted_home, anchor_set);
  if (!home_fresh || !*home_fresh)
    return home_fresh;
  return executable_metadata_is_fresh_with_ancestors(recorded, &trusted_home, anchor_set);
}

}  // namespace ava::command::detail
