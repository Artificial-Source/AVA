#include "sys.h"
#include "ava/debug/print_members_on.h"
#include "ava/session/record.h"
#include "ava/session/session_store_internal.h"
#include "ava/core/json.h"
#include "ava/core/path.h"
#include "ava/core/sha256.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::session {
namespace detail {

ava::core::Result<std::pair<int, off_t>> open_regular_snapshot(std::filesystem::path const& path, std::size_t max_file_bytes)
{
  // Inspect the final component with O_PATH first. In particular, never open a
  // pathname replaced by a FIFO through blocking O_RDONLY before classifying it.
  ava::core::UniqueFd metadata_fd(::open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
  if (metadata_fd.get() < 0)
  {
    auto category =
        errno == ENOENT ? ava::core::ErrorCategory::NotFound : (errno == ELOOP ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io);
    auto error = ava::core::Error(category, errno == ENOENT ? "session not found" : "failed to securely inspect session file");
    error.with_context("path", path.string()).with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(error));
  }
  struct stat metadata_status{};
  int metadata_error = fstat(metadata_fd.get(), &metadata_status) == 0 ? 0 : errno;
  int metadata_close_error = metadata_fd.close_checked();
  if (metadata_error != 0)
    return std::unexpected(path_io_error("failed to inspect session file metadata", path, metadata_error));
  if (metadata_close_error != 0)
    return std::unexpected(path_io_error("failed to close session file metadata descriptor", path, metadata_close_error));
  if (!S_ISREG(metadata_status.st_mode))
  {
    if (S_ISLNK(metadata_status.st_mode))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session path must not be a symlink");
      error.with_context("path", path.string());
      return std::unexpected(std::move(error));
    }
    return std::unexpected(path_io_error("session path is not a regular file", path));
  }
  if (metadata_status.st_size < 0)
    return std::unexpected(path_io_error("session snapshot has an invalid size", path));
  auto const size = static_cast<std::uintmax_t>(metadata_status.st_size);
  if (size > max_file_bytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session file exceeds bounded read limit");
    error.with_context("path", path.string()).with_context("max_file_bytes", std::to_string(max_file_bytes));
    return std::unexpected(std::move(error));
  }

  ava::core::UniqueFd fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0)
    return std::unexpected(path_io_error("failed to securely open session snapshot", path, errno));
  struct stat status{};
  int status_error = fstat(fd.get(), &status) == 0 ? 0 : errno;
  if (status_error != 0 || !S_ISREG(status.st_mode) || metadata_status.st_dev != status.st_dev || metadata_status.st_ino != status.st_ino)
  {
    if (status_error != 0)
      return std::unexpected(path_io_error("failed to inspect session snapshot", path, status_error));
    return std::unexpected(path_io_error("session snapshot target changed while opening", path));
  }
  return std::pair<int, off_t>(fd.release(), metadata_status.st_size);
}

}  // namespace detail

namespace {

using detail::open_regular_snapshot;
using detail::parse_strict_session_record;
using detail::path_io_error;
using detail::strict_session_record_error;

void wipe_bytes(void* data, std::size_t size) noexcept
{
  auto* cursor = static_cast<unsigned char volatile*>(data);
  while (size-- > 0) *cursor++ = 0;
}

class ByteVectorWiper final
{
 public:
  explicit ByteVectorWiper(std::vector<std::uint8_t>& bytes) noexcept : bytes_(bytes) { }
  ~ByteVectorWiper() noexcept { wipe_bytes(bytes_.data(), bytes_.size()); }
  ByteVectorWiper(ByteVectorWiper const&) = delete;
  ByteVectorWiper& operator=(ByteVectorWiper const&) = delete;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::vector<std::uint8_t>& bytes_;
};

void wipe_string(std::string& value) noexcept
{
  wipe_bytes(value.data(), value.size());
  value.clear();
}

void wipe_session_entry(SessionEntry& entry) noexcept
{
  wipe_string(entry.id);
  wipe_string(entry.parent_id);
  wipe_string(entry.timestamp);
  wipe_string(entry.data_json);
}

class StringWiper final
{
 public:
  explicit StringWiper(std::string& value) noexcept : value_(value) { }
  ~StringWiper() noexcept { wipe_string(value_); }
  StringWiper(StringWiper const&) = delete;
  StringWiper& operator=(StringWiper const&) = delete;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::string& value_;
};

template <std::size_t Size>
class CharArrayWiper final
{
 public:
  explicit CharArrayWiper(std::array<char, Size>& value) noexcept : value_(value) { }
  ~CharArrayWiper() noexcept { wipe_bytes(value_.data(), value_.size()); }
  CharArrayWiper(CharArrayWiper const&) = delete;
  CharArrayWiper& operator=(CharArrayWiper const&) = delete;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::array<char, Size>& value_;
};

class SessionEntryWiper final
{
 public:
  explicit SessionEntryWiper(SessionEntry& entry) noexcept : entry_(entry) { }
  ~SessionEntryWiper() noexcept { wipe_session_entry(entry_); }
  SessionEntryWiper(SessionEntryWiper const&) = delete;
  SessionEntryWiper& operator=(SessionEntryWiper const&) = delete;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  SessionEntry& entry_;
};

class SessionEntryVectorWiper final
{
 public:
  explicit SessionEntryVectorWiper(std::vector<SessionEntry>& entries) noexcept : entries_(entries) { }
  ~SessionEntryVectorWiper() noexcept
  {
    if (!active_)
      return;
    for (auto& entry : entries_) wipe_session_entry(entry);
    entries_.clear();
  }
  SessionEntryVectorWiper(SessionEntryVectorWiper const&) = delete;
  SessionEntryVectorWiper& operator=(SessionEntryVectorWiper const&) = delete;
  void dismiss() noexcept { active_ = false; }
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::vector<SessionEntry>& entries_;
  bool active_ = true;
};

bool exact_snapshot_metadata_matches(struct stat const& before, struct stat const& after) noexcept
{
  if (before.st_size != after.st_size)
    return false;
#if defined(__linux__)
  return before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec && before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#else
  return before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime;
#endif
}

enum class SnapshotDecoding
{
  Strict,
  Compatibility,
};

template <typename Visitor>
ava::core::VoidResult visit_session_snapshot_fd(int fd, off_t snapshot_size, std::filesystem::path const& path, SessionReadLimits const& limits,
                                                Visitor const& visitor, SessionCancelCallback const& cancel_requested, bool tolerate_invalid_final_suffix,
                                                std::function<void()> const* after_first_read_for_test = nullptr,
                                                std::vector<std::uint8_t>* exact_bytes = nullptr, SnapshotDecoding decoding = SnapshotDecoding::Strict)
{
  std::array<char, 8192> buffer{};
  CharArrayWiper wipe_buffer(buffer);
  std::string line;
  StringWiper wipe_line(line);
  line.reserve(std::min<std::size_t>(limits.max_line_bytes, buffer.size()));
  std::size_t entry_count = 0;
  auto consume_line = [&](bool final_unterminated) -> ava::core::Result<bool> {
    if (decoding == SnapshotDecoding::Compatibility && !line.empty() && line.back() == '\r')
      line.pop_back();
    // Compatibility load retains legacy surrogate/key handling; only framing
    // and descriptor ownership are shared with the strict authority reader.
    auto const strict_status =
        decoding == SnapshotDecoding::Strict ? ava::core::validate_strict_json(line, ava::core::json::kMaxNestingDepth) : ava::core::StrictJsonStatus::Valid;
    if (tolerate_invalid_final_suffix && final_unterminated && strict_status == ava::core::StrictJsonStatus::Invalid)
    {
      wipe_string(line);
      return false;
    }
    if (strict_status != ava::core::StrictJsonStatus::Valid)
      return std::unexpected(strict_session_record_error(strict_status, path, final_unterminated));
    if (++entry_count > limits.max_entries)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry count exceeds bounded read limit");
      error.with_context("max_entries", std::to_string(limits.max_entries));
      return std::unexpected(std::move(error));
    }
    auto entry = decoding == SnapshotDecoding::Strict ? parse_strict_session_record(line, path, final_unterminated) : parse_session_entry_line(line, path);
    if (!entry)
      return std::unexpected(std::move(entry.error()));
    SessionEntryWiper wipe_entry(*entry);
    auto keep_going = visitor(std::move(*entry));
    if (!keep_going)
      return std::unexpected(std::move(keep_going.error()));
    wipe_string(line);
    return *keep_going;
  };

  off_t offset = 0;
  bool ran_read_hook = false;
  while (offset < snapshot_size)
  {
    if (cancel_requested && cancel_requested())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session read canceled", ava::core::ErrorCode::Canceled));
    auto const remaining = snapshot_size - offset;
    auto const wanted = remaining < static_cast<off_t>(buffer.size()) ? static_cast<std::size_t>(remaining) : buffer.size();
    ssize_t count = 0;
    do
    {
      count = ::pread(fd, buffer.data(), wanted, offset);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
      return std::unexpected(path_io_error("failed while reading session file", path, errno));
    if (count == 0 || static_cast<std::size_t>(count) > wanted)
      return std::unexpected(path_io_error("session file shrank or became unreadable during bounded read", path));
    offset += count;
    if (exact_bytes != nullptr)
    {
      auto const* begin = reinterpret_cast<std::uint8_t const*>(buffer.data());
      exact_bytes->insert(exact_bytes->end(), begin, begin + count);
    }

    if (!ran_read_hook && after_first_read_for_test != nullptr && *after_first_read_for_test)
    {
      ran_read_hook = true;
      try
      {
        (*after_first_read_for_test)();
      }
      catch (...)
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "lease-bound session read test hook failed"));
      }
    }

    for (ssize_t index = 0; index < count; ++index)
    {
      char const ch = buffer[static_cast<std::size_t>(index)];
      if (ch == '\n')
      {
        auto keep_going = consume_line(false);
        if (!keep_going)
          return std::unexpected(std::move(keep_going.error()));
        if (!*keep_going)
          return {};
      }
      else
      {
        if (line.size() >= limits.max_line_bytes)
        {
          auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session line exceeds bounded read limit");
          error.with_context("path", path.string()).with_context("max_line_bytes", std::to_string(limits.max_line_bytes));
          return std::unexpected(std::move(error));
        }
        line.push_back(ch);
      }
    }
  }
  if (!line.empty())
  {
    auto keep_going = consume_line(true);
    if (!keep_going)
      return std::unexpected(std::move(keep_going.error()));
  }
  return {};
}

ava::core::VoidResult visit_regular_session_snapshot(std::filesystem::path const& path, SessionReadLimits const& limits, SessionEntryVisitor const& visitor,
                                                     SessionCancelCallback const& cancel_requested, bool tolerate_invalid_final_suffix)
{
  auto opened = open_regular_snapshot(path, limits.max_file_bytes);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  ava::core::UniqueFd fd(opened->first);
  auto visited = visit_session_snapshot_fd(fd.get(), opened->second, path, limits, visitor, cancel_requested, tolerate_invalid_final_suffix);
  int const close_error = fd.close_checked();
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  if (close_error != 0)
    return std::unexpected(path_io_error("failed to close session snapshot", path, close_error));
  return {};
}

ava::core::Result<bool> accumulate_session_summary(SessionSummary& summary, bool& has_manual_title, SessionEntry const& entry, bool inspect_metadata)
{
  ++summary.entry_count;
  summary.last_updated = entry.timestamp;
  if (!inspect_metadata)
    return true;
  if (entry.type == EntryType::SessionMetadata)
  {
    if (auto name = ava::core::json::string_field(entry.data_json, "name"))
    {
      summary.title = std::move(*name);
      has_manual_title = true;
    }
    else if (!has_manual_title)
    {
      if (auto generated = ava::core::json::string_field(entry.data_json, "generated_title"))
        summary.title = std::move(*generated);
    }
  }
  if (entry.type == EntryType::SessionStart || entry.type == EntryType::SessionMetadata)
  {
    if (auto cwd = ava::core::json::string_field(entry.data_json, "original_cwd"); cwd && !cwd->empty())
    {
      std::filesystem::path candidate(*cwd);
      if (!candidate.is_absolute() || candidate.lexically_normal() != candidate)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persisted session cwd is invalid"));
      if (!summary.original_cwd.empty() && summary.original_cwd != candidate)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persisted session cwd changed"));
      summary.original_cwd = std::move(candidate);
    }
  }
  return true;
}

}  // namespace

using ava::core::normalized_absolute_path;
using detail::append_authority_error;
using detail::append_mutex_for_path;
using detail::path_io_error;
using detail::same_directory_identity;
using detail::same_file_identity;
using detail::SessionTraceScope;
using detail::validate_read_limits;

ava::core::Result<std::vector<SessionEntry>> SessionStore::load() const
{
  std::optional<SessionTraceScope> trace;
  if (observation_attachment_->enabled.load(std::memory_order_acquire))
  {
    std::shared_ptr<ava::observability::RunObservation> observation;
    try
    {
      std::shared_ptr<ava::observability::TraceContext const> context;
      {
        std::lock_guard lock(observation_attachment_->mutex);
        observation = observation_attachment_->observation;
        context = observation_attachment_->context;
      }
      if (observation && observation->enabled() && context)
      {
        trace.emplace(std::move(observation), std::move(context), ava::observability::TraceEventType::SessionLoadAttempt,
                      ava::observability::TraceEventType::SessionLoadResult, std::nullopt, is_ephemeral());
      }
    }
    catch (...)
    {
      if (observation)
        observation->account_external_failure();
    }
  }
  if (auto valid_session_id = validate_session_id(options_r()->session_id); !valid_session_id)
  {
    return std::unexpected(std::move(valid_session_id.error()));
  }

  if (ephemeral_state_)
  {
    std::lock_guard lock(ephemeral_state_->entries_mutex);
    auto entries = ephemeral_state_->entries;
    if (trace)
      trace->succeed(entries.size());
    return entries;
  }

  auto const path = session_path();
  auto& append_mutex = append_mutex_for_path(path);
  std::lock_guard append_lock(append_mutex);

  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(path, status_error);
  if (status_error && status_error != std::errc::no_such_file_or_directory)
    return std::unexpected(path_io_error("failed to inspect session file", path, status_error.value()));
  if (!std::filesystem::exists(status))
  {
    if (trace)
      trace->succeed();
    return std::vector<SessionEntry>{};
  }
  if (std::filesystem::is_symlink(status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session path must not be a symlink");
    error.with_context("session_id", options_r()->session_id);
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  if (!std::filesystem::is_regular_file(status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session path is not a regular file");
    error.with_context("session_id", options_r()->session_id);
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }

  auto file = open_regular_snapshot(path, std::numeric_limits<std::size_t>::max());
  if (!file)
  {
    auto error = std::move(file.error());
    error.with_context("session_id", options_r()->session_id);
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }

  ava::core::UniqueFd snapshot_fd(file->first);
  std::vector<SessionEntry> entries;
  auto visited = visit_session_snapshot_fd(
      snapshot_fd.get(), file->second, path,
      SessionReadLimits{.max_file_bytes = std::numeric_limits<std::size_t>::max(),
                        .max_line_bytes = kMaxSessionLineBytes - 1,
                        .max_entries = std::numeric_limits<std::size_t>::max()},
      [&entries](SessionEntry&& entry) -> ava::core::Result<bool> {
        entries.push_back(std::move(entry));
        return true;
      },
      nullptr, false, nullptr, nullptr, SnapshotDecoding::Compatibility);
  int const close_error = snapshot_fd.close_checked();
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  if (close_error != 0)
    return std::unexpected(path_io_error("failed to close compatibility session snapshot", path, close_error));
  if (trace)
    trace->succeed(entries.size());
  return entries;
}

ava::core::VoidResult SessionStore::visit_entries(SessionReadLimits limits, SessionEntryVisitor const& visitor, SessionCancelCallback cancel_requested) const
{
  if (auto valid = validate_read_limits(limits); !valid)
    return valid;
  if (!visitor)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session entry visitor is required"));
  if (auto valid_session_id = validate_session_id(options_r()->session_id); !valid_session_id)
    return valid_session_id;

  if (ephemeral_state_)
  {
    std::vector<SessionEntry> snapshot;
    {
      std::lock_guard lock(ephemeral_state_->entries_mutex);
      if (ephemeral_state_->entries.size() > limits.max_entries)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "session entry count exceeds bounded read limit"));
      // Apply the same JSONL byte policy before copying an in-memory snapshot;
      // an ephemeral authority must not silently widen file or line limits.
      std::size_t total_bytes = 0;
      for (auto const& entry : ephemeral_state_->entries)
      {
        auto serialized = serialize_session_entry_line(entry);
        if (!serialized)
          return std::unexpected(std::move(serialized.error()));
        StringWiper wipe_serialized(*serialized);
        if (serialized->size() > limits.max_line_bytes)
        {
          auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session line exceeds bounded read limit");
          error.with_context("max_line_bytes", std::to_string(limits.max_line_bytes));
          return std::unexpected(std::move(error));
        }
        if (serialized->size() + 1 > limits.max_file_bytes - total_bytes)
        {
          auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session file exceeds bounded read limit");
          error.with_context("max_file_bytes", std::to_string(limits.max_file_bytes));
          return std::unexpected(std::move(error));
        }
        total_bytes += serialized->size() + 1;
      }
      snapshot = ephemeral_state_->entries;
    }
    for (auto const& entry : snapshot)
    {
      if (cancel_requested && cancel_requested())
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session read canceled", ava::core::ErrorCode::Canceled));
      auto keep_going = visitor(entry);
      if (!keep_going)
        return std::unexpected(std::move(keep_going.error()));
      if (!*keep_going)
        break;
    }
    return {};
  }

  // Reads are pinned to one descriptor/fstat byte snapshot. No registry or
  // append mutex is held across open/read/parse; a concurrently incomplete
  // trailing record fails this transient read rather than being accepted.
  return visit_regular_session_snapshot(session_path(), limits, visitor, cancel_requested, false);
}

ava::core::VoidResult SessionStore::visit_entries_leased(SessionLease const& lease, SessionReadLimits limits, SessionEntryVisitor const& visitor,
                                                         SessionCancelCallback cancel_requested, bool invoke_after_lease_bound_read_test_hook,
                                                         bool tolerate_invalid_final_suffix, SessionByteFingerprint* exact_fingerprint) const
{
  if (auto valid = validate_read_limits(limits); !valid)
    return valid;
  if (!visitor)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session entry visitor is required"));
  if (auto valid_session_id = validate_session_id(options_r()->session_id); !valid_session_id)
    return valid_session_id;
  if (ephemeral_state_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lease-bound reads require a persistent session store"));
  if (!lease.active())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lease-bound session read requires an active session lease"));

  auto const path = normalized_absolute_path(session_path());
  if (path != lease.canonical_path())
  {
    auto error = append_authority_error("session lease does not exactly match the persistent read target", path);
    error.with_context("lease_path", lease.canonical_path().string());
    return std::unexpected(std::move(error));
  }
  auto const parent_path = path.parent_path();
  auto const session_name = path.filename().string();
  if (session_name.empty())
    return std::unexpected(path_io_error("lease-bound session read target has no basename", path));

  ava::core::UniqueFd parent_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (parent_fd.get() < 0)
    return std::unexpected(path_io_error("failed to anchor lease-bound session read directory", parent_path, errno));

  struct stat initial_parent_status{};
  if (fstat(parent_fd.get(), &initial_parent_status) != 0 || !S_ISDIR(initial_parent_status.st_mode))
  {
    int const error_number = errno;
    return std::unexpected(path_io_error("lease-bound session read parent is not a directory", parent_path, error_number));
  }

  struct ReadIdentity
  {
    struct stat lease_status{};
    struct stat named_status{};
  };
  auto validate_identity = [&](std::string_view timing) -> ava::core::Result<ReadIdentity> {
    ReadIdentity identity;
    if (fstat(lease.fd_, &identity.lease_status) != 0)
      return std::unexpected(path_io_error("failed to inspect session lease " + std::string(timing), path, errno));
    if (fstatat(parent_fd.get(), session_name.c_str(), &identity.named_status, AT_SYMLINK_NOFOLLOW) != 0)
      return std::unexpected(path_io_error("failed to inspect leased session name " + std::string(timing), path, errno));
    if (!same_file_identity(identity.lease_status, identity.named_status) || identity.lease_status.st_nlink != 1 || identity.named_status.st_nlink != 1)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "lease-bound session read target was replaced " + std::string(timing));
      error.with_context("timing", std::string(timing));
      error.with_context("lease_links", std::to_string(identity.lease_status.st_nlink));
      error.with_context("name_links", std::to_string(identity.named_status.st_nlink));
      return std::unexpected(std::move(error));
    }

    struct stat anchored_parent_status{};
    if (fstat(parent_fd.get(), &anchored_parent_status) != 0)
      return std::unexpected(path_io_error("failed to inspect anchored session directory " + std::string(timing), parent_path, errno));
    if (!same_directory_identity(initial_parent_status, anchored_parent_status))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "anchored session directory changed during lease-bound read");
      error.with_context("timing", std::string(timing)).with_context("path", parent_path.string());
      return std::unexpected(std::move(error));
    }

    ava::core::UniqueFd publication_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (publication_fd.get() < 0)
      return std::unexpected(path_io_error("failed to re-open session directory publication " + std::string(timing), parent_path, errno));
    struct stat publication_status{};
    int const publication_error = fstat(publication_fd.get(), &publication_status) == 0 ? 0 : errno;
    int const publication_close_error = publication_fd.close_checked();
    if (publication_error != 0)
      return std::unexpected(path_io_error("failed to inspect session directory publication " + std::string(timing), parent_path, publication_error));
    if (publication_close_error != 0)
      return std::unexpected(path_io_error("failed to close session directory publication " + std::string(timing), parent_path, publication_close_error));
    if (!same_directory_identity(initial_parent_status, publication_status))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session directory publication changed during lease-bound read");
      error.with_context("timing", std::string(timing)).with_context("path", parent_path.string());
      return std::unexpected(std::move(error));
    }
    return identity;
  };

  auto initial = validate_identity("before snapshot");
  if (!initial)
    return std::unexpected(std::move(initial.error()));
  if (initial->lease_status.st_size < 0)
    return std::unexpected(path_io_error("lease-bound session snapshot has an invalid size", path));
  auto const initial_size = initial->lease_status.st_size;
  if (static_cast<std::uintmax_t>(initial_size) > limits.max_file_bytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session file exceeds bounded read limit");
    error.with_context("path", path.string()).with_context("max_file_bytes", std::to_string(limits.max_file_bytes));
    return std::unexpected(std::move(error));
  }

  std::vector<std::uint8_t> exact_bytes;
  ByteVectorWiper wipe_exact_bytes(exact_bytes);
  if (exact_fingerprint != nullptr)
    exact_bytes.reserve(static_cast<std::size_t>(initial_size));
  auto visited =
      visit_session_snapshot_fd(lease.fd_, initial_size, path, limits, visitor, cancel_requested, tolerate_invalid_final_suffix,
                                invoke_after_lease_bound_read_test_hook && after_lease_bound_read_for_test_ ? &after_lease_bound_read_for_test_ : nullptr,
                                exact_fingerprint != nullptr ? &exact_bytes : nullptr);
  auto final = validate_identity("after snapshot");
  bool const exact_changed = exact_fingerprint != nullptr && final &&
                             (!exact_snapshot_metadata_matches(initial->lease_status, final->lease_status) ||
                              !exact_snapshot_metadata_matches(initial->named_status, final->named_status));
  if (final && (!same_file_identity(initial->lease_status, final->lease_status) || final->lease_status.st_size < initial_size ||
                final->named_status.st_size < initial_size || exact_changed))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, exact_fingerprint != nullptr ? "leased session content changed during exact snapshot"
                                                                                                  : "leased session inode shrank or changed during snapshot");
    error.with_context("path", path.string());
    final = std::unexpected(std::move(error));
  }
  int const parent_close_error = parent_fd.close_checked();
  if (!final)
    return std::unexpected(std::move(final.error()));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  if (parent_close_error != 0)
    return std::unexpected(path_io_error("failed to close lease-bound session read directory", parent_path, parent_close_error));
  if (exact_fingerprint != nullptr)
  {
    if (exact_bytes.size() != static_cast<std::size_t>(initial_size))
      return std::unexpected(path_io_error("exact leased session snapshot byte count changed", path));
    *exact_fingerprint = SessionByteFingerprint{.byte_count = static_cast<std::uint64_t>(exact_bytes.size()), .sha256 = ava::core::sha256(exact_bytes)};
  }
  return {};
}

ava::core::Result<std::vector<SessionEntry>> SessionStore::load(SessionLease const& lease) const
{
  return load_bounded(lease, legacy_unbounded_session_read_limits());
}

ava::core::Result<std::vector<SessionEntry>> SessionStore::load_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested) const
{
  std::vector<SessionEntry> entries;
  SessionEntryVectorWiper wipe_entries(entries);
  entries.reserve(std::min<std::size_t>(limits.max_entries, 256));
  auto visited = visit_entries(
      limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        entries.push_back(entry);
        return true;
      },
      std::move(cancel_requested));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  wipe_entries.dismiss();
  return entries;
}

ava::core::Result<std::vector<SessionEntry>> SessionStore::load_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                        SessionCancelCallback cancel_requested) const
{
  std::vector<SessionEntry> entries;
  SessionEntryVectorWiper wipe_entries(entries);
  entries.reserve(std::min<std::size_t>(limits.max_entries, 256));
  auto visited = visit_entries_leased(
      lease, limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        entries.push_back(entry);
        return true;
      },
      std::move(cancel_requested));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  wipe_entries.dismiss();
  return entries;
}

ava::core::Result<std::vector<SessionEntry>> SessionStore::load_recoverable_prefix_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                                           SessionCancelCallback cancel_requested) const
{
  std::vector<SessionEntry> entries;
  SessionEntryVectorWiper wipe_entries(entries);
  entries.reserve(std::min<std::size_t>(limits.max_entries, 256));
  auto visited = visit_entries_leased(
      lease, limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        entries.push_back(entry);
        return true;
      },
      std::move(cancel_requested), true, true);
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  wipe_entries.dismiss();
  return entries;
}

ava::core::Result<SessionRecoverableSnapshot> SessionStore::load_recoverable_snapshot_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                                              SessionCancelCallback cancel_requested) const
{
  SessionRecoverableSnapshot snapshot;
  SessionEntryVectorWiper wipe_entries(snapshot.entries);
  snapshot.entries.reserve(std::min<std::size_t>(limits.max_entries, 256));
  auto visited = visit_entries_leased(
      lease, limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        snapshot.entries.push_back(entry);
        return true;
      },
      std::move(cancel_requested), true, true, &snapshot.fingerprint);
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  wipe_entries.dismiss();
  return snapshot;
}

ava::core::Result<SessionSummary> SessionStore::inspect_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested) const
{
  SessionSummary summary{.session_id = options_session_id_copy(), .path = session_path(), .last_updated = {}, .entry_count = 0, .original_cwd = {}, .title = {}};
  bool has_manual_title = false;
  auto visited = visit_entries(
      limits, [&](SessionEntry const& entry) -> ava::core::Result<bool> { return accumulate_session_summary(summary, has_manual_title, entry, true); },
      std::move(cancel_requested));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  return summary;
}

ava::core::Result<SessionSummary> SessionStore::inspect_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                SessionCancelCallback cancel_requested) const
{
  SessionSummary summary{.session_id = options_session_id_copy(), .path = session_path(), .last_updated = {}, .entry_count = 0, .original_cwd = {}, .title = {}};
  bool has_manual_title = false;
  auto visited = visit_entries_leased(
      lease, limits, [&](SessionEntry const& entry) -> ava::core::Result<bool> { return accumulate_session_summary(summary, has_manual_title, entry, true); },
      std::move(cancel_requested));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  return summary;
}

ava::core::Result<SessionSummary> SessionStore::inspect_bounded_for_listing(SessionReadLimits limits, bool inspect_metadata,
                                                                            SessionCancelCallback cancel_requested) const
{
  if (auto valid = validate_read_limits(limits); !valid)
    return std::unexpected(std::move(valid.error()));
  SessionSummary summary{.session_id = options_session_id_copy(), .path = session_path(), .last_updated = {}, .entry_count = 0, .original_cwd = {}, .title = {}};
  bool has_manual_title = false;
  auto visited = visit_regular_session_snapshot(
      session_path(), limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> { return accumulate_session_summary(summary, has_manual_title, entry, inspect_metadata); },
      cancel_requested, true);
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  return summary;
}

SessionReadLimits legacy_unbounded_session_read_limits()
{
  return SessionReadLimits{.max_file_bytes = std::numeric_limits<std::size_t>::max(),
                           .max_line_bytes = kMaxSessionLineBytes,
                           .max_entries = std::numeric_limits<std::size_t>::max()};
}

}  // namespace ava::session
