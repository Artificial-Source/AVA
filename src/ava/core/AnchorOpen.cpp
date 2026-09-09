#include "sys.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorOpen_test_support.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/open_beneath.h"

#include <cerrno>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "debug.h"

#ifndef O_PATH
#define O_PATH O_RDONLY
#endif

namespace ava::core {
namespace {

std::mutex external_reopen_hook_mutex;
std::shared_ptr<std::function<void()>> external_reopen_hook;

void invoke_external_reopen_hook()
{
  std::shared_ptr<std::function<void()>> hook;
  {
    std::lock_guard lock(external_reopen_hook_mutex);
    hook = external_reopen_hook;
  }
  if (hook && *hook)
    (*hook)();
}

class UniqueFd
{
 public:
  UniqueFd() = default;

  explicit UniqueFd(int fd) : fd_(fd) { }

  ~UniqueFd()
  {
    if (fd_ != -1)
      ::close(fd_);
  }

  UniqueFd(UniqueFd const&) = delete;
  UniqueFd& operator=(UniqueFd const&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }

  UniqueFd& operator=(UniqueFd&& other) noexcept
  {
    if (this != &other)
    {
      if (fd_ != -1)
      {
        ::close(fd_);
      }

      fd_ = std::exchange(other.fd_, -1);
    }

    return *this;
  }

  int get() const { return fd_; }
  int release() { return std::exchange(fd_, -1); }

 private:
  int fd_ = -1;
};

[[noreturn]] void throw_errno(char const* operation)
{
  int const error = errno;
  throw std::system_error(error, std::generic_category(), operation);
}

[[noreturn]] void throw_error(int error, char const* operation)
{
  throw std::system_error(error, std::generic_category(), operation);
}

UniqueFd duplicate_fd(int fd)
{
  int const copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);

  if (copy == -1)
    throw_errno("fcntl(F_DUPFD_CLOEXEC)");

  return UniqueFd(copy);
}

UniqueFd open_root()
{
  int const fd = ::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);

  if (fd == -1)
    throw_errno("open(/)");

  return UniqueFd(fd);
}

UniqueFd open_nofollow(int directory, std::filesystem::path const& component)
{
  int const fd = ::openat(directory, component.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);

  if (fd == -1)
    throw_errno("openat(path component)");

  return UniqueFd(fd);
}

UniqueFd open_parent(int directory)
{
  int const fd = ::openat(directory, "..", O_PATH | O_DIRECTORY | O_CLOEXEC);

  if (fd == -1)
    throw_errno("openat(..)");

  return UniqueFd(fd);
}

mode_t file_type(int fd)
{
  struct stat status{};

  if (::fstat(fd, &status) == -1)
    throw_errno("fstat");

  return status.st_mode & S_IFMT;
}

std::string read_link(int fd)
{
  std::string target(256, '\0');

  for (;;)
  {
    ssize_t const length = ::readlinkat(fd, "", target.data(), target.size());

    if (length == -1)
      throw_errno("readlinkat");

    if (static_cast<std::size_t>(length) < target.size())
    {
      target.resize(static_cast<std::size_t>(length));
      return target;
    }

    if (target.size() > 1024 * 1024)
      throw_error(ENAMETOOLONG, "readlinkat");

    target.resize(2 * target.size());
  }
}

struct FileId
{
  std::uint64_t mount;
  std::uint64_t inode;

  friend bool operator==(FileId const&, FileId const&) = default;
};

FileId file_id(int fd)
{
  struct statx status{};
  unsigned int constexpr mask = STATX_INO | STATX_MNT_ID;

  if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, mask, &status) == -1)
    throw_errno("statx");

  if ((status.stx_mask & mask) != mask)
    throw_error(ENOTSUP, "statx did not return inode and mount IDs");

  return {status.stx_mnt_id, status.stx_ino};
}

bool is_beneath_any_anchor(int directory, std::vector<FileId> const& anchor_ids)
{
  UniqueFd current = duplicate_fd(directory);
  FileId current_id = file_id(current.get());

  for (;;)
  {
    for (FileId const& anchor_id : anchor_ids)
    {
      if (current_id == anchor_id)
        return true;
    }

    UniqueFd parent = open_parent(current.get());
    FileId const parent_id = file_id(parent.get());

    if (parent_id == current_id)
      return false;

    current = std::move(parent);
    current_id = parent_id;
  }
}

struct ResolvedPath
{
  UniqueFd object;
  UniqueFd parent;
  std::filesystem::path final_component;
  bool is_directory;
};

std::vector<std::filesystem::path> path_components(std::filesystem::path const& path)
{
  std::vector<std::filesystem::path> result;

  for (auto const& component : path.relative_path())
    result.push_back(component);

  return result;
}

bool is_beneath_any_anchor(ResolvedPath const& path, std::vector<FileId> const& anchor_ids)
{
  int const directory = path.is_directory ? path.object.get() : path.parent.get();

  return is_beneath_any_anchor(directory, anchor_ids);
}

Result<ResolvedPath> resolve_external_path(AnchorSet const& anchors, std::filesystem::path const& absolute, bool nofollow_logical_final)
{
  auto const& anchor_list = anchors.anchors();
  std::vector<FileId> anchor_ids;
  anchor_ids.reserve(anchor_list.size());
  for (auto const& anchor : anchor_list)
  {
    if (file_type(anchor.fd) != S_IFDIR)
      throw_error(ENOTDIR, "anchor file descriptor");
    anchor_ids.push_back(file_id(anchor.fd));
  }

  UniqueFd root = open_root();
  UniqueFd current = duplicate_fd(root.get());
  std::deque<std::filesystem::path> components;
  for (auto const& component : path_components(absolute))
    components.push_back(component);
  unsigned int followed_symlinks = 0;

  while (!components.empty())
  {
    auto component = std::move(components.front());
    components.pop_front();
    if (component.empty() || component == ".")
      continue;
    if (component == "..")
    {
      current = open_parent(current.get());
      if (is_beneath_any_anchor(current.get(), anchor_ids))
        return std::unexpected(Error(ErrorCategory::Configuration, "external path resolution enters a writable anchor"));
      continue;
    }

    UniqueFd object = open_nofollow(current.get(), component);
    mode_t const type = file_type(object.get());
    if (type == S_IFLNK && !(nofollow_logical_final && components.empty()))
    {
      if (++followed_symlinks > 40)
        throw_error(ELOOP, "symbolic-link resolution");
      auto const target = std::filesystem::path(read_link(object.get()));
      if (target.is_absolute())
        current = duplicate_fd(root.get());
      auto const target_components = path_components(target);
      for (auto it = target_components.rbegin(); it != target_components.rend(); ++it)
        components.push_front(*it);
      continue;
    }

    ResolvedPath resolved{std::move(object), duplicate_fd(current.get()), std::move(component), type == S_IFDIR};
    if (is_beneath_any_anchor(resolved, anchor_ids))
      return std::unexpected(Error(ErrorCategory::Configuration, "external path resolution enters a writable anchor"));
    if (components.empty())
      return resolved;
    if (!resolved.is_directory)
      throw_error(ENOTDIR, "non-directory pathname component");
    current = std::move(resolved.object);
  }

  ResolvedPath resolved{std::move(current), UniqueFd(), {}, true};
  if (is_beneath_any_anchor(resolved, anchor_ids))
    return std::unexpected(Error(ErrorCategory::Configuration, "external path resolution enters a writable anchor"));
  return resolved;
}

UniqueFd reopen_resolved_object(ResolvedPath const& resolved, int flags, mode_t mode)
{
  auto const expected = file_id(resolved.object.get());
  invoke_external_reopen_hook();
  int const fd = resolved.parent.get() >= 0 ? ::openat(resolved.parent.get(), resolved.final_component.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, mode)
                                            : ::openat(resolved.object.get(), ".", flags | O_NOFOLLOW | O_CLOEXEC, mode);
  if (fd == -1)
    throw_errno("openat(resolved external target)");
  UniqueFd reopened(fd);
  if (file_id(reopened.get()) != expected)
    throw_error(EIO, "resolved external target identity mismatch");
  return reopened;
}

// Build an error for a failed open, translating the errno into the category
// that matches its meaning for an anchor-resolved path: symlink escapes and
// loops are permission denials, a missing final component is NotFound, and
// everything else is a generic I/O failure. path is the candidate being
// opened; anchor_root is attached when the open was beneath a specific anchor
// (empty for external reads).
Error open_path_error(std::string const& message, int error_number, std::filesystem::path const& path, std::filesystem::path const& anchor_root = {})
{
  ErrorCategory const category = (error_number == ELOOP || error_number == EXDEV) ? ErrorCategory::PermissionDenied
                                                                                  : (error_number == ENOENT ? ErrorCategory::NotFound : ErrorCategory::Io);
  auto error = Error(category, message);
  if (!path.empty())
    error.with_context("path", path.string());
  if (!anchor_root.empty())
    error.with_context("anchor_root", anchor_root.string());
  if (error_number != 0)
    error.with_context("cause", std::generic_category().message(error_number));
  return error;
}

}  // namespace

void testing::AnchorOpenTestAccess::set_before_external_reopen_hook(std::function<void()> hook)
{
  auto retained = std::make_shared<std::function<void()>>(std::move(hook));
  std::lock_guard lock(external_reopen_hook_mutex);
  external_reopen_hook = std::move(retained);
}

void testing::AnchorOpenTestAccess::clear_before_external_reopen_hook() noexcept
{
  std::lock_guard lock(external_reopen_hook_mutex);
  external_reopen_hook.reset();
}

// Return the current working directory as a logical path, preserving symlinks.
// This function is guaranteed to return a non-empty, absolute path, or throw
// an exception.
//
// The shell sets $PWD to the logical path; if it resolves to the same physical
// directory as getcwd(), prefer it so that symlinked path components are kept.
// This is the path-equality check exception: inode comparison is used to verify
// that $PWD and getcwd() refer to the same directory, but the returned path
// is always the logical form.
std::filesystem::path logical_cwd()
{
  char const* const pwd_environment = std::getenv("PWD");

  if (pwd_environment == nullptr || pwd_environment[0] != '/')
    throw std::runtime_error("PWD is not set to an absolute path");

  std::filesystem::path const logical(pwd_environment);

  // $PWD was validated above to start with '/'; if this fires, that validation was bypassed, so keep the check that
  // rejects a non-absolute $PWD before constructing logical.
  ASSERT(logical.is_absolute());

  int const cwd_fd = ::open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);

  if (cwd_fd == -1)
    throw_errno("open(.)");

  UniqueFd cwd(cwd_fd);

  int const logical_fd = ::open(logical.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);

  if (logical_fd == -1)
    throw_errno("PWD does not resolve to a directory: open(PWD)");

  UniqueFd logical_directory(logical_fd);

  if (file_id(cwd.get()) != file_id(logical_directory.get()))
    // If this error is thrown that the current directory was changed without adjusting the environment variable PWD to the new (logical) path.
    throw std::runtime_error("PWD does not correspond to the current working directory");

  return logical;
}

AnchorOpen::AnchorOpen(int fd, std::filesystem::path root, std::filesystem::path absolute, std::filesystem::path relative)
    : fd_(fd), root_(std::move(root)), absolute_(std::move(absolute)), relative_(std::move(relative))
{
}

AnchorOpen::AnchorOpen(AnchorOpen&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), root_(std::move(other.root_)), absolute_(std::move(other.absolute_)), relative_(std::move(other.relative_))
{
}

AnchorOpen& AnchorOpen::operator=(AnchorOpen&& other) noexcept
{
  if (this == &other)
    return *this;
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = std::exchange(other.fd_, -1);
  root_ = std::move(other.root_);
  absolute_ = std::move(other.absolute_);
  relative_ = std::move(other.relative_);
  return *this;
}

AnchorOpen::~AnchorOpen()
{
  if (fd_ >= 0)
    ::close(fd_);
}

int AnchorOpen::fd() const noexcept
{
  return fd_;
}

std::filesystem::path const& AnchorOpen::root() const noexcept
{
  return root_;
}

std::filesystem::path const& AnchorOpen::absolute() const noexcept
{
  return absolute_;
}

std::filesystem::path const& AnchorOpen::relative() const noexcept
{
  return relative_;
}

Result<AnchorOpen> open_writable(AnchorSet const& anchors, std::filesystem::path const& candidate, int flags, mode_t mode)
{
  DoutEntering(dc::avacore, "open_writable(" << anchors << ", " << candidate << ", " << NAMESPACE_DEBUG::PosixMode(flags) << ", " << std::oct << mode << ")");

  auto ref = anchors.find_anchor(candidate);
  if (!ref)
    return std::unexpected(std::move(ref.error()));
  auto const& anchor = ref->anchor();
  auto const absolute = ref->absolute();
  int const fd = open_beneath(anchor.fd, ref->relative(), flags, mode);
  if (fd < 0)
    return std::unexpected(open_path_error("open_writable failed to open path beneath its anchor", errno, absolute, anchor.root));
  return AnchorOpen(fd, anchor.root, std::move(absolute), ref->relative());
}

Result<AnchorOpen> open_readable(AnchorSet const& anchors, std::filesystem::path const& candidate, int flags, mode_t mode)
{
  DoutEntering(dc::avacore, "open_readable(" << anchors << ", " << candidate << ", " << NAMESPACE_DEBUG::PosixMode(flags) << ", " << std::oct << mode << ")");

  if ((flags & O_ACCMODE) != O_RDONLY)
    return std::unexpected(Error(ErrorCategory::InvalidArgument, "open_readable requires O_RDONLY access flags"));

  auto ref = anchors.find_anchor(candidate);
  if (ref)
  {
    auto const& anchor = ref->anchor();
    int const fd = open_beneath(anchor.fd, ref->relative(), flags, mode);
    if (fd < 0)
      return std::unexpected(open_path_error("open_readable failed to open path beneath its anchor", errno, ref->absolute(), anchor.root));
    return AnchorOpen(fd, anchor.root, ref->absolute(), ref->relative());
  }

  // find_anchor returns PermissionDenied specifically when the candidate is
  // lexically outside all writable anchors (every other failure it reports is
  // a hard Io error such as an empty path or no anchors being available). That
  // case is an external read and is allowed; any other category is propagated.
  if (ref.error().category() != ErrorCategory::PermissionDenied)
    return std::unexpected(std::move(ref.error()));

  // Relative candidates resolve against the first anchor inside find_anchor and
  // therefore never reach this branch; only absolute candidates can be external.
  // If this fires, a relative candidate slipped past find_anchor; fix find_anchor to resolve relative candidates
  // against the first anchor before returning.
  ASSERT(candidate.is_absolute());
  std::filesystem::path const absolute = candidate.lexically_normal();

  try
  {
    auto resolved = resolve_external_path(anchors, absolute, (flags & O_NOFOLLOW) != 0);
    if (!resolved)
    {
      auto error = std::move(resolved.error());
      error.with_context("path", absolute.string());
      return std::unexpected(std::move(error));
    }
    auto opened = reopen_resolved_object(*resolved, flags, mode);
    return AnchorOpen(opened.release(), {}, std::move(absolute), {});
  }
  catch (std::system_error const& error)
  {
    return std::unexpected(open_path_error("open_readable failed to inspect and open external path", error.code().value(), absolute, {}));
  }
}

}  // namespace ava::core
