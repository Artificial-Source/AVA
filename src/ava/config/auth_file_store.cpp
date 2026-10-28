#include "sys.h"
#include "ava/config/auth_file_store.h"
#include "ava/config/auth_record.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/fd_io.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::config {
namespace {

constexpr std::size_t max_auth_file_bytes = 1024 * 1024;

class ScopedFd
{
 public:
  explicit ScopedFd(int fd) : fd_(fd) { }
  ScopedFd(ScopedFd const&) = delete;
  ScopedFd& operator=(ScopedFd const&) = delete;
  ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }
  ScopedFd& operator=(ScopedFd&& other) noexcept
  {
    if (this != &other)
    {
      close_if_open();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~ScopedFd() { close_if_open(); }

  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  void close_if_open() noexcept
  {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
  }

  int fd_ = -1;
};

class TempPathCleanup
{
 public:
  explicit TempPathCleanup(std::filesystem::path path) : path_(std::move(path)) { }
  TempPathCleanup(TempPathCleanup const&) = delete;
  TempPathCleanup& operator=(TempPathCleanup const&) = delete;
  TempPathCleanup(TempPathCleanup&& other) noexcept : path_(std::move(other.path_)), active_(std::exchange(other.active_, false)) { }
  TempPathCleanup& operator=(TempPathCleanup&& other) noexcept
  {
    if (this != &other)
    {
      cleanup();
      path_ = std::move(other.path_);
      active_ = std::exchange(other.active_, false);
    }
    return *this;
  }
  ~TempPathCleanup() { cleanup(); }

  void dismiss() noexcept { active_ = false; }

 private:
  void cleanup() noexcept
  {
    if (!active_)
      return;
    std::error_code remove_error;
    std::filesystem::remove(path_, remove_error);
    active_ = false;
  }

  std::filesystem::path path_;
  bool active_ = true;
};

std::string errno_message()
{
  return std::strerror(errno);
}

bool is_symlink_mode(mode_t mode)
{
  return S_ISLNK(mode);
}

ava::core::Error auth_file_error(ava::core::ErrorCategory category, std::string message, std::filesystem::path const& path)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("path", path.string());
  return error;
}

bool has_error_context(ava::core::Error const& error, std::string_view key, std::string_view value)
{
  return std::ranges::any_of(error.context(), [&](ava::core::ErrorContext const& item) { return item.key == key && item.value == value; });
}

ava::core::VoidResult ensure_auth_directory(XdgPaths const& paths)
{
  std::error_code mkdir_error;
  std::filesystem::create_directories(paths.auth_file.parent_path(), mkdir_error);
  if (mkdir_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to create auth directory");
    error.with_context("path", paths.auth_file.parent_path().string());
    error.with_context("cause", mkdir_error.message());
    return std::unexpected(std::move(error));
  }
  if (::chmod(paths.auth_file.parent_path().c_str(), S_IRWXU) != 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to set auth directory permissions");
    error.with_context("path", paths.auth_file.parent_path().string());
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::VoidResult reject_unsafe_auth_replace_target(std::filesystem::path const& path)
{
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(path, status_error);
  if (status_error)
  {
    if (status_error.default_error_condition() == std::errc::no_such_file_or_directory)
      return {};
    auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to inspect auth file before writing", path);
    error.with_context("cause", status_error.message());
    return std::unexpected(std::move(error));
  }
  if (!std::filesystem::exists(status))
    return {};
  if (std::filesystem::is_symlink(status))
  {
    return std::unexpected(auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth file is a symbolic link", path));
  }
  if (!std::filesystem::is_regular_file(status))
  {
    return std::unexpected(auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth file is not a regular file", path));
  }
  return {};
}

ava::core::VoidResult write_all_to_fd(int fd, std::string_view body, std::filesystem::path const& path)
{
  std::size_t offset = 0;
  while (offset < body.size())
  {
    auto const written = ava::core::restart_on_eintr([&] { return ::write(fd, body.data() + offset, body.size() - offset); });
    if (written < 0)
    {
      auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to write auth file", path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (written == 0)
    {
      return std::unexpected(auth_file_error(ava::core::ErrorCategory::Io, "auth file write made no progress", path));
    }
    offset += static_cast<std::size_t>(written);
  }
  return {};
}

ava::core::VoidResult fsync_fd(int fd, std::filesystem::path const& path, std::string_view message)
{
  if (::fsync(fd) == 0)
    return {};
  auto error = auth_file_error(ava::core::ErrorCategory::Io, std::string(message), path);
  error.with_context("cause", errno_message());
  return std::unexpected(std::move(error));
}

ava::core::VoidResult fsync_parent_dir(std::filesystem::path const& path)
{
  auto const parent = path.parent_path();
  ava::core::UniqueFd const dir_fd(ava::core::restart_on_eintr([&] { return ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY); }));
  if (dir_fd.get() < 0)
  {
    auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to open auth directory for sync", parent);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  int const error_number = ava::core::fsync_with_restart(dir_fd.get());
  if (error_number == 0)
    return {};
  auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to sync auth directory", parent);
  error.with_context("cause", std::strerror(error_number));
  return std::unexpected(std::move(error));
}

ava::core::Result<ScopedFd> acquire_auth_lock(XdgPaths const& paths)
{
  auto const lock_path = paths.auth_file.parent_path() / (paths.auth_file.filename().string() + ".lock");
  int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  ScopedFd fd(::open(lock_path.c_str(), flags, S_IRUSR | S_IWUSR));
  if (fd.get() < 0)
  {
    auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to open auth lock file", lock_path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }

  struct stat opened_st{};
  if (::fstat(fd.get(), &opened_st) != 0)
  {
    auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to inspect auth lock file", lock_path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (!S_ISREG(opened_st.st_mode))
  {
    return std::unexpected(auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth lock file is not regular", lock_path));
  }
  if (opened_st.st_uid != ::geteuid() || opened_st.st_nlink != 1)
    return std::unexpected(
        auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth lock file must be owned by the current user and not hard-linked", lock_path));
  if (::fchmod(fd.get(), S_IRUSR | S_IWUSR) != 0)
  {
    auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to set auth lock permissions", lock_path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  while (::flock(fd.get(), LOCK_EX) != 0)
  {
    if (errno == EINTR)
      continue;
    auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to lock auth file", lock_path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  return fd;
}

ava::core::VoidResult write_auth_file_atomic(std::filesystem::path const& path, std::string_view body)
{
  auto replace_check = reject_unsafe_auth_replace_target(path);
  if (!replace_check)
    return std::unexpected(replace_check.error());

  auto const parent = path.parent_path();
  auto const basename = path.filename().string();
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    auto const temp_path = parent / (basename + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(attempt));
    ScopedFd const fd(::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR));
    if (fd.get() < 0)
    {
      if (errno == EEXIST)
        continue;
      auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to create temporary auth file", temp_path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    TempPathCleanup cleanup(temp_path);
    struct stat opened_st{};
    if (::fstat(fd.get(), &opened_st) != 0)
    {
      auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to inspect temporary auth file", temp_path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (!S_ISREG(opened_st.st_mode))
    {
      return std::unexpected(auth_file_error(ava::core::ErrorCategory::PermissionDenied, "temporary auth file is not regular", temp_path));
    }
    if (::fchmod(fd.get(), S_IRUSR | S_IWUSR) != 0)
    {
      auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to set auth file permissions", temp_path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (auto written = write_all_to_fd(fd.get(), body, temp_path); !written)
      return written;
    if (auto synced = fsync_fd(fd.get(), temp_path, "failed to sync auth file"); !synced)
      return synced;
    if (::rename(temp_path.c_str(), path.c_str()) != 0)
    {
      auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed to replace auth file", path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    cleanup.dismiss();
    auto synced = fsync_parent_dir(path);
    if (!synced)
      synced.error().with_context("publication", "auth file replaced; directory durability not confirmed");
    return synced;
  }
  return std::unexpected(auth_file_error(ava::core::ErrorCategory::Io, "failed to create unique temporary auth file", path));
}

ava::core::Result<CandidateRead> missing_or_ignored_auth_file(std::filesystem::path const& path, bool explicit_ava_auth_file, std::string_view message,
                                                              std::string_view cause = {})
{
  if (!explicit_ava_auth_file)
    return CandidateRead{};
  auto error = auth_file_error(ava::core::ErrorCategory::Io, std::string(message), path);
  if (!cause.empty())
    error.with_context("cause", std::string(cause));
  return std::unexpected(std::move(error));
}

}  // namespace

ava::core::Result<CandidateRead> read_text_if_exists(std::filesystem::path const& path, bool explicit_ava_auth_file, bool allow_broad_permissions)
{
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(path, status_error);
  if (status_error)
  {
    if (status_error.default_error_condition() == std::errc::no_such_file_or_directory)
      return CandidateRead{};
    return missing_or_ignored_auth_file(path, explicit_ava_auth_file, "failed to inspect auth file", status_error.message());
  }
  if (!std::filesystem::exists(status))
    return CandidateRead{};
  if (std::filesystem::is_symlink(status))
  {
    if (!explicit_ava_auth_file)
      return CandidateRead{};
    auto error = auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth file is a symbolic link", path);
    return std::unexpected(std::move(error));
  }
  if (!std::filesystem::is_regular_file(status))
  {
    if (!explicit_ava_auth_file)
      return CandidateRead{};
    auto error = auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth file is not a regular file", path);
    return std::unexpected(std::move(error));
  }

  int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  ScopedFd const fd(::open(path.c_str(), flags));
  if (fd.get() < 0)
  {
    return missing_or_ignored_auth_file(path, explicit_ava_auth_file, "failed to open auth file", errno_message());
  }

  struct stat st{};
  if (::fstat(fd.get(), &st) != 0)
  {
    return missing_or_ignored_auth_file(path, explicit_ava_auth_file, "failed to inspect opened auth file", errno_message());
  }
  if (!S_ISREG(st.st_mode) || is_symlink_mode(st.st_mode))
  {
    if (!explicit_ava_auth_file)
      return CandidateRead{};
    auto error = auth_file_error(ava::core::ErrorCategory::PermissionDenied, "opened auth file is not regular", path);
    return std::unexpected(std::move(error));
  }
  if (st.st_uid != ::geteuid())
  {
    if (!explicit_ava_auth_file)
      return CandidateRead{};
    auto error = auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth file is not owned by the current user", path);
    return std::unexpected(std::move(error));
  }
  if (!allow_broad_permissions && (st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
  {
    if (!explicit_ava_auth_file)
      return CandidateRead{};
    auto error = auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth file permissions are too broad; run `chmod 600` on the auth file", path);
    error.with_context("reason", "broad_permissions");
    error.with_context("expected_permissions", "0600");
    return std::unexpected(std::move(error));
  }
  if (st.st_size < 0 || static_cast<std::uintmax_t>(st.st_size) > max_auth_file_bytes)
  {
    if (!explicit_ava_auth_file)
      return CandidateRead{};
    auto error = auth_file_error(ava::core::ErrorCategory::Io, "auth file is too large", path);
    error.with_context("max_bytes", std::to_string(max_auth_file_bytes));
    return std::unexpected(std::move(error));
  }

#ifndef O_NOFOLLOW
  struct stat path_st{};
  if (::lstat(path.c_str(), &path_st) != 0)
  {
    return missing_or_ignored_auth_file(path, explicit_ava_auth_file, "failed to inspect auth file", errno_message());
  }
  if (is_symlink_mode(path_st.st_mode) || path_st.st_dev != st.st_dev || path_st.st_ino != st.st_ino)
  {
    if (!explicit_ava_auth_file)
      return CandidateRead{};
    auto error = auth_file_error(ava::core::ErrorCategory::PermissionDenied, "auth file changed during open", path);
    return std::unexpected(std::move(error));
  }
#endif

  std::string content;
  std::array<char, 4096> buffer{};
  while (true)
  {
    auto const bytes_read = ava::core::restart_on_eintr([&] { return ::read(fd.get(), buffer.data(), buffer.size()); });
    if (bytes_read == 0)
      break;
    if (bytes_read < 0)
    {
      auto error = auth_file_error(ava::core::ErrorCategory::Io, "failed while reading auth file", path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    content.append(buffer.data(), static_cast<std::size_t>(bytes_read));
    if (content.size() > max_auth_file_bytes)
    {
      auto error = auth_file_error(ava::core::ErrorCategory::Io, "auth file is too large", path);
      error.with_context("max_bytes", std::to_string(max_auth_file_bytes));
      return std::unexpected(std::move(error));
    }
  }
  return CandidateRead{.content = content};
}

ava::core::VoidResult store_provider_object(XdgPaths const& paths, std::string_view provider_id, std::string raw_object)
{
  if (auto ensured = ensure_auth_directory(paths); !ensured)
    return ensured;
  auto lock = acquire_auth_lock(paths);
  if (!lock)
    return std::unexpected(std::move(lock.error()));

  auto content = read_text_if_exists(paths.auth_file, true);
  if (!content && content.error().category() == ava::core::ErrorCategory::PermissionDenied && has_error_context(content.error(), "reason", "broad_permissions"))
  {
    content = CandidateRead{};
  }
  if (!content)
    return std::unexpected(std::move(content.error()));
  auto members = content->content ? parse_auth_record_members(*content->content, paths.auth_file)
                                  : ava::core::Result<std::vector<AuthRecordMember>>(std::vector<AuthRecordMember>{});
  if (!members)
    return std::unexpected(std::move(members.error()));

  std::vector<AuthRecordMember> merged;
  merged.reserve(members->size() + 1);
  bool replaced = false;
  for (auto& member : *members)
  {
    if (member.key == provider_id)
    {
      if (!replaced)
      {
        merged.push_back(AuthRecordMember{.key = std::string(provider_id), .raw_value = raw_object});
        replaced = true;
      }
      continue;
    }
    merged.push_back(std::move(member));
  }
  if (!replaced)
  {
    merged.push_back(AuthRecordMember{.key = std::string(provider_id), .raw_value = std::move(raw_object)});
  }

  return write_auth_file_atomic(paths.auth_file, serialize_auth_record_members(merged));
}

}  // namespace ava::config
