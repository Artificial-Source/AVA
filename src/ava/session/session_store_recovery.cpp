#include "sys.h"
#include "ava/session/assistant_output.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/session/session_store_internal.h"
#include "ava/core/fd_io.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::session {

namespace {

using detail::anchored_child_diagnostic_path;
using detail::append_mutex_for_path;
using detail::parse_strict_session_record;
using detail::path_io_error;
using detail::same_directory_identity;
using detail::same_file_identity;
using detail::strict_session_record_error;
using detail::validate_read_limits;

ava::core::VoidResult check_recovery_cancellation(SessionCancelCallback const& cancel_requested)
{
  if (cancel_requested && cancel_requested())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session recovery canceled"));
  return {};
}

using ava::core::normalized_absolute_path;

bool unchanged_file_snapshot(struct stat const& current, struct stat const& initial)
{
  return same_file_identity(current, initial) && current.st_size == initial.st_size && current.st_mtim.tv_sec == initial.st_mtim.tv_sec &&
         current.st_mtim.tv_nsec == initial.st_mtim.tv_nsec && current.st_ctim.tv_sec == initial.st_ctim.tv_sec &&
         current.st_ctim.tv_nsec == initial.st_ctim.tv_nsec;
}

ava::core::VoidResult require_unchanged_repair_namespace(int repair_fd, int parent_fd, struct stat const& initial_file_status,
                                                         struct stat const& anchored_parent_status, std::filesystem::path const& parent_path,
                                                         std::string const& session_name, std::filesystem::path const& diagnostic_path)
{
  struct stat descriptor_status{};
  if (fstat(repair_fd, &descriptor_status) != 0)
    return std::unexpected(path_io_error("failed to revalidate session descriptor before torn tail repair", diagnostic_path, errno));
  if (!unchanged_file_snapshot(descriptor_status, initial_file_status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session changed while preparing torn tail repair");
    error.with_context("path", diagnostic_path.string());
    return std::unexpected(std::move(error));
  }

  struct stat named_status{};
  if (fstatat(parent_fd, session_name.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) != 0)
    return std::unexpected(path_io_error("failed to revalidate the session name before torn tail repair", diagnostic_path, errno));
  if (!unchanged_file_snapshot(named_status, initial_file_status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session name no longer identifies the recovery target");
    error.with_context("path", diagnostic_path.string());
    return std::unexpected(std::move(error));
  }

  int namespace_fd = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (namespace_fd < 0)
    return std::unexpected(path_io_error("failed to re-open the session directory before torn tail repair", parent_path, errno));
  struct stat namespace_status{};
  int namespace_error = 0;
  if (fstat(namespace_fd, &namespace_status) != 0)
    namespace_error = errno;
  int close_error = 0;
  if (::close(namespace_fd) != 0)
    close_error = errno;
  if (namespace_error != 0)
    return std::unexpected(path_io_error("failed to inspect the re-opened session directory before torn tail repair", parent_path, namespace_error));
  if (close_error != 0)
    return std::unexpected(path_io_error("failed to close the re-opened session directory before torn tail repair", parent_path, close_error));
  if (!same_directory_identity(namespace_status, anchored_parent_status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session directory namespace changed while preparing torn tail repair");
    error.with_context("path", parent_path.string());
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::Result<std::filesystem::path> quarantine_session_suffix(int parent_fd, std::filesystem::path const& parent_path, std::string const& session_name,
                                                                   std::string_view recovery_kind, std::string_view suffix,
                                                                   SessionCancelCallback const& cancel_requested,
                                                                   std::function<void(std::filesystem::path const&)> const* before_publication_for_test)
{
  for (int attempt = 0; attempt < 16; ++attempt)
  {
    auto const unique = ava::core::make_id("quarantine");
    auto const final_name = session_name + "." + std::string(recovery_kind) + "." + unique + ".bin";
    auto const temporary_name = "." + session_name + "." + std::string(recovery_kind) + ".tmp." + unique;
    auto const final_path = anchored_child_diagnostic_path(parent_fd, final_name, parent_path);
    auto const temporary_path = anchored_child_diagnostic_path(parent_fd, temporary_name, parent_path);

    ava::core::UniqueFd quarantine_fd(::openat(parent_fd, temporary_name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (quarantine_fd.get() < 0)
    {
      if (errno == EEXIST)
        continue;
      return std::unexpected(path_io_error("failed to create temporary session recovery quarantine", temporary_path, errno));
    }

    struct stat initial_status{};
    if (fstat(quarantine_fd.get(), &initial_status) != 0)
    {
      int const error_number = errno;
      int const close_error = quarantine_fd.close_checked();
      auto error = path_io_error("failed to inspect newly created session recovery quarantine", temporary_path, error_number);
      error.with_context("quarantine_target", final_path.string());
      if (close_error != 0)
        error.with_context("quarantine_close_cause", std::strerror(close_error));
      return std::unexpected(std::move(error));
    }
    auto same_quarantine_metadata = [](struct stat const& left, struct stat const& right) {
      return same_file_identity(left, right) && left.st_uid == right.st_uid && left.st_gid == right.st_gid && left.st_mode == right.st_mode &&
             left.st_size == right.st_size;
    };
    auto cleanup_temporary_if_owned = [&]() -> int {
      struct stat named_status{};
      if (fstatat(parent_fd, temporary_name.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? 0 : errno;
      if (!same_file_identity(initial_status, named_status))
        return ESTALE;
      return ::unlinkat(parent_fd, temporary_name.c_str(), 0) == 0 ? 0 : errno;
    };
    auto abandon_prepublication = [&](ava::core::Error error) -> ava::core::Result<std::filesystem::path> {
      int const cleanup_error = cleanup_temporary_if_owned();
      int const close_error = quarantine_fd.close_checked();
      error.with_context("quarantine_target", final_path.string());
      if (cleanup_error != 0)
        error.with_context("temporary_cleanup", cleanup_error == ESTALE ? "temporary name changed; replacement preserved" : std::strerror(cleanup_error));
      if (close_error != 0)
        error.with_context("quarantine_close_cause", std::strerror(close_error));
      return std::unexpected(std::move(error));
    };
    auto prepublication_io_failure = [&](std::string message, int error_number) {
      return abandon_prepublication(path_io_error(std::move(message), temporary_path, error_number));
    };
    auto published_failure = [&](std::string message, int error_number = 0) -> ava::core::Result<std::filesystem::path> {
      auto error = error_number == 0 ? ava::core::Error(ava::core::ErrorCategory::PermissionDenied, std::move(message))
                                     : path_io_error(std::move(message), final_path, error_number);
      error.with_context("quarantine_path", final_path.string()).with_context("temporary_path", temporary_path.string());
      int const close_error = quarantine_fd.close_checked();
      if (close_error != 0)
        error.with_context("quarantine_close_cause", std::strerror(close_error));
      return std::unexpected(std::move(error));
    };
    auto descriptor_content_matches = [&]() -> bool {
      if (initial_status.st_size < 0 || static_cast<std::uintmax_t>(initial_status.st_size) != suffix.size())
        return false;
      std::array<char, 8192> buffer{};
      std::size_t offset = 0;
      while (offset < suffix.size())
      {
        auto const wanted = std::min(buffer.size(), suffix.size() - offset);
        ssize_t count = 0;
        do
        {
          count = ::pread(quarantine_fd.get(), buffer.data(), wanted, static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count <= 0 || std::string_view(buffer.data(), static_cast<std::size_t>(count)) != suffix.substr(offset, static_cast<std::size_t>(count)))
          return false;
        offset += static_cast<std::size_t>(count);
      }
      return true;
    };

    if (fchmod(quarantine_fd.get(), 0600) != 0)
      return prepublication_io_failure("failed to set session recovery quarantine permissions", errno);
    if (fstat(quarantine_fd.get(), &initial_status) != 0)
      return prepublication_io_failure("failed to inspect temporary session recovery quarantine", errno);
    if (!S_ISREG(initial_status.st_mode) || (initial_status.st_mode & 07777) != 0600 || initial_status.st_uid != geteuid() || initial_status.st_nlink != 1)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "temporary session recovery quarantine has unsafe metadata");
      error.with_context("mode", std::to_string(initial_status.st_mode & 07777))
          .with_context("owner", std::to_string(initial_status.st_uid))
          .with_context("link_count", std::to_string(initial_status.st_nlink));
      return abandon_prepublication(std::move(error));
    }
    if (int const write_error = ava::core::write_all(quarantine_fd.get(), suffix); write_error != 0)
      return prepublication_io_failure("failed to write session recovery quarantine", write_error);
    if (int const sync_error = ava::core::fdatasync_with_restart(quarantine_fd.get()); sync_error != 0)
      return prepublication_io_failure("failed to sync session recovery quarantine", sync_error);
    if (fstat(quarantine_fd.get(), &initial_status) != 0)
      return prepublication_io_failure("failed to re-inspect temporary session recovery quarantine", errno);

    if (before_publication_for_test && *before_publication_for_test)
    {
      try
      {
        (*before_publication_for_test)(temporary_path);
      }
      catch (...)
      {
        return abandon_prepublication(ava::core::Error(ava::core::ErrorCategory::Unknown, "session recovery quarantine prepublication test hook failed"));
      }
    }
    if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
      return abandon_prepublication(std::move(canceled.error()));

    struct stat prepublication_status{};
    if (fstat(quarantine_fd.get(), &prepublication_status) != 0)
      return prepublication_io_failure("failed to verify session recovery quarantine before publication", errno);
    if (!same_quarantine_metadata(initial_status, prepublication_status) || prepublication_status.st_nlink != 1 || !descriptor_content_matches())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                    "session recovery quarantine inode or content changed before descriptor-anchored publication");
      error.with_context("link_count", std::to_string(prepublication_status.st_nlink));
      return abandon_prepublication(std::move(error));
    }

    int publication_error = 0;
    int empty_path_error = 0;
    if (::linkat(quarantine_fd.get(), "", parent_fd, final_name.c_str(), AT_EMPTY_PATH) != 0)
    {
      publication_error = errno;
      if (publication_error == ENOENT || publication_error == EPERM)
      {
        // Older kernels require CAP_DAC_READ_SEARCH for AT_EMPTY_PATH. The
        // link(2) procfs alternative still links this exact open descriptor.
        empty_path_error = publication_error;
        auto const descriptor_path = ava::core::descriptor_proc_path(quarantine_fd.get());
        publication_error = ::linkat(AT_FDCWD, descriptor_path.c_str(), parent_fd, final_name.c_str(), AT_SYMLINK_FOLLOW) == 0 ? 0 : errno;
      }
    }
    if (publication_error != 0)
    {
      auto error = path_io_error("failed to publish exact session recovery quarantine inode", temporary_path, publication_error);
      if (empty_path_error != 0)
      {
        error.with_context("session_path", (parent_path / session_name).string())
            .with_context("at_empty_path_cause", std::strerror(empty_path_error))
            .with_context("proc_self_fd_cause", std::strerror(publication_error))
            .with_context("recovery", "ensure /proc/self/fd is accessible and permits linking the open quarantine descriptor");
      }
      auto failed = abandon_prepublication(std::move(error));
      if (publication_error == EEXIST)
        continue;
      return failed;
    }

    struct stat linked_status{};
    struct stat final_status{};
    if (fstat(quarantine_fd.get(), &linked_status) != 0)
      return published_failure("failed to inspect published session recovery quarantine", errno);
    if (fstatat(parent_fd, final_name.c_str(), &final_status, AT_SYMLINK_NOFOLLOW) != 0)
      return published_failure("failed to inspect final session recovery quarantine name", errno);
    if (!same_quarantine_metadata(initial_status, linked_status) || !same_quarantine_metadata(initial_status, final_status) || linked_status.st_nlink != 2 ||
        final_status.st_nlink != 2 || !descriptor_content_matches())
    {
      return published_failure("published session recovery quarantine does not identify the exact validated inode");
    }

    struct stat temporary_status{};
    if (fstatat(parent_fd, temporary_name.c_str(), &temporary_status, AT_SYMLINK_NOFOLLOW) != 0)
      return published_failure("published session recovery quarantine temporary name disappeared before verified cleanup", errno);
    if (!same_quarantine_metadata(initial_status, temporary_status) || temporary_status.st_nlink != 2)
      return published_failure("session recovery quarantine temporary name was replaced; replacement preserved");
    if (::unlinkat(parent_fd, temporary_name.c_str(), 0) != 0)
      return published_failure("session recovery quarantine was published but its verified temporary name could not be removed", errno);

    struct stat committed_status{};
    struct stat committed_final_status{};
    if (fstat(quarantine_fd.get(), &committed_status) != 0 || fstatat(parent_fd, final_name.c_str(), &committed_final_status, AT_SYMLINK_NOFOLLOW) != 0)
    {
      return published_failure("failed to verify committed session recovery quarantine", errno);
    }
    if (!same_quarantine_metadata(initial_status, committed_status) || !same_quarantine_metadata(initial_status, committed_final_status) ||
        committed_status.st_nlink != 1 || committed_final_status.st_nlink != 1 || !descriptor_content_matches())
    {
      return published_failure("committed session recovery quarantine failed inode, metadata, link-count, or content verification");
    }

    int const close_error = quarantine_fd.close_checked();
    int const sync_error = ava::core::fsync_with_restart(parent_fd);
    if (close_error != 0)
    {
      auto error = path_io_error("session recovery quarantine was published but its descriptor close failed", final_path, close_error);
      error.with_context("quarantine_path", final_path.string());
      if (sync_error != 0)
        error.with_context("directory_sync_cause", std::strerror(sync_error));
      return std::unexpected(std::move(error));
    }
    if (sync_error != 0)
    {
      auto error = path_io_error("session recovery quarantine was published but the session directory sync failed", final_path, sync_error);
      error.with_context("quarantine_path", final_path.string());
      return std::unexpected(std::move(error));
    }
    return final_path;
  }

  auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to allocate a unique session recovery quarantine path");
  error.with_context("path", (parent_path / session_name).string());
  return std::unexpected(std::move(error));
}

}  // namespace

void SessionStore::set_before_recovery_quarantine_publication_for_test(std::function<void(std::filesystem::path const&)> hook)
{
  before_recovery_quarantine_publication_for_test_ = std::move(hook);
}

void SessionStore::set_after_recovery_quarantine_publication_for_test(std::function<void()> hook)
{
  after_recovery_quarantine_publication_for_test_ = std::move(hook);
}

ava::core::Result<std::optional<std::filesystem::path>> SessionStore::recover_torn_tail(SessionLease const& lease, SessionReadLimits limits,
                                                                                        SessionCancelCallback cancel_requested) const
{
  if (ephemeral_state_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "ephemeral sessions do not support torn tail recovery"));
  if (auto valid_limits = validate_read_limits(limits); !valid_limits)
    return std::unexpected(std::move(valid_limits.error()));
  if (auto valid_session_id = validate_session_id(options_r()->session_id); !valid_session_id)
    return std::unexpected(std::move(valid_session_id.error()));
  if (lease.fd_ < 0 || lease.canonical_path_.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "torn tail recovery requires an active session lease"));
  if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
    return std::unexpected(std::move(canceled.error()));

  auto const path = session_path();
  auto const parent_path = path.parent_path();
  auto const session_name = path.filename().string();
  auto& append_mutex = append_mutex_for_path(path);
  std::lock_guard append_lock(append_mutex);

  ava::core::UniqueFd parent_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (parent_fd.get() < 0)
    return std::unexpected(path_io_error("failed to open the session directory for torn tail recovery", parent_path, errno));
  ava::core::UniqueFd repair_fd;
  auto close_fd = [](ava::core::UniqueFd& fd) { return fd.close_checked(); };
  auto close_both_best_effort = [&] {
    static_cast<void>(close_fd(repair_fd));
    static_cast<void>(close_fd(parent_fd));
  };

  struct stat parent_status{};
  if (fstat(parent_fd.get(), &parent_status) != 0 || !S_ISDIR(parent_status.st_mode))
  {
    int const error_number = errno;
    close_both_best_effort();
    return std::unexpected(path_io_error("session recovery parent is not a directory", parent_path, error_number));
  }

  repair_fd.reset(::openat(parent_fd.get(), session_name.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
  if (repair_fd.get() < 0)
  {
    int const error_number = errno;
    close_both_best_effort();
    return std::unexpected(path_io_error("failed to open the original session path for torn tail recovery", path, error_number));
  }

  struct stat lease_status{};
  struct stat repair_status{};
  if (fstat(lease.fd_, &lease_status) != 0)
  {
    auto error = path_io_error("failed to verify session lease for torn tail recovery", lease.canonical_path_, errno);
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (fstat(repair_fd.get(), &repair_status) != 0)
  {
    auto error = path_io_error("failed to inspect session opened for torn tail recovery", path, errno);
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (!same_file_identity(lease_status, repair_status))
  {
    close_both_best_effort();
    auto error = ava::core::Error(ava::core::ErrorCategory::Session,
                                  "session lease does not match the store because its descriptor does not identify the recovery target");
    error.with_context("path", path.string()).with_context("lease_path", lease.canonical_path_.string());
    return std::unexpected(std::move(error));
  }
  if (repair_status.st_size < 0)
  {
    close_both_best_effort();
    return std::unexpected(path_io_error("session size is invalid during torn tail recovery", path));
  }
  if (static_cast<std::uintmax_t>(repair_status.st_size) > limits.max_file_bytes)
  {
    close_both_best_effort();
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session file exceeds torn tail recovery byte limit");
    error.with_context("path", path.string()).with_context("max_file_bytes", std::to_string(limits.max_file_bytes));
    return std::unexpected(std::move(error));
  }

  auto close_without_repair = [&]() -> ava::core::Result<std::optional<std::filesystem::path>> {
    int const repair_close_error = close_fd(repair_fd);
    int const parent_close_error = close_fd(parent_fd);
    if (repair_close_error != 0)
      return std::unexpected(path_io_error("failed to close complete session during torn tail recovery", path, repair_close_error));
    if (parent_close_error != 0)
      return std::unexpected(path_io_error("failed to close session directory during torn tail recovery", parent_path, parent_close_error));
    return std::optional<std::filesystem::path>{};
  };
  if (repair_status.st_size == 0)
    return close_without_repair();

  char final_byte = '\0';
  ssize_t final_byte_count = 0;
  do
  {
    final_byte_count = ::pread(repair_fd.get(), &final_byte, 1, repair_status.st_size - 1);
  } while (final_byte_count < 0 && errno == EINTR);
  if (final_byte_count != 1)
  {
    int const error_number = final_byte_count < 0 ? errno : EIO;
    close_both_best_effort();
    return std::unexpected(path_io_error("failed to inspect final session byte during torn tail recovery", path, error_number));
  }
  if (final_byte == '\n')
    return close_without_repair();

  std::unordered_set<std::string> entry_ids;
  entry_ids.reserve(std::min<std::size_t>(limits.max_entries, 1024));
  std::size_t entry_count = 0;
  auto validate_entry_integrity = [&](SessionEntry const& entry) -> ava::core::VoidResult {
    if (entry_count >= limits.max_entries)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry count exceeds torn tail recovery limit");
      error.with_context("path", path.string()).with_context("max_entries", std::to_string(limits.max_entries));
      return std::unexpected(std::move(error));
    }
    if (entry_ids.contains(entry.id))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session recovery found a duplicate entry id");
      error.with_context("path", path.string()).with_context("entry_id", entry.id);
      return std::unexpected(std::move(error));
    }
    if (!entry.parent_id.empty() && !entry_ids.contains(entry.parent_id))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session recovery found a parent id that does not name an earlier record");
      error.with_context("path", path.string()).with_context("entry_id", entry.id).with_context("parent_id", entry.parent_id);
      return std::unexpected(std::move(error));
    }
    entry_ids.insert(entry.id);
    ++entry_count;
    return {};
  };

  std::array<char, 8192> buffer{};
  std::string line;
  line.reserve(std::min<std::size_t>(limits.max_line_bytes, buffer.size()));
  off_t offset = 0;
  off_t last_validated_newline = 0;
  while (offset < repair_status.st_size)
  {
    if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
    {
      close_both_best_effort();
      return std::unexpected(std::move(canceled.error()));
    }
    auto const remaining = repair_status.st_size - offset;
    auto const wanted = remaining < static_cast<off_t>(buffer.size()) ? static_cast<std::size_t>(remaining) : buffer.size();
    ssize_t count = 0;
    do
    {
      count = ::pread(repair_fd.get(), buffer.data(), wanted, offset);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
    {
      auto error = path_io_error("failed while scanning session for torn tail recovery", path, errno);
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    if (count == 0)
    {
      auto error = path_io_error("session became unreadable while scanning for torn tail recovery", path);
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    for (ssize_t index = 0; index < count; ++index)
    {
      char const ch = buffer[static_cast<std::size_t>(index)];
      if (ch == '\n')
      {
        if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
        {
          close_both_best_effort();
          return std::unexpected(std::move(canceled.error()));
        }
        auto entry = parse_strict_session_record(line, path, false);
        if (!entry)
        {
          auto error = std::move(entry.error());
          error.with_context("record_end_offset", std::to_string(offset + index + 1));
          close_both_best_effort();
          return std::unexpected(std::move(error));
        }
        if (auto valid_integrity = validate_entry_integrity(*entry); !valid_integrity)
        {
          auto error = std::move(valid_integrity.error());
          error.with_context("record_end_offset", std::to_string(offset + index + 1));
          close_both_best_effort();
          return std::unexpected(std::move(error));
        }
        line.clear();
        last_validated_newline = offset + index + 1;
      }
      else
      {
        if (line.size() >= limits.max_line_bytes || line.size() + 1 >= kMaxSessionLineBytes)
        {
          close_both_best_effort();
          auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session line exceeds torn tail recovery scan limit");
          error.with_context("path", path.string()).with_context("max_line_bytes", std::to_string(limits.max_line_bytes));
          return std::unexpected(std::move(error));
        }
        line.push_back(ch);
      }
    }
    offset += count;
  }

  if (line.empty())
  {
    int const repair_close_error = close_fd(repair_fd);
    int const parent_close_error = close_fd(parent_fd);
    if (repair_close_error != 0)
      return std::unexpected(path_io_error("failed to close session after torn tail scan", path, repair_close_error));
    if (parent_close_error != 0)
      return std::unexpected(path_io_error("failed to close session directory after torn tail scan", parent_path, parent_close_error));
    return std::optional<std::filesystem::path>{};
  }

  auto const strict_status = ava::core::validate_strict_json(line, ava::core::json::kMaxNestingDepth);
  bool const append_final_lf = strict_status == ava::core::StrictJsonStatus::Valid;
  if (append_final_lf)
  {
    auto entry = parse_strict_session_record(line, path, true);
    if (!entry)
    {
      auto error = std::move(entry.error());
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    if (auto valid_integrity = validate_entry_integrity(*entry); !valid_integrity)
    {
      auto error = std::move(valid_integrity.error());
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
  }
  else if (strict_status != ava::core::StrictJsonStatus::Invalid)
  {
    auto error = strict_session_record_error(strict_status, path, true);
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }

  if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
  {
    close_both_best_effort();
    return std::unexpected(std::move(canceled.error()));
  }
  if (auto unchanged = require_unchanged_repair_namespace(repair_fd.get(), parent_fd.get(), repair_status, parent_status, parent_path, session_name, path);
      !unchanged)
  {
    auto error = std::move(unchanged.error());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }

  if (append_final_lf)
  {
    ssize_t count = 0;
    do
    {
      count = ::pwrite(repair_fd.get(), "\n", 1, repair_status.st_size);
    } while (count < 0 && errno == EINTR);
    if (count != 1)
    {
      int const error_number = count < 0 ? errno : EIO;
      auto error = path_io_error("failed to append final LF during torn tail recovery", path, error_number);
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    if (int const sync_error = ava::core::fdatasync_with_restart(repair_fd.get()); sync_error != 0)
    {
      auto error = path_io_error("failed to sync final LF during torn tail recovery", path, sync_error);
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    struct stat repaired_status{};
    if (fstat(repair_fd.get(), &repaired_status) != 0)
    {
      auto error = path_io_error("failed to inspect session after appending final LF", path, errno);
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    if (auto unchanged = require_unchanged_repair_namespace(repair_fd.get(), parent_fd.get(), repaired_status, parent_status, parent_path, session_name, path);
        !unchanged)
    {
      auto error = std::move(unchanged.error());
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    int const repair_close_error = close_fd(repair_fd);
    int const parent_close_error = close_fd(parent_fd);
    if (repair_close_error != 0)
      return std::unexpected(path_io_error("failed to close session after torn tail recovery", path, repair_close_error));
    if (parent_close_error != 0)
      return std::unexpected(path_io_error("failed to close session directory after torn tail recovery", parent_path, parent_close_error));
    return std::optional<std::filesystem::path>{};
  }

  auto quarantined = quarantine_session_suffix(parent_fd.get(), parent_path, session_name, "torn-tail", line, cancel_requested,
                                               before_recovery_quarantine_publication_for_test_ ? &before_recovery_quarantine_publication_for_test_ : nullptr);
  if (!quarantined)
  {
    auto error = std::move(quarantined.error());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (after_recovery_quarantine_publication_for_test_)
  {
    try
    {
      after_recovery_quarantine_publication_for_test_();
    }
    catch (...)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "session recovery post-publication test hook failed");
      error.with_context("quarantine_path", quarantined->string());
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
  }
  if (auto unchanged = require_unchanged_repair_namespace(repair_fd.get(), parent_fd.get(), repair_status, parent_status, parent_path, session_name, path);
      !unchanged)
  {
    auto error = std::move(unchanged.error());
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (ftruncate(repair_fd.get(), last_validated_newline) != 0)
  {
    auto error = path_io_error("failed to truncate quarantined torn session tail", path, errno);
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (int const sync_error = ava::core::fdatasync_with_restart(repair_fd.get()); sync_error != 0)
  {
    auto error = path_io_error("session tail was truncated but the source sync failed", path, sync_error);
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  struct stat repaired_status{};
  if (fstat(repair_fd.get(), &repaired_status) != 0)
  {
    auto error = path_io_error("failed to inspect session after truncating its torn tail", path, errno);
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (auto unchanged = require_unchanged_repair_namespace(repair_fd.get(), parent_fd.get(), repaired_status, parent_status, parent_path, session_name, path);
      !unchanged)
  {
    auto error = std::move(unchanged.error());
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  int const repair_close_error = close_fd(repair_fd);
  int const parent_close_error = close_fd(parent_fd);
  if (repair_close_error != 0)
  {
    auto error = path_io_error("failed to close repaired session", path, repair_close_error);
    error.with_context("quarantine_path", quarantined->string());
    return std::unexpected(std::move(error));
  }
  if (parent_close_error != 0)
  {
    auto error = path_io_error("failed to close session directory after torn tail recovery", parent_path, parent_close_error);
    error.with_context("quarantine_path", quarantined->string());
    return std::unexpected(std::move(error));
  }
  return std::optional<std::filesystem::path>(std::move(*quarantined));
}

ava::core::Result<std::optional<AssistantOutputSuffixRecovery>> SessionStore::recover_incomplete_assistant_output_suffix(
    SessionReadLimits limits, SessionCancelCallback cancel_requested) const
{
  return recover_incomplete_assistant_output_suffix_ephemeral_impl(limits, std::move(cancel_requested), false);
}

ava::core::Result<std::optional<AssistantOutputSuffixRecovery>> SessionStore::recover_incomplete_assistant_output_suffix_ephemeral_impl(
    SessionReadLimits limits, SessionCancelCallback cancel_requested, bool mutation_serialization_held) const
{
  if (!ephemeral_state_)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "in-memory assistant-output suffix recovery requires an ephemeral session"));
  }
  if (auto valid_limits = validate_read_limits(limits); !valid_limits)
    return std::unexpected(std::move(valid_limits.error()));

  std::optional<std::unique_lock<std::mutex>> mutation_lock;
  if (!mutation_serialization_held)
    mutation_lock.emplace(ephemeral_state_->mutation_mutex);
  std::lock_guard entries_lock(ephemeral_state_->entries_mutex);
  std::vector<SessionEntry> const& entries = ephemeral_state_->entries;
  std::unordered_set<std::string> entry_ids;
  std::size_t total_bytes = 0;
  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
      return std::unexpected(std::move(canceled.error()));
    if (index >= limits.max_entries)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "ephemeral session entry count exceeds assistant-output recovery limit");
      error.with_context("max_entries", std::to_string(limits.max_entries));
      return std::unexpected(std::move(error));
    }
    if (entry_ids.contains(entries[index].id) || (!entries[index].parent_id.empty() && !entry_ids.contains(entries[index].parent_id)))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "ephemeral assistant-output recovery found malformed session entry identity history");
      error.with_context("entry_id", entries[index].id);
      return std::unexpected(std::move(error));
    }
    entry_ids.insert(entries[index].id);
    auto serialized = serialize_session_entry_line(entries[index]);
    if (!serialized)
      return std::unexpected(std::move(serialized.error()));
    if (serialized->size() > limits.max_line_bytes || serialized->size() + 1 > limits.max_file_bytes - total_bytes)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "ephemeral session exceeds assistant-output recovery read limit");
      error.with_context("max_file_bytes", std::to_string(limits.max_file_bytes)).with_context("max_line_bytes", std::to_string(limits.max_line_bytes));
      return std::unexpected(std::move(error));
    }
    total_bytes += serialized->size() + 1;
  }

  auto const projection = classify_assistant_output(entries);
  std::optional<std::size_t> suffix_start;
  for (auto const& diagnostic : projection.diagnostics)
  {
    if (diagnostic.severity == AssistantOutputDiagnosticSeverity::Warning && diagnostic.kind == AssistantOutputDiagnosticKind::IncompleteAssistantTurn)
    {
      if (suffix_start || diagnostic.entry_index >= entries.size())
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "ephemeral assistant-output recovery found an ambiguous staging suffix"));
      suffix_start = diagnostic.entry_index;
      continue;
    }
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "ephemeral assistant-output history is malformed; refusing suffix recovery");
    error.with_context("diagnostic_kind", std::string(to_string(diagnostic.kind)))
        .with_context("diagnostic_entry_id", diagnostic.entry_id)
        .with_context("diagnostic", diagnostic.message);
    return std::unexpected(std::move(error));
  }
  if (!suffix_start)
    return std::optional<AssistantOutputSuffixRecovery>{};
  for (std::size_t index = *suffix_start; index < entries.size(); ++index)
  {
    if (entries[index].type != EntryType::AssistantOutputItem)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "ephemeral assistant-output recovery suffix contains an unrelated entry"));
    }
  }

  std::size_t removed_bytes = 0;
  for (std::size_t index = *suffix_start; index < entries.size(); ++index)
  {
    auto serialized = serialize_session_entry_line(entries[index]);
    if (!serialized)
      return std::unexpected(std::move(serialized.error()));
    removed_bytes += serialized->size() + 1;
  }
  auto const removed_entries = entries.size() - *suffix_start;
  ephemeral_state_->entries.erase(ephemeral_state_->entries.begin() + static_cast<std::ptrdiff_t>(*suffix_start), ephemeral_state_->entries.end());
  return std::optional<AssistantOutputSuffixRecovery>(
      AssistantOutputSuffixRecovery{.quarantine_path = std::nullopt, .removed_entry_count = removed_entries, .removed_byte_count = removed_bytes});
}

ava::core::Result<std::optional<AssistantOutputSuffixRecovery>> SessionStore::recover_incomplete_assistant_output_suffix(
    SessionLease const& lease, SessionReadLimits limits, SessionCancelCallback cancel_requested) const
{
  if (ephemeral_state_)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistent assistant-output suffix recovery does not accept an ephemeral session"));
  }
  if (auto valid_limits = validate_read_limits(limits); !valid_limits)
    return std::unexpected(std::move(valid_limits.error()));
  if (auto valid_session_id = validate_session_id(options_r()->session_id); !valid_session_id)
    return std::unexpected(std::move(valid_session_id.error()));
  if (!lease.active() || lease.canonical_path_.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "assistant-output suffix recovery requires an active session lease"));
  }
  auto const path = session_path();
  if (normalized_absolute_path(path) != lease.canonical_path_)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "assistant-output suffix recovery lease does not exactly match the store");
    error.with_context("path", path.string()).with_context("lease_path", lease.canonical_path_.string());
    return std::unexpected(std::move(error));
  }
  if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
    return std::unexpected(std::move(canceled.error()));

  auto const parent_path = path.parent_path();
  auto const session_name = path.filename().string();
  auto& append_mutex = append_mutex_for_path(path);
  std::lock_guard append_lock(append_mutex);
  ava::core::UniqueFd parent_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (parent_fd.get() < 0)
  {
    return std::unexpected(path_io_error("failed to open the session directory for assistant-output suffix recovery", parent_path, errno));
  }
  ava::core::UniqueFd repair_fd(::openat(parent_fd.get(), session_name.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
  if (repair_fd.get() < 0)
  {
    auto error = path_io_error("failed to securely open the original session path for assistant-output suffix recovery", path, errno);
    static_cast<void>(parent_fd.close_checked());
    return std::unexpected(std::move(error));
  }
  auto close_both_best_effort = [&] {
    static_cast<void>(repair_fd.close_checked());
    static_cast<void>(parent_fd.close_checked());
  };
  auto close_without_repair = [&]() -> ava::core::Result<std::optional<AssistantOutputSuffixRecovery>> {
    int const repair_close_error = repair_fd.close_checked();
    int const parent_close_error = parent_fd.close_checked();
    if (repair_close_error != 0)
      return std::unexpected(path_io_error("failed to close complete session during assistant-output suffix recovery", path, repair_close_error));
    if (parent_close_error != 0)
      return std::unexpected(path_io_error("failed to close session directory during assistant-output suffix recovery", parent_path, parent_close_error));
    return std::optional<AssistantOutputSuffixRecovery>{};
  };

  struct stat parent_status{};
  if (fstat(parent_fd.get(), &parent_status) != 0 || !S_ISDIR(parent_status.st_mode))
  {
    int const error_number = errno;
    close_both_best_effort();
    return std::unexpected(path_io_error("session recovery parent is not a directory", parent_path, error_number));
  }
  struct stat lease_status{};
  struct stat repair_status{};
  if (fstat(lease.fd_, &lease_status) != 0)
  {
    auto error = path_io_error("failed to verify session lease for assistant-output suffix recovery", lease.canonical_path_, errno);
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (fstat(repair_fd.get(), &repair_status) != 0)
  {
    auto error = path_io_error("failed to inspect session opened for assistant-output suffix recovery", path, errno);
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (!same_file_identity(lease_status, repair_status))
  {
    close_both_best_effort();
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session lease does not identify the named assistant-output recovery target");
    error.with_context("path", path.string()).with_context("lease_path", lease.canonical_path_.string());
    return std::unexpected(std::move(error));
  }
  if (repair_status.st_size < 0 || static_cast<std::uintmax_t>(repair_status.st_size) > limits.max_file_bytes)
  {
    close_both_best_effort();
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session file exceeds assistant-output suffix recovery byte limit");
    error.with_context("path", path.string()).with_context("max_file_bytes", std::to_string(limits.max_file_bytes));
    return std::unexpected(std::move(error));
  }
  if (repair_status.st_size == 0)
    return close_without_repair();

  char final_byte = '\0';
  ssize_t final_byte_count = 0;
  do
  {
    final_byte_count = ::pread(repair_fd.get(), &final_byte, 1, repair_status.st_size - 1);
  } while (final_byte_count < 0 && errno == EINTR);
  if (final_byte_count != 1)
  {
    auto error = path_io_error("failed to inspect final session byte during assistant-output suffix recovery", path, final_byte_count < 0 ? errno : EIO);
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (final_byte != '\n')
  {
    close_both_best_effort();
    auto error =
        ava::core::Error(ava::core::ErrorCategory::Session, "assistant-output suffix recovery requires torn-tail recovery before scanning complete records");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }

  struct PhysicalRecord
  {
    off_t start = 0;
    off_t end = 0;
    SessionEntry entry;
  };
  std::vector<PhysicalRecord> records;
  std::unordered_set<std::string> entry_ids;
  std::array<char, 8192> buffer{};
  std::string line;
  line.reserve(std::min<std::size_t>(limits.max_line_bytes, buffer.size()));
  off_t offset = 0;
  off_t line_start = 0;
  while (offset < repair_status.st_size)
  {
    if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
    {
      close_both_best_effort();
      return std::unexpected(std::move(canceled.error()));
    }
    auto const remaining = repair_status.st_size - offset;
    auto const wanted = remaining < static_cast<off_t>(buffer.size()) ? static_cast<std::size_t>(remaining) : buffer.size();
    ssize_t count = 0;
    do
    {
      count = ::pread(repair_fd.get(), buffer.data(), wanted, offset);
    } while (count < 0 && errno == EINTR);
    if (count <= 0)
    {
      auto error = path_io_error("failed while scanning session for assistant-output suffix recovery", path, count < 0 ? errno : EIO);
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    for (ssize_t index = 0; index < count; ++index)
    {
      char const ch = buffer[static_cast<std::size_t>(index)];
      if (ch != '\n')
      {
        if (line.size() >= limits.max_line_bytes || line.size() + 1 >= kMaxSessionLineBytes)
        {
          close_both_best_effort();
          auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session line exceeds assistant-output suffix recovery scan limit");
          error.with_context("path", path.string()).with_context("max_line_bytes", std::to_string(limits.max_line_bytes));
          return std::unexpected(std::move(error));
        }
        line.push_back(ch);
        continue;
      }

      if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
      {
        close_both_best_effort();
        return std::unexpected(std::move(canceled.error()));
      }
      if (records.size() >= limits.max_entries)
      {
        close_both_best_effort();
        auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry count exceeds assistant-output suffix recovery limit");
        error.with_context("path", path.string()).with_context("max_entries", std::to_string(limits.max_entries));
        return std::unexpected(std::move(error));
      }
      auto parsed = parse_strict_session_record(line, path, false);
      if (!parsed)
      {
        auto error = std::move(parsed.error());
        error.with_context("record_end_offset", std::to_string(offset + index + 1));
        close_both_best_effort();
        return std::unexpected(std::move(error));
      }
      if (entry_ids.contains(parsed->id) || (!parsed->parent_id.empty() && !entry_ids.contains(parsed->parent_id)))
      {
        close_both_best_effort();
        auto error = ava::core::Error(ava::core::ErrorCategory::Session, "assistant-output suffix recovery found malformed session entry identity history");
        error.with_context("entry_id", parsed->id).with_context("record_end_offset", std::to_string(offset + index + 1));
        return std::unexpected(std::move(error));
      }
      entry_ids.insert(parsed->id);
      records.push_back(PhysicalRecord{.start = line_start, .end = offset + index + 1, .entry = std::move(*parsed)});
      line.clear();
      line_start = offset + index + 1;
    }
    offset += count;
  }
  if (!line.empty() || records.empty())
  {
    close_both_best_effort();
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::Session, "assistant-output suffix recovery could not establish a complete framed session history"));
  }

  std::vector<SessionEntry> entries;
  entries.reserve(records.size());
  for (auto const& record : records) entries.push_back(record.entry);
  auto const projection = classify_assistant_output(entries);
  std::optional<std::size_t> suffix_start_index;
  for (auto const& diagnostic : projection.diagnostics)
  {
    if (diagnostic.severity == AssistantOutputDiagnosticSeverity::Warning && diagnostic.kind == AssistantOutputDiagnosticKind::IncompleteAssistantTurn)
    {
      if (suffix_start_index || diagnostic.entry_index >= records.size())
      {
        close_both_best_effort();
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "assistant-output suffix recovery found an ambiguous final staging suffix"));
      }
      suffix_start_index = diagnostic.entry_index;
      continue;
    }
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "assistant-output history is malformed; refusing suffix recovery");
    error.with_context("diagnostic_kind", std::string(to_string(diagnostic.kind)))
        .with_context("diagnostic_entry_id", diagnostic.entry_id)
        .with_context("diagnostic", diagnostic.message);
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (!suffix_start_index)
    return close_without_repair();
  for (std::size_t index = *suffix_start_index; index < records.size(); ++index)
  {
    if (records[index].entry.type != EntryType::AssistantOutputItem)
    {
      close_both_best_effort();
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "assistant-output recovery suffix contains an unrelated session entry"));
    }
  }

  off_t const suffix_start_offset = records[*suffix_start_index].start;
  auto const removed_byte_count = static_cast<std::size_t>(repair_status.st_size - suffix_start_offset);
  std::string suffix_bytes(removed_byte_count, '\0');
  std::size_t suffix_offset = 0;
  while (suffix_offset < suffix_bytes.size())
  {
    if (auto canceled = check_recovery_cancellation(cancel_requested); !canceled)
    {
      close_both_best_effort();
      return std::unexpected(std::move(canceled.error()));
    }
    ssize_t count = 0;
    do
    {
      count = ::pread(repair_fd.get(), suffix_bytes.data() + suffix_offset, suffix_bytes.size() - suffix_offset,
                      suffix_start_offset + static_cast<off_t>(suffix_offset));
    } while (count < 0 && errno == EINTR);
    if (count <= 0)
    {
      auto error = path_io_error("failed to read the exact assistant-output suffix for quarantine", path, count < 0 ? errno : EIO);
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
    suffix_offset += static_cast<std::size_t>(count);
  }
  if (auto unchanged = require_unchanged_repair_namespace(repair_fd.get(), parent_fd.get(), repair_status, parent_status, parent_path, session_name, path);
      !unchanged)
  {
    auto error = std::move(unchanged.error());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }

  auto quarantined = quarantine_session_suffix(parent_fd.get(), parent_path, session_name, "incomplete-assistant-output", suffix_bytes, cancel_requested,
                                               before_recovery_quarantine_publication_for_test_ ? &before_recovery_quarantine_publication_for_test_ : nullptr);
  if (!quarantined)
  {
    auto error = std::move(quarantined.error());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (after_recovery_quarantine_publication_for_test_)
  {
    try
    {
      after_recovery_quarantine_publication_for_test_();
    }
    catch (...)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "assistant-output suffix recovery post-publication test hook failed");
      error.with_context("quarantine_path", quarantined->string());
      close_both_best_effort();
      return std::unexpected(std::move(error));
    }
  }
  if (auto unchanged = require_unchanged_repair_namespace(repair_fd.get(), parent_fd.get(), repair_status, parent_status, parent_path, session_name, path);
      !unchanged)
  {
    auto error = std::move(unchanged.error());
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (ftruncate(repair_fd.get(), suffix_start_offset) != 0)
  {
    auto error = path_io_error("failed to truncate quarantined incomplete assistant-output suffix", path, errno);
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (int const sync_error = ava::core::fdatasync_with_restart(repair_fd.get()); sync_error != 0)
  {
    auto error = path_io_error("assistant-output suffix was truncated but the source sync failed", path, sync_error);
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  struct stat repaired_status{};
  if (fstat(repair_fd.get(), &repaired_status) != 0)
  {
    auto error = path_io_error("failed to inspect session after truncating incomplete assistant-output suffix", path, errno);
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  if (auto unchanged = require_unchanged_repair_namespace(repair_fd.get(), parent_fd.get(), repaired_status, parent_status, parent_path, session_name, path);
      !unchanged)
  {
    auto error = std::move(unchanged.error());
    error.with_context("quarantine_path", quarantined->string());
    close_both_best_effort();
    return std::unexpected(std::move(error));
  }
  int const repair_close_error = repair_fd.close_checked();
  int const parent_close_error = parent_fd.close_checked();
  if (repair_close_error != 0)
  {
    auto error = path_io_error("failed to close repaired session after assistant-output suffix recovery", path, repair_close_error);
    error.with_context("quarantine_path", quarantined->string());
    return std::unexpected(std::move(error));
  }
  if (parent_close_error != 0)
  {
    auto error = path_io_error("failed to close session directory after assistant-output suffix recovery", parent_path, parent_close_error);
    error.with_context("quarantine_path", quarantined->string());
    return std::unexpected(std::move(error));
  }
  return std::optional<AssistantOutputSuffixRecovery>(AssistantOutputSuffixRecovery{
      .quarantine_path = std::move(*quarantined), .removed_entry_count = records.size() - *suffix_start_index, .removed_byte_count = removed_byte_count});
}

}  // namespace ava::session
