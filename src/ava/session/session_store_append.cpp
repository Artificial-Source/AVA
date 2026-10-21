#include "sys.h"
#include "ava/session/assistant_output.h"
#include "ava/session/record.h"
#include "ava/session/session_store_internal.h"
#include "ava/session/validation.h"
#include "ava/core/fd_io.h"
#include "ava/core/path.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::session {
namespace detail {

std::string_view append_commit_state_text(AppendCommitState state)
{
  switch (state)
  {
    case AppendCommitState::NotStarted:
      return "not_started";
    case AppendCommitState::PartialOrUnknown:
      return "partial_or_unknown";
    case AppendCommitState::CommittedToLeasedInode:
      return "committed_to_leased_inode";
  }
  AI_NEVER_REACHED
}

bool has_append_commit_state(ava::core::Error const& error)
{
  return std::ranges::any_of(error.context(), [](ava::core::ErrorContext const& item) { return item.key == "append_commit_state"; });
}

ava::core::Error with_append_commit_state(ava::core::Error error, AppendCommitState state, std::filesystem::path const& path)
{
  error.with_context("append_commit_state", std::string(append_commit_state_text(state)));
  if (state == AppendCommitState::PartialOrUnknown)
  {
    error.with_context("recovery",
                       "retain the same active SessionLease, call SessionStore::recover_torn_tail, then call "
                       "SessionStore::recover_incomplete_assistant_output_suffix before any further append");
  }
  error.with_context("path", path.string());
  return error;
}

}  // namespace detail

namespace {

using ava::core::normalized_absolute_path;
using detail::append_authority_error;
using detail::append_mutex_for_path;
using detail::AppendCommitState;
using detail::has_append_commit_state;
using detail::parse_strict_session_record;
using detail::path_io_error;
using detail::same_directory_identity;
using detail::same_file_identity;
using detail::SessionTraceScope;
using detail::with_append_commit_state;

}  // namespace

ava::core::VoidResult SessionStore::append(SessionLease const& lease, SessionEntry const& entry)
{
  if (entry.type == EntryType::AssistantOutputItem || entry.type == EntryType::AssistantTurnCommit)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                  "raw SessionStore append cannot mutate v4 assistant-output records; use SessionAppendTarget or append_validated_copy");
    error.with_context("entry_type", to_string(entry.type));
    return std::unexpected(with_append_commit_state(std::move(error), AppendCommitState::NotStarted, session_path()));
  }
  if (is_ephemeral())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistent session append lease cannot be used with an ephemeral store");
    return std::unexpected(with_append_commit_state(std::move(error), AppendCommitState::NotStarted, session_path()));
  }

  auto const path = session_path();
  auto const canonical_path = normalized_absolute_path(path);
  // Keep the legacy ordinary-record escape hatch from splitting a staged v4
  // assistant transaction. Its authoritative snapshot, state transition, and
  // durable write share the same per-path serialization as append targets.
  if (!lease.active() || canonical_path != lease.canonical_path())
  {
    auto appended = append_impl(&lease, entry);
    if (!appended && !has_append_commit_state(appended.error()))
      return std::unexpected(with_append_commit_state(std::move(appended.error()), AppendCommitState::NotStarted, path));
    return appended;
  }

  std::lock_guard serialization_lock(append_mutex_for_path(canonical_path));
  std::vector<SessionEntry> history;
  auto loaded = visit_entries_leased(
      lease, legacy_unbounded_session_read_limits(),
      [&](SessionEntry const& historical_entry) -> ava::core::Result<bool> {
        history.push_back(historical_entry);
        return true;
      },
      nullptr, false);
  if (!loaded)
    return std::unexpected(with_append_commit_state(std::move(loaded.error()), AppendCommitState::NotStarted, path));
  auto current_state = AssistantOutputAppendState::from_validated_history(history);
  if (!current_state)
    return std::unexpected(with_append_commit_state(std::move(current_state.error()), AppendCommitState::NotStarted, path));
  try
  {
    if (auto preflight = current_state->apply_candidate(entry); !preflight)
      return std::unexpected(with_append_commit_state(std::move(preflight.error()), AppendCommitState::NotStarted, path));
  }
  catch (...)
  {
    return std::unexpected(with_append_commit_state(
        ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to preflight raw assistant-output append state"), AppendCommitState::NotStarted, path));
  }

  auto appended = append_impl(&lease, entry, true);
  if (!appended && !has_append_commit_state(appended.error()))
    return std::unexpected(with_append_commit_state(std::move(appended.error()), AppendCommitState::NotStarted, path));
  return appended;
}

ava::core::VoidResult SessionStore::append_ephemeral(SessionEntry const& entry)
{
  if (entry.type == EntryType::AssistantOutputItem || entry.type == EntryType::AssistantTurnCommit)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                  "raw SessionStore append cannot mutate v4 assistant-output records; use SessionAppendTarget");
    error.with_context("entry_type", to_string(entry.type));
    return std::unexpected(std::move(error));
  }
  if (!is_ephemeral())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistent session append requires an active exact SessionLease"));
  }

  std::lock_guard serialization_lock(ephemeral_state_->mutation_mutex);
  auto history = load();
  if (!history)
    return std::unexpected(std::move(history.error()));
  auto current_state = AssistantOutputAppendState::from_validated_history(*history);
  if (!current_state)
    return std::unexpected(std::move(current_state.error()));
  try
  {
    if (auto preflight = current_state->apply_candidate(entry); !preflight)
      return std::unexpected(std::move(preflight.error()));
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to preflight raw assistant-output append state"));
  }
  return append_impl(nullptr, entry, true);
}

ava::core::VoidResult SessionStore::append_validated_copy(SessionLease const& lease, std::vector<SessionEntry> const& entries)
{
  if (is_ephemeral())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "validated session copy requires a persistent destination"));
  if (!lease.active() || !lease.created_)
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "validated session copy requires the active creating SessionLease for a new destination"));

  auto const path = session_path();
  auto const canonical_path = normalized_absolute_path(path);
  if (canonical_path != lease.canonical_path())
  {
    auto error = append_authority_error("validated session copy lease does not exactly match its destination", canonical_path);
    error.with_context("lease_path", lease.canonical_path().string());
    return std::unexpected(std::move(error));
  }

  for (auto const& entry : entries)
  {
    auto serialized = serialize_session_entry_line(entry);
    if (!serialized)
      return std::unexpected(std::move(serialized.error()));
    auto strict = parse_strict_session_record(*serialized, path, false);
    if (!strict)
      return std::unexpected(std::move(strict.error()));
  }
  auto const replay = validate_session_replay(entries);
  if (!replay.ok())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "validated session copy history fails replay validation");
    error.with_context("errors", std::to_string(replay.error_count));
    if (!replay.issues.empty())
      error.with_context("first_issue", replay.issues.front().message).with_context("entry_id", replay.issues.front().entry_id);
    return std::unexpected(std::move(error));
  }
  auto const assistant_output = classify_assistant_output(entries);
  if (!assistant_output.diagnostics.empty())
  {
    auto const& diagnostic = assistant_output.diagnostics.front();
    auto error =
        ava::core::Error(ava::core::ErrorCategory::Session, "validated session copy history contains an assistant-output diagnostic and cannot be copied");
    error.with_context("diagnostic_kind", std::string(to_string(diagnostic.kind)))
        .with_context("diagnostic_entry_id", diagnostic.entry_id)
        .with_context("diagnostic", diagnostic.message);
    return std::unexpected(std::move(error));
  }

  auto& append_mutex = append_mutex_for_path(canonical_path);
  std::lock_guard append_lock(append_mutex);
  auto destination_entries = load(lease);
  if (!destination_entries)
    return std::unexpected(std::move(destination_entries.error()));
  if (!destination_entries->empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "validated session copy destination must be newly created and empty");
    error.with_context("path", path.string()).with_context("existing_entries", std::to_string(destination_entries->size()));
    return std::unexpected(std::move(error));
  }

  for (auto const& entry : entries)
  {
    auto appended = append_impl(&lease, entry, true);
    if (!appended)
      return appended;
  }
  return {};
}

ava::core::VoidResult SessionStore::append_impl(SessionLease const* lease, SessionEntry const& entry, bool append_serialization_held)
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
        trace.emplace(std::move(observation), std::move(context), ava::observability::TraceEventType::SessionAppendAttempt,
                      ava::observability::TraceEventType::SessionAppendResult, entry.type, is_ephemeral());
      }
    }
    catch (...)
    {
      if (observation)
        observation->account_external_failure();
    }
  }
  if (auto valid_session_id = validate_session_id(options_.session_id); !valid_session_id)
    return valid_session_id;
  if (entry.data_json.empty() || entry.data_json.front() != '{' || entry.data_json.back() != '}')
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session entry data must be a JSON object");
    error.with_context("entry_id", entry.id);
    return std::unexpected(std::move(error));
  }
  if (entry.data_json.find('\n') != std::string::npos || entry.data_json.find('\r') != std::string::npos)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session entry data must not contain raw newlines");
    error.with_context("entry_id", entry.id);
    return std::unexpected(std::move(error));
  }
  if (auto valid_parent_id = validate_parent_id(entry.parent_id, entry.id); !valid_parent_id)
    return valid_parent_id;

  auto line = serialize_session_entry_line(entry);
  if (!line)
    return std::unexpected(std::move(line.error()));
  auto strict_entry = parse_strict_session_record(*line, session_path(), false);
  if (!strict_entry)
  {
    auto error = std::move(strict_entry.error());
    error.with_context("operation", "append").with_context("entry_id", entry.id);
    return std::unexpected(std::move(error));
  }

  if (ephemeral_state_)
  {
    if (lease != nullptr)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "ephemeral session append must not receive a lease"));
    std::lock_guard lock(ephemeral_state_->entries_mutex);
    ephemeral_state_->entries.push_back(entry);
    if (trace)
      trace->succeed();
    return {};
  }

  auto const path = session_path();
  if (lease == nullptr || !lease->active())
    return std::unexpected(with_append_commit_state(append_authority_error("persistent session append requires an active session lease", path),
                                                    AppendCommitState::NotStarted, path));
  auto const canonical_path = normalized_absolute_path(path);
  if (canonical_path != lease->canonical_path())
  {
    auto error = append_authority_error("session lease does not exactly match the persistent append target", canonical_path);
    error.with_context("lease_path", lease->canonical_path().string());
    return std::unexpected(with_append_commit_state(std::move(error), AppendCommitState::NotStarted, path));
  }
  auto const parent_path = canonical_path.parent_path();
  auto const session_name = canonical_path.filename().string();
  if (session_name.empty())
    return std::unexpected(
        with_append_commit_state(path_io_error("session append target has no basename", canonical_path), AppendCommitState::NotStarted, path));

  auto& append_mutex = append_mutex_for_path(canonical_path);
  std::optional<std::unique_lock<std::mutex>> append_lock;
  if (!append_serialization_held)
    append_lock.emplace(append_mutex);

  int parent_fd = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (parent_fd < 0)
    return std::unexpected(
        with_append_commit_state(path_io_error("failed to anchor session append directory", parent_path, errno), AppendCommitState::NotStarted, path));
  int append_fd = -1;
  auto close_fd = [](int& fd) {
    if (fd < 0)
      return 0;
    int const result = ::close(fd);
    int const error = result == 0 ? 0 : errno;
    fd = -1;
    return error;
  };
  auto close_both = [&] {
    int const append_close_error = close_fd(append_fd);
    int const parent_close_error = close_fd(parent_fd);
    return std::pair(append_close_error, parent_close_error);
  };
  auto fail_before_write = [&](ava::core::Error error) -> ava::core::VoidResult {
    auto const [append_close_error, parent_close_error] = close_both();
    if (append_close_error != 0)
      error.with_context("append_close_cause", std::strerror(append_close_error));
    if (parent_close_error != 0)
      error.with_context("parent_close_cause", std::strerror(parent_close_error));
    return std::unexpected(with_append_commit_state(std::move(error), AppendCommitState::NotStarted, path));
  };
  auto fail_after_write = [&](ava::core::Error error) -> ava::core::VoidResult {
    auto const [append_close_error, parent_close_error] = close_both();
    if (append_close_error != 0)
      error.with_context("append_close_cause", std::strerror(append_close_error));
    if (parent_close_error != 0)
      error.with_context("parent_close_cause", std::strerror(parent_close_error));
    return std::unexpected(with_append_commit_state(std::move(error), AppendCommitState::CommittedToLeasedInode, path));
  };

  struct stat anchored_parent_status{};
  if (fstat(parent_fd, &anchored_parent_status) != 0 || !S_ISDIR(anchored_parent_status.st_mode))
  {
    int const error_number = errno;
    return fail_before_write(path_io_error("session append parent is not a directory", parent_path, error_number));
  }
  append_fd = ::openat(parent_fd, session_name.c_str(), O_RDWR | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (append_fd < 0)
  {
    auto const category = errno == ELOOP ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
    auto error = ava::core::Error(category, errno == ELOOP ? "session append target must not be a symlink" : "failed to open session append target");
    error.with_context("path", path.string()).with_context("cause", std::strerror(errno));
    return fail_before_write(std::move(error));
  }

  struct AppendIdentity
  {
    struct stat lease_status{};
    struct stat append_status{};
    struct stat named_status{};
  };
  auto validate_identity = [&](std::string_view timing) -> ava::core::Result<AppendIdentity> {
    AppendIdentity identity;
    if (fstat(lease->fd_, &identity.lease_status) != 0)
      return std::unexpected(path_io_error("failed to inspect session lease " + std::string(timing), path, errno));
    if (fstat(append_fd, &identity.append_status) != 0)
      return std::unexpected(path_io_error("failed to inspect session append descriptor " + std::string(timing), path, errno));
    if (fstatat(parent_fd, session_name.c_str(), &identity.named_status, AT_SYMLINK_NOFOLLOW) != 0)
      return std::unexpected(path_io_error("failed to inspect session append name " + std::string(timing), path, errno));
    if (!same_file_identity(identity.lease_status, identity.append_status) || !same_file_identity(identity.lease_status, identity.named_status) ||
        identity.lease_status.st_nlink != 1 || identity.append_status.st_nlink != 1 || identity.named_status.st_nlink != 1)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session append target was replaced " + std::string(timing));
      error.with_context("timing", std::string(timing));
      error.with_context("lease_links", std::to_string(identity.lease_status.st_nlink));
      error.with_context("append_links", std::to_string(identity.append_status.st_nlink));
      error.with_context("name_links", std::to_string(identity.named_status.st_nlink));
      return std::unexpected(std::move(error));
    }
    struct stat current_parent_status{};
    if (fstat(parent_fd, &current_parent_status) != 0)
      return std::unexpected(path_io_error("failed to inspect anchored session directory " + std::string(timing), parent_path, errno));
    if (!same_directory_identity(anchored_parent_status, current_parent_status))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "anchored session directory changed during append");
      error.with_context("timing", std::string(timing)).with_context("path", parent_path.string());
      return std::unexpected(std::move(error));
    }
    int publication_fd = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (publication_fd < 0)
      return std::unexpected(path_io_error("failed to re-open session directory publication " + std::string(timing), parent_path, errno));
    struct stat publication_status{};
    int publication_status_error = fstat(publication_fd, &publication_status) == 0 ? 0 : errno;
    int publication_close_error = ::close(publication_fd) == 0 ? 0 : errno;
    if (publication_status_error != 0)
      return std::unexpected(
          path_io_error("failed to inspect re-opened session directory publication " + std::string(timing), parent_path, publication_status_error));
    if (publication_close_error != 0)
      return std::unexpected(
          path_io_error("failed to close re-opened session directory publication " + std::string(timing), parent_path, publication_close_error));
    if (!same_directory_identity(anchored_parent_status, publication_status))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session directory publication changed during append");
      error.with_context("timing", std::string(timing)).with_context("path", parent_path.string());
      return std::unexpected(std::move(error));
    }
    return identity;
  };

  auto initial_identity = validate_identity("before session framing inspection");
  if (!initial_identity)
    return fail_before_write(std::move(initial_identity.error()));
  if (initial_identity->append_status.st_size > 0)
  {
    char last_byte = '\0';
    ssize_t count = 0;
    do
    {
      count = ::pread(append_fd, &last_byte, 1, initial_identity->append_status.st_size - 1);
    } while (count < 0 && errno == EINTR);
    if (count != 1)
      return fail_before_write(path_io_error("failed to read session framing before append", path, count < 0 ? errno : EIO));
    if (last_byte != '\n')
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "refusing to append to a session with an unterminated tail");
      error.with_context("recovery", "retain the active SessionLease and call SessionStore::recover_torn_tail before appending");
      return fail_before_write(std::move(error));
    }
  }

  auto before_mode = validate_identity("immediately before session mode update");
  if (!before_mode)
    return fail_before_write(std::move(before_mode.error()));
  if (fchmod(append_fd, 0600) != 0)
    return fail_before_write(path_io_error("failed to preserve session file permissions", path, errno));
  if (before_append_identity_check_for_test_)
  {
    try
    {
      before_append_identity_check_for_test_();
    }
    catch (...)
    {
      return fail_before_write(ava::core::Error(ava::core::ErrorCategory::Unknown, "session append pre-write test hook failed"));
    }
  }
  auto before_write = validate_identity("immediately before entry write");
  if (!before_write)
    return fail_before_write(std::move(before_write.error()));

  auto payload = std::move(*line);
  payload.push_back('\n');
  std::size_t written = 0;
  int write_error = 0;
  while (written < payload.size())
  {
    ssize_t count = 0;
    if (append_write_for_test_)
      count = append_write_for_test_(append_fd, std::string_view(payload).substr(written));
    else
      count = ::write(append_fd, payload.data() + written, payload.size() - written);
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      write_error = errno;
      break;
    }
    if (count == 0 || static_cast<std::size_t>(count) > payload.size() - written)
    {
      write_error = EIO;
      break;
    }
    written += static_cast<std::size_t>(count);
  }
  if (written != payload.size())
  {
    auto error = path_io_error("failed to write complete session entry through the append descriptor", path, write_error == 0 ? EIO : write_error);
    error.with_context("bytes_written", std::to_string(written)).with_context("payload_bytes", std::to_string(payload.size()));
    auto const [append_close_error, parent_close_error] = close_both();
    if (append_close_error != 0)
      error.with_context("append_close_cause", std::strerror(append_close_error));
    if (parent_close_error != 0)
      error.with_context("parent_close_cause", std::strerror(parent_close_error));
    return std::unexpected(
        with_append_commit_state(std::move(error), written == 0 ? AppendCommitState::NotStarted : AppendCommitState::PartialOrUnknown, path));
  }

  // A complete write is not a durable commit until its data reaches storage.
  // Batch appends share this barrier before publishing each record's epoch.
  if (int const sync_error = ava::core::fdatasync_with_restart(append_fd); sync_error != 0)
  {
    auto error = path_io_error("failed to sync the complete session entry through the append descriptor", path, sync_error);
    error.with_context("bytes_written", std::to_string(written)).with_context("payload_bytes", std::to_string(payload.size()));
    auto const [append_close_error, parent_close_error] = close_both();
    if (append_close_error != 0)
      error.with_context("append_close_cause", std::strerror(append_close_error));
    if (parent_close_error != 0)
      error.with_context("parent_close_cause", std::strerror(parent_close_error));
    return std::unexpected(with_append_commit_state(std::move(error), AppendCommitState::PartialOrUnknown, path));
  }

  if (after_append_write_for_test_)
  {
    try
    {
      after_append_write_for_test_();
    }
    catch (...)
    {
      return fail_after_write(ava::core::Error(ava::core::ErrorCategory::Unknown, "session append post-write test hook failed"));
    }
  }
  auto after_write = validate_identity("after the entry write");
  if (!after_write)
    return fail_after_write(std::move(after_write.error()));
  int const append_close_error = close_fd(append_fd);
  int const parent_close_error = close_fd(parent_fd);
  if (append_close_error != 0)
    return std::unexpected(with_append_commit_state(path_io_error("session entry was written but the append descriptor close failed", path, append_close_error),
                                                    AppendCommitState::CommittedToLeasedInode, path));
  if (parent_close_error != 0)
    return std::unexpected(
        with_append_commit_state(path_io_error("session entry was written but the session directory close failed", parent_path, parent_close_error),
                                 AppendCommitState::CommittedToLeasedInode, path));

  if (trace)
    trace->succeed();
  return {};
}

}  // namespace ava::session
