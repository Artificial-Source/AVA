#include "sys.h"
#include "ava/session/record.h"
#include "ava/session/session_store_internal.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ava::session {
namespace {

using detail::anchored_child_diagnostic_path;
using detail::append_mutex_for_path;
using detail::path_io_error;
using detail::same_file_identity;

ava::core::Result<std::string> rollback_quarantine_name(std::string_view session_name)
{
  std::array<unsigned char, 16> random_bytes{};
  std::size_t offset = 0;
  while (offset < random_bytes.size())
  {
    auto const count = ::getrandom(random_bytes.data() + offset, random_bytes.size() - offset, 0);
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(path_io_error("failed to obtain a CSPRNG rollback quarantine name", std::filesystem::path(session_name), errno));
    }
    if (count == 0)
      return std::unexpected(path_io_error("failed to obtain a complete CSPRNG rollback quarantine name", std::filesystem::path(session_name), EIO));
    offset += static_cast<std::size_t>(count);
  }

  constexpr char hex[] = "0123456789abcdef";
  std::string suffix;
  suffix.reserve(random_bytes.size() * 2);
  for (auto const byte : random_bytes)
  {
    suffix.push_back(hex[byte >> 4]);
    suffix.push_back(hex[byte & 0x0F]);
  }
  return "." + std::string(session_name) + ".rollback." + suffix;
}

int renameat2_no_replace(int parent_fd, std::string const& source_name, std::string const& destination_name)
{
  return static_cast<int>(::syscall(SYS_renameat2, parent_fd, source_name.c_str(), parent_fd, destination_name.c_str(), RENAME_NOREPLACE));
}

bool renameat2_no_replace_unavailable(int error_number)
{
  return error_number == ENOSYS || error_number == EINVAL || error_number == EOPNOTSUPP;
}

}  // namespace

ava::core::VoidResult SessionStore::remove_created_file(SessionLease const& lease) const
{
  if (ephemeral_state_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "ephemeral sessions do not have a persistent file to remove"));
  if (auto valid_session_id = validate_session_id(options_r()->session_id); !valid_session_id)
    return valid_session_id;
  if (lease.fd_ < 0 || lease.canonical_path_.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "created session cleanup requires an active session lease"));
  if (!lease.created_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session cleanup lease did not create this session file"));

  auto const path = session_path();
  std::error_code absolute_error;
  auto absolute_path = std::filesystem::absolute(path, absolute_error).lexically_normal();
  if (absolute_error)
    absolute_path = path.lexically_normal();
  if (absolute_path != lease.canonical_path_)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session lease does not match the created cleanup target");
    error.with_context("path", absolute_path.string()).with_context("lease_path", lease.canonical_path_.string());
    return std::unexpected(std::move(error));
  }

  auto const parent_path = path.parent_path();
  auto const session_name = path.filename().string();
  if (session_name.empty())
    return std::unexpected(path_io_error("created session cleanup target has no basename", path));

  auto& append_mutex = append_mutex_for_path(path);
  std::lock_guard append_lock(append_mutex);
  struct stat lease_status{};
  if (fstat(lease.fd_, &lease_status) != 0)
    return std::unexpected(path_io_error("failed to inspect the created session lease before cleanup", path, errno));
  if (!S_ISREG(lease_status.st_mode) || lease_status.st_nlink != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "created session cleanup requires one regular linked lease target");
    error.with_context("path", path.string()).with_context("link_count", std::to_string(lease_status.st_nlink));
    return std::unexpected(std::move(error));
  }

  int parent_fd = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (parent_fd < 0)
    return std::unexpected(path_io_error("failed to open the session directory for created-session rollback", parent_path, errno));
  auto finish_error = [&](ava::core::Error error, bool sync_parent) -> ava::core::VoidResult {
    if (parent_fd >= 0)
    {
      if (sync_parent && fsync(parent_fd) != 0)
        error.with_context("rollback_directory_sync_cause", std::strerror(errno));
      if (::close(parent_fd) != 0)
        error.with_context("rollback_directory_close_cause", std::strerror(errno));
      parent_fd = -1;
    }
    return std::unexpected(std::move(error));
  };

  struct stat named_status{};
  if (fstatat(parent_fd, session_name.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) != 0)
    return finish_error(path_io_error("failed to inspect the created session name before cleanup", path, errno), false);
  if (!same_file_identity(lease_status, named_status) || named_status.st_nlink != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "created session name no longer identifies the leased cleanup target");
    error.with_context("path", path.string()).with_context("link_count", std::to_string(named_status.st_nlink));
    return finish_error(std::move(error), false);
  }

  if (before_created_file_rollback_detach_for_test_)
  {
    try
    {
      before_created_file_rollback_detach_for_test_();
    }
    catch (...)
    {
      return finish_error(ava::core::Error(ava::core::ErrorCategory::Unknown, "created-session rollback pre-detach test hook failed"), false);
    }
  }

  std::string quarantine_name;
  bool detached = false;
  for (int attempt = 0; attempt < 16; ++attempt)
  {
    auto generated_name = rollback_quarantine_name(session_name);
    if (!generated_name)
    {
      auto error = std::move(generated_name.error());
      error.with_context("path", path.string());
      return finish_error(std::move(error), false);
    }
    quarantine_name = std::move(*generated_name);
    if (renameat2_no_replace(parent_fd, session_name, quarantine_name) == 0)
    {
      detached = true;
      break;
    }

    int const error_number = errno;
    if (error_number == EEXIST)
      continue;
    if (renameat2_no_replace_unavailable(error_number))
    {
      auto error = path_io_error("Linux renameat2 RENAME_NOREPLACE is unavailable for created-session rollback", path, error_number);
      return finish_error(std::move(error), false);
    }
    return finish_error(path_io_error("failed to detach the created session name for rollback", path, error_number), false);
  }
  if (!detached)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to allocate a unique created-session rollback quarantine path");
    error.with_context("path", path.string());
    return finish_error(std::move(error), false);
  }

  auto const quarantine_path = anchored_child_diagnostic_path(parent_fd, quarantine_name, parent_path);
  auto restore_or_preserve = [&](ava::core::Error error) -> ava::core::VoidResult {
    if (renameat2_no_replace(parent_fd, quarantine_name, session_name) == 0)
    {
      error.with_context("rollback_restored_path", path.string());
      return finish_error(std::move(error), true);
    }

    int const restore_error = errno;
    error.with_context("quarantine_path", quarantine_path.string());
    error.with_context("rollback_restore_cause", std::strerror(restore_error));
    if (renameat2_no_replace_unavailable(restore_error))
      error.with_context("rollback_restore_requirement", "Linux renameat2 RENAME_NOREPLACE");
    return finish_error(std::move(error), true);
  };

  if (after_created_file_rollback_detach_for_test_)
  {
    try
    {
      after_created_file_rollback_detach_for_test_();
    }
    catch (...)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "created-session rollback post-detach test hook failed");
      error.with_context("quarantine_path", quarantine_path.string());
      return finish_error(std::move(error), true);
    }
  }

  int detached_fd = ::openat(parent_fd, quarantine_name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
  if (detached_fd < 0)
  {
    auto error = path_io_error("failed to open the detached created session for rollback verification", quarantine_path, errno);
    return restore_or_preserve(std::move(error));
  }
  struct stat detached_status{};
  int detached_status_error = 0;
  if (fstat(detached_fd, &detached_status) != 0)
    detached_status_error = errno;
  int detached_close_error = 0;
  if (::close(detached_fd) != 0)
    detached_close_error = errno;
  detached_fd = -1;
  if (detached_status_error != 0)
  {
    auto error = path_io_error("failed to inspect the detached created session for rollback verification", quarantine_path, detached_status_error);
    return restore_or_preserve(std::move(error));
  }
  if (detached_close_error != 0)
  {
    auto error = path_io_error("failed to close the detached created session after rollback verification", quarantine_path, detached_close_error);
    error.with_context("quarantine_path", quarantine_path.string());
    return finish_error(std::move(error), true);
  }

  struct stat current_lease_status{};
  if (fstat(lease.fd_, &current_lease_status) != 0)
  {
    auto error = path_io_error("failed to revalidate the created session lease after rollback detachment", path, errno);
    return restore_or_preserve(std::move(error));
  }
  if (!same_file_identity(current_lease_status, detached_status) || current_lease_status.st_nlink != 1 || detached_status.st_nlink != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "detached session name does not identify the creating lease target");
    error.with_context("path", path.string())
        .with_context("quarantine_path", quarantine_path.string())
        .with_context("lease_link_count", std::to_string(current_lease_status.st_nlink))
        .with_context("detached_link_count", std::to_string(detached_status.st_nlink));
    return restore_or_preserve(std::move(error));
  }

  struct stat republished_status{};
  if (fstatat(parent_fd, session_name.c_str(), &republished_status, AT_SYMLINK_NOFOLLOW) == 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "created session name was republished after rollback detachment");
    error.with_context("path", path.string()).with_context("quarantine_path", quarantine_path.string());
    return finish_error(std::move(error), true);
  }
  if (errno != ENOENT)
  {
    auto error = path_io_error("failed to check for created-session republication after rollback detachment", path, errno);
    error.with_context("quarantine_path", quarantine_path.string());
    return finish_error(std::move(error), true);
  }

  struct stat final_lease_status{};
  if (fstat(lease.fd_, &final_lease_status) != 0)
  {
    auto error = path_io_error("failed to revalidate the created session lease before rollback unlink", path, errno);
    error.with_context("quarantine_path", quarantine_path.string());
    return finish_error(std::move(error), true);
  }
  struct stat final_detached_status{};
  if (fstatat(parent_fd, quarantine_name.c_str(), &final_detached_status, AT_SYMLINK_NOFOLLOW) != 0)
  {
    auto error = path_io_error("failed to revalidate the detached created session before rollback unlink", quarantine_path, errno);
    error.with_context("quarantine_path", quarantine_path.string());
    return finish_error(std::move(error), true);
  }
  if (!same_file_identity(final_lease_status, final_detached_status) || final_lease_status.st_nlink != 1 || final_detached_status.st_nlink != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "detached created session changed before rollback unlink");
    error.with_context("path", path.string())
        .with_context("quarantine_path", quarantine_path.string())
        .with_context("lease_link_count", std::to_string(final_lease_status.st_nlink))
        .with_context("detached_link_count", std::to_string(final_detached_status.st_nlink));
    return finish_error(std::move(error), true);
  }

  if (::unlinkat(parent_fd, quarantine_name.c_str(), 0) != 0)
  {
    auto error = path_io_error("failed to remove the detached created session during rollback", quarantine_path, errno);
    error.with_context("quarantine_path", quarantine_path.string());
    return finish_error(std::move(error), true);
  }

  struct stat unlinked_status{};
  if (fstat(lease.fd_, &unlinked_status) != 0)
  {
    auto error = path_io_error("created session was removed but its lease could not be revalidated", path, errno);
    error.with_context("quarantine_path", quarantine_path.string());
    return finish_error(std::move(error), true);
  }
  if (unlinked_status.st_nlink != 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "created session cleanup left an unexpected linked name");
    error.with_context("path", path.string()).with_context("link_count", std::to_string(unlinked_status.st_nlink));
    return finish_error(std::move(error), true);
  }
  if (fsync(parent_fd) != 0)
  {
    auto error = path_io_error("created session was removed but the rollback directory sync failed", parent_path, errno);
    error.with_context("quarantine_path", quarantine_path.string());
    return finish_error(std::move(error), false);
  }
  if (::close(parent_fd) != 0)
  {
    int const close_error = errno;
    parent_fd = -1;
    auto error = path_io_error("created session was removed but the rollback directory close failed", parent_path, close_error);
    error.with_context("quarantine_path", quarantine_path.string());
    return std::unexpected(std::move(error));
  }
  parent_fd = -1;
  return {};
}

}  // namespace ava::session
