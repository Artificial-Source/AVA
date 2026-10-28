#include "sys.h"
#include "ava/session/session_store_internal.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/fd_io.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::session {
namespace {

using detail::path_io_error;
using detail::same_file_identity;

bool ancestor_sync_is_best_effort(int error_number)
{
  return error_number == EINVAL || error_number == ENOTSUP || error_number == EROFS || error_number == EACCES;
}

ava::core::VoidResult sync_session_ancestor_directories(std::vector<std::filesystem::path> const& ancestor_sync_paths)
{
  // Sync newly created ancestors and the existing directory that received
  // their outermost entry. Unsupported/inaccessible ancestor barriers are best
  // effort; the inode and its immediate parent retain mandatory barriers.
  for (auto const& ancestor : ancestor_sync_paths)
  {
    ava::core::UniqueFd ancestor_fd(::open(ancestor.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (ancestor_fd.get() < 0)
    {
      int const open_error = errno;
      if (ancestor_sync_is_best_effort(open_error))
        continue;
      return std::unexpected(path_io_error("failed to open session lease ancestor directory for sync", ancestor, open_error));
    }
    int const sync_error = ava::core::fsync_with_restart(ancestor_fd.get());
    int const close_error = ancestor_fd.close_checked();
    if (sync_error != 0 && !ancestor_sync_is_best_effort(sync_error))
    {
      auto error = path_io_error("failed to sync session lease ancestor directory", ancestor, sync_error);
      if (close_error != 0)
        error.with_context("ancestor_close_cause", std::strerror(close_error));
      return std::unexpected(std::move(error));
    }
    if (close_error != 0)
      return std::unexpected(path_io_error("failed to close session lease ancestor directory after sync", ancestor, close_error));
  }
  return {};
}

}  // namespace

SessionLease::SessionLease(int fd, std::filesystem::path canonical_path, bool created) : fd_(fd), canonical_path_(std::move(canonical_path)), created_(created)
{
}

SessionLease::SessionLease(SessionLease&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), canonical_path_(std::move(other.canonical_path_)), created_(std::exchange(other.created_, false))
{
}

SessionLease& SessionLease::operator=(SessionLease&& other) noexcept
{
  if (this != &other)
  {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
    canonical_path_ = std::move(other.canonical_path_);
    created_ = std::exchange(other.created_, false);
  }
  return *this;
}

SessionLease::~SessionLease()
{
  if (fd_ >= 0)
    ::close(fd_);
}

ava::core::Result<SessionLease> SessionLease::create_and_acquire(std::filesystem::path const& session_path)
{
  std::error_code absolute_error;
  auto diagnostic_path = std::filesystem::absolute(session_path, absolute_error).lexically_normal();
  if (absolute_error)
    diagnostic_path = session_path.lexically_normal();

  auto const parent_path = session_path.parent_path();
  auto const final_name = session_path.filename().string();
  if (final_name.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "new session lease path must name a file");
    error.with_context("path", diagnostic_path.string());
    return std::unexpected(std::move(error));
  }
  std::vector<std::filesystem::path> directories_to_create;
  for (auto directory = parent_path.is_absolute() ? parent_path : std::filesystem::path(".") / parent_path; !directory.empty();)
  {
    std::error_code status_error;
    if (std::filesystem::is_directory(directory, status_error))
      break;
    if (status_error && status_error != std::errc::no_such_file_or_directory)
      return std::unexpected(path_io_error("failed to inspect session lease directory before creation", directory, status_error.value()));
    directories_to_create.push_back(directory);
    auto next = directory.parent_path();
    if (next == directory)
      break;
    directory = std::move(next);
  }
  std::vector<std::filesystem::path> ancestor_sync_paths;
  for (auto directory = directories_to_create.rbegin(); directory != directories_to_create.rend(); ++directory)
  {
    std::error_code mkdir_error;
    bool const created = std::filesystem::create_directory(*directory, mkdir_error);
    if (mkdir_error)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to create session lease directory");
      error.with_context("path", directory->string()).with_context("cause", mkdir_error.message());
      return std::unexpected(std::move(error));
    }
    if (created)
      ancestor_sync_paths.push_back(*directory);
  }
  if (!ancestor_sync_paths.empty())
  {
    // The immediate parent has its own mandatory barrier. Other newly created
    // directories and the directory receiving their outermost entry sync inside out.
    auto receiving_directory = ancestor_sync_paths.front().parent_path();
    if (ancestor_sync_paths.back() == directories_to_create.front())
      ancestor_sync_paths.pop_back();
    std::ranges::reverse(ancestor_sync_paths);
    ancestor_sync_paths.push_back(std::move(receiving_directory));
  }

  ava::core::UniqueFd parent_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!parent_fd)
    return std::unexpected(path_io_error("failed to open new session lease directory", parent_path, errno));
  // Change permissions only on the no-follow directory descriptor used for
  // publication, never on a pathname that might resolve to a symlink target.
  if (::fchmod(parent_fd.get(), 0700) != 0)
    return std::unexpected(path_io_error("failed to set session lease directory permissions", parent_path, errno));

  static std::atomic<std::uint64_t> temporary_sequence = 0;
  std::string temporary_name;
  int fd = -1;
  for (std::size_t attempt = 0; attempt < 32; ++attempt)
  {
    temporary_name = ".ava-session-lease-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                     std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed) + 1) + ".tmp";
    fd = ::openat(parent_fd.get(), temporary_name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0 || errno != EEXIST)
      break;
  }
  if (fd < 0)
  {
    int const error_number = errno;
    auto const category = error_number == ELOOP ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
    auto error = ava::core::Error(
        category, error_number == ELOOP ? "temporary session lease path must not be a symlink" : "failed to create private new session lease inode");
    error.with_context("path", diagnostic_path.string()).with_context("directory", parent_path.string()).with_context("cause", std::strerror(error_number));
    return std::unexpected(std::move(error));
  }

  ava::core::UniqueFd lease_fd(fd);
  bool temporary_link_exists = true;
  bool final_link_exists = false;
  struct stat status{};
  bool status_valid = false;
  auto cleanup_owned_name = [&](std::string const& name, std::string_view context_key, ava::core::Error& error) {
    struct stat named_status{};
    struct stat current_status{};
    if ((!status_valid && fstat(fd, &current_status) != 0) ||
        (fstatat(parent_fd.get(), name.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) != 0 && errno != ENOENT))
    {
      error.with_context(std::string(context_key), std::strerror(errno));
      return;
    }
    auto const& expected_status = status_valid ? status : current_status;
    if (same_file_identity(expected_status, named_status) && ::unlinkat(parent_fd.get(), name.c_str(), 0) != 0)
      error.with_context(std::string(context_key), std::strerror(errno));
  };
  auto fail_created_inode = [&](ava::core::Error error) -> ava::core::Result<SessionLease> {
    if (final_link_exists)
      cleanup_owned_name(final_name, "final_cleanup_cause", error);
    if (temporary_link_exists)
      cleanup_owned_name(temporary_name, "temporary_cleanup_cause", error);
    if (final_link_exists)
    {
      if (int const sync_error = ava::core::fsync_with_restart(parent_fd.get()); sync_error != 0)
        error.with_context("cleanup_directory_sync_cause", std::strerror(sync_error));
    }
    if (int const close_error = lease_fd.close_checked(); close_error != 0)
      error.with_context("lease_close_cause", std::strerror(close_error));
    if (int const close_error = parent_fd.close_checked(); close_error != 0)
      error.with_context("parent_close_cause", std::strerror(close_error));
    return std::unexpected(std::move(error));
  };

  if (fstat(fd, &status) != 0)
    return fail_created_inode(path_io_error("failed to inspect private new session lease inode", diagnostic_path, errno));
  status_valid = true;
  if (!S_ISREG(status.st_mode))
    return fail_created_inode(path_io_error("private new session lease inode is not a regular file", diagnostic_path));
  if (status.st_nlink != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "private new session lease inode must have exactly one link");
    error.with_context("path", diagnostic_path.string()).with_context("link_count", std::to_string(status.st_nlink));
    return fail_created_inode(std::move(error));
  }
  if (fchmod(fd, 0600) != 0)
    return fail_created_inode(path_io_error("failed to set private new session lease permissions", diagnostic_path, errno));
  if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    return fail_created_inode(path_io_error("failed to acquire private new session lease", diagnostic_path, errno));

  // Publish only an inode whose initial contents have reached storage.
  if (int const sync_error = ava::core::fdatasync_with_restart(fd); sync_error != 0)
    return fail_created_inode(path_io_error("failed to sync private new session lease inode before publication", diagnostic_path, sync_error));

  if (::linkat(parent_fd.get(), temporary_name.c_str(), parent_fd.get(), final_name.c_str(), 0) != 0)
  {
    int const error_number = errno;
    auto const category = error_number == ELOOP ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
    auto error = ava::core::Error(
        category, error_number == ELOOP ? "new session lease path must not be a symlink" : "failed to publish locked new session lease without replacement");
    error.with_context("path", diagnostic_path.string()).with_context("cause", std::strerror(error_number));
    return fail_created_inode(std::move(error));
  }
  final_link_exists = true;

  if (fstat(fd, &status) != 0)
    return fail_created_inode(path_io_error("failed to inspect published new session lease", diagnostic_path, errno));
  if (status.st_nlink != 2)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "published new session lease must have exactly two links before commit");
    error.with_context("path", diagnostic_path.string()).with_context("link_count", std::to_string(status.st_nlink));
    return fail_created_inode(std::move(error));
  }
  if (::unlinkat(parent_fd.get(), temporary_name.c_str(), 0) != 0)
    return fail_created_inode(path_io_error("failed to remove private new session lease name after publication", diagnostic_path, errno));
  temporary_link_exists = false;

  if (fstat(fd, &status) != 0)
    return fail_created_inode(path_io_error("failed to inspect committed new session lease", diagnostic_path, errno));
  if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || (status.st_mode & 0777) != 0600)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "committed new session lease has invalid ownership invariants");
    error.with_context("path", diagnostic_path.string())
        .with_context("link_count", std::to_string(status.st_nlink))
        .with_context("mode", std::to_string(status.st_mode & 0777));
    return fail_created_inode(std::move(error));
  }
  struct stat final_status{};
  if (fstatat(parent_fd.get(), final_name.c_str(), &final_status, AT_SYMLINK_NOFOLLOW) != 0)
    return fail_created_inode(path_io_error("failed to verify committed new session lease name", diagnostic_path, errno));
  if (!same_file_identity(status, final_status) || final_status.st_nlink != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "committed new session name does not identify the locked lease inode");
    error.with_context("path", diagnostic_path.string()).with_context("link_count", std::to_string(final_status.st_nlink));
    return fail_created_inode(std::move(error));
  }
  // The final name and removal of the temporary name must both be durable.
  if (int const sync_error = ava::core::fsync_with_restart(parent_fd.get()); sync_error != 0)
    return fail_created_inode(path_io_error("failed to sync published new session lease directory", parent_path, sync_error));
  if (auto synced = sync_session_ancestor_directories(ancestor_sync_paths); !synced)
    return fail_created_inode(std::move(synced.error()));
  if (int const close_error = parent_fd.close_checked(); close_error != 0)
  {
    auto error = path_io_error("new session lease was durably published but its directory descriptor close failed", parent_path, close_error);
    error.with_context("creation_state", "durably_published");
    if (int const lease_close_error = lease_fd.close_checked(); lease_close_error != 0)
      error.with_context("lease_close_cause", std::strerror(lease_close_error));
    return std::unexpected(std::move(error));
  }
  return SessionLease(lease_fd.release(), diagnostic_path, true);
}

ava::core::Result<SessionLease> SessionLease::acquire(std::filesystem::path const& session_path)
{
  std::error_code absolute_error;
  auto diagnostic_path = std::filesystem::absolute(session_path, absolute_error).lexically_normal();
  if (absolute_error)
    diagnostic_path = session_path.lexically_normal();

  // Classify the final component without a potentially blocking data open.
  // A hostile FIFO replacement must fail as metadata, never stall lease
  // acquisition before the regular-file check.
  int metadata_fd = ::open(session_path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
  if (metadata_fd < 0)
  {
    auto const category = errno == ELOOP ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
    auto error = ava::core::Error(category, errno == ELOOP ? "session lease path must not be a symlink" : "failed to inspect session lease");
    error.with_context("path", diagnostic_path.string()).with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(error));
  }
  struct stat metadata_status{};
  int const metadata_error = fstat(metadata_fd, &metadata_status) == 0 ? 0 : errno;
  int const metadata_close_error = ::close(metadata_fd) == 0 ? 0 : errno;
  if (metadata_error != 0)
    return std::unexpected(path_io_error("failed to inspect session lease metadata", diagnostic_path, metadata_error));
  if (metadata_close_error != 0)
    return std::unexpected(path_io_error("failed to close session lease metadata descriptor", diagnostic_path, metadata_close_error));
  if (!S_ISREG(metadata_status.st_mode))
  {
    if (S_ISLNK(metadata_status.st_mode))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session lease path must not be a symlink");
      error.with_context("path", diagnostic_path.string());
      return std::unexpected(std::move(error));
    }
    return std::unexpected(path_io_error("session lease target is not a regular file", diagnostic_path));
  }

  int const fd = ::open(session_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
  {
    auto const category = errno == ELOOP ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
    auto error = ava::core::Error(category, errno == ELOOP ? "session lease path must not be a symlink" : "failed to open session lease");
    error.with_context("path", diagnostic_path.string()).with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(error));
  }
  SessionLease lease(fd, diagnostic_path, false);
  struct stat status{};
  if (fstat(fd, &status) != 0)
    return std::unexpected(path_io_error("failed to inspect session lease target", diagnostic_path, errno));
  if (!same_file_identity(metadata_status, status))
    return std::unexpected(path_io_error("session lease target changed while opening", diagnostic_path));
  if (status.st_nlink != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session lease target must have exactly one link");
    error.with_context("path", diagnostic_path.string()).with_context("link_count", std::to_string(status.st_nlink));
    return std::unexpected(std::move(error));
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0)
  {
    int const error_number = errno;
    auto error = ava::core::Error(
        error_number == EWOULDBLOCK || error_number == EAGAIN ? ava::core::ErrorCategory::Session : ava::core::ErrorCategory::Io,
        error_number == EWOULDBLOCK || error_number == EAGAIN ? "session is already owned by another AVA host" : "failed to acquire session lease");
    error.with_context("path", diagnostic_path.string()).with_context("cause", std::strerror(error_number));
    return std::unexpected(std::move(error));
  }
  return lease;
}

bool SessionLease::active() const noexcept
{
  return fd_ >= 0;
}

ava::core::Result<SessionLease> SessionLease::duplicate() const
{
  if (fd_ < 0)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "cannot duplicate an inactive session lease"));
  int const duplicate_fd = fcntl(fd_, F_DUPFD_CLOEXEC, 3);
  if (duplicate_fd < 0)
    return std::unexpected(path_io_error("failed to duplicate exact session lease with CLOEXEC", canonical_path_, errno));
  return SessionLease(duplicate_fd, canonical_path_, created_);
}

std::filesystem::path const& SessionLease::canonical_path() const noexcept
{
  return canonical_path_;
}

off_t SessionLease::offset_for_test() const noexcept
{
  if (fd_ < 0)
    return -1;
  return ::lseek(fd_, 0, SEEK_CUR);
}

}  // namespace ava::session
