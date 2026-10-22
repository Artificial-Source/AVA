#include "sys.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/atomic_file.h"
#include "ava/core/fd_io.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::core {
namespace {

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

std::string errno_message(int error_number = errno)
{
  return std::strerror(error_number);
}

std::string describe(std::string_view description)
{
  return description.empty() ? std::string("file") : std::string(description);
}

Error file_error(ErrorCategory category, std::string message, std::filesystem::path const& path)
{
  auto error = Error(category, std::move(message));
  error.with_context("path", path.string());
  return error;
}

VoidResult reject_unsafe_replace_target(std::filesystem::path const& path, std::string_view description)
{
  if (path.empty())
    return std::unexpected(file_error(ErrorCategory::InvalidArgument, describe(description) + " path is empty", path));

  struct stat target_st{};
  if (::lstat(path.c_str(), &target_st) != 0)
  {
    if (errno == ENOENT)
      return {};
    auto error = file_error(ErrorCategory::Io, "failed to inspect existing " + describe(description), path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (S_ISLNK(target_st.st_mode))
    return std::unexpected(file_error(ErrorCategory::PermissionDenied, describe(description) + " target is a symlink", path));
  if (!S_ISREG(target_st.st_mode))
    return std::unexpected(file_error(ErrorCategory::PermissionDenied, describe(description) + " target is not a regular file", path));
  return {};
}

VoidResult write_all_to_fd(int fd, std::string_view body, std::filesystem::path const& path, std::string_view description)
{
  int const error_number = ava::core::write_all(fd, body);
  if (error_number == 0)
    return {};
  auto error = file_error(ErrorCategory::Io, "failed to write " + describe(description), path);
  error.with_context("cause", errno_message(error_number));
  return std::unexpected(std::move(error));
}

VoidResult fsync_fd(int fd, std::filesystem::path const& path, std::string_view description)
{
  int const error_number = fsync_with_restart(fd);
  if (error_number == 0)
    return {};
  auto error = file_error(ErrorCategory::Io, "failed to sync " + describe(description), path);
  error.with_context("cause", errno_message(error_number));
  return std::unexpected(std::move(error));
}

void fsync_parent_best_effort(std::filesystem::path const& path)
{
  auto const parent = path.parent_path();
  if (parent.empty())
    return;
  ava::core::UniqueFd const dir_fd(::open(parent.c_str(), O_RDONLY | O_CLOEXEC));
  if (dir_fd.get() < 0)
    return;
  static_cast<void>(fsync_with_restart(dir_fd.get()));
}

}  // namespace

VoidResult write_text_file_atomic(std::filesystem::path const& path, std::string_view body, std::string_view description)
{
  if (auto checked = reject_unsafe_replace_target(path, description); !checked)
    return checked;

  auto const parent = path.parent_path();
  if (!parent.empty())
  {
    std::error_code create_error;
    std::filesystem::create_directories(parent, create_error);
    if (create_error)
    {
      auto error = file_error(ErrorCategory::Io, "failed to create " + describe(description) + " directory", parent);
      error.with_context("cause", create_error.message());
      return std::unexpected(std::move(error));
    }
  }

  auto const basename = path.filename().string();
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    auto const temp_path = parent / (basename + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(attempt));
    ava::core::UniqueFd const fd(::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR));
    if (fd.get() < 0)
    {
      if (errno == EEXIST)
        continue;
      auto error = file_error(ErrorCategory::Io, "failed to create temporary " + describe(description), temp_path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    TempPathCleanup cleanup(temp_path);
    struct stat opened_st{};
    if (::fstat(fd.get(), &opened_st) != 0)
    {
      auto error = file_error(ErrorCategory::Io, "failed to inspect temporary " + describe(description), temp_path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (!S_ISREG(opened_st.st_mode))
      return std::unexpected(file_error(ErrorCategory::PermissionDenied, "temporary " + describe(description) + " is not regular", temp_path));
    if (::fchmod(fd.get(), S_IRUSR | S_IWUSR) != 0)
    {
      auto error = file_error(ErrorCategory::Io, "failed to set temporary " + describe(description) + " permissions", temp_path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (auto written = write_all_to_fd(fd.get(), body, temp_path, description); !written)
      return written;
    if (auto synced = fsync_fd(fd.get(), temp_path, description); !synced)
      return synced;
    if (::rename(temp_path.c_str(), path.c_str()) != 0)
    {
      auto error = file_error(ErrorCategory::Io, "failed to replace " + describe(description), path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    cleanup.dismiss();
    fsync_parent_best_effort(path);
    return {};
  }

  return std::unexpected(file_error(ErrorCategory::Io, "failed to create unique temporary " + describe(description), path));
}

}  // namespace ava::core
