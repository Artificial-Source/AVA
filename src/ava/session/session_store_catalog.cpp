#include "sys.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/record.h"
#include "ava/session/session_store_internal.h"
#include "ava/core/ids.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ava::session {

using detail::open_regular_snapshot;
using detail::path_io_error;
using detail::project_key;
using detail::ScopedFd;
using detail::validate_read_limits;

ava::core::Result<SessionStore> SessionStore::create(std::filesystem::path const& workspace_dir, std::filesystem::path const& root_dir)
{
  return SessionStore(SessionStoreOptions{
      .root_dir = root_dir,
      .workspace_dir = workspace_dir,
      .session_id = ava::core::make_id("session"),
  });
}

ava::core::Result<SessionStore> SessionStore::create_ephemeral(std::filesystem::path const& workspace_dir)
{
  std::error_code temp_error;
  auto temp_root = std::filesystem::temp_directory_path(temp_error);
  if (temp_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to resolve temporary directory for ephemeral session");
    error.with_context("cause", temp_error.message());
    return std::unexpected(std::move(error));
  }

  auto scratch_root = temp_root / ("ava-" + ava::core::make_id("ephemeral-session"));
  return SessionStore(
      SessionStoreOptions{
          .root_dir = scratch_root,
          .workspace_dir = workspace_dir,
          .session_id = ava::core::make_id("session"),
      },
      std::make_shared<EphemeralState>(scratch_root));
}

ava::core::Result<SessionStore> SessionStore::open(std::filesystem::path const& workspace_dir, std::string session_id, std::filesystem::path const& root_dir)
{
  if (auto valid_session_id = validate_session_id(session_id); !valid_session_id)
  {
    return std::unexpected(std::move(valid_session_id.error()));
  }

  SessionStore store(SessionStoreOptions{
      .root_dir = root_dir,
      .workspace_dir = workspace_dir,
      .session_id = std::move(session_id),
  });
  auto opened = open_regular_snapshot(store.session_path(), std::numeric_limits<std::size_t>::max());
  if (!opened)
  {
    auto error = std::move(opened.error());
    error.with_context("session_id", store.options_r()->session_id);
    return std::unexpected(std::move(error));
  }
  ScopedFd snapshot_fd(opened->first);
  if (int const close_error = snapshot_fd.close_checked(); close_error != 0)
    return std::unexpected(path_io_error("failed to close session open snapshot", store.session_path(), close_error));
  return store;
}

ava::core::Result<std::vector<SessionSummary>> SessionStore::list_sessions(std::filesystem::path const& workspace_dir, std::filesystem::path const& root_dir)
{
  auto const directory = root_dir / project_key(workspace_dir);
  std::error_code exists_error;
  bool const directory_exists = std::filesystem::exists(directory, exists_error);
  if (exists_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect session directory");
    error.with_context("path", directory.string());
    error.with_context("cause", exists_error.message());
    return std::unexpected(std::move(error));
  }
  if (!directory_exists)
  {
    return std::vector<SessionSummary>{};
  }

  std::vector<SessionSummary> summaries;
  std::error_code iter_error;
  for (std::filesystem::directory_iterator iter(directory, iter_error), end; iter != end; iter.increment(iter_error))
  {
    if (iter_error)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to list sessions");
      error.with_context("path", directory.string());
      error.with_context("cause", iter_error.message());
      return std::unexpected(std::move(error));
    }
    auto const& entry = *iter;
    std::error_code entry_error;
    auto const entry_status = entry.symlink_status(entry_error);
    if (entry_error || std::filesystem::is_symlink(entry_status) || !std::filesystem::is_regular_file(entry_status) || entry.path().extension() != ".jsonl")
    {
      continue;
    }
    auto const session_id = entry.path().stem().string();
    auto store = SessionStore::open(workspace_dir, session_id, root_dir);
    if (!store)
    {
      continue;
    }
    auto summary = store->inspect_bounded_for_listing(SessionReadLimits{.max_file_bytes = std::numeric_limits<std::size_t>::max(),
                                                                        .max_line_bytes = kMaxSessionLineBytes,
                                                                        .max_entries = std::numeric_limits<std::size_t>::max()},
                                                      true);
    if (!summary)
    {
      if (is_unsupported_session_version_error(summary.error()))
      {
        auto error = std::move(summary.error());
        error.with_context("session_id", session_id);
        return std::unexpected(std::move(error));
      }
      continue;
    }
    summaries.push_back(std::move(*summary));
  }

  std::ranges::sort(summaries, [](SessionSummary const& left, SessionSummary const& right) {
    std::error_code left_error;
    std::error_code right_error;
    auto const left_time = std::filesystem::last_write_time(left.path, left_error);
    auto const right_time = std::filesystem::last_write_time(right.path, right_error);
    if (!left_error && !right_error && left_time != right_time)
    {
      return left_time > right_time;
    }
    return left.session_id > right.session_id;
  });

  return summaries;
}

ava::core::Result<std::vector<SessionSummary>> SessionStore::list_sessions_bounded(std::filesystem::path const& workspace_dir,
                                                                                   std::filesystem::path const& root_dir, SessionListLimits limits,
                                                                                   SessionCancelCallback cancel_requested)
{
  if (limits.max_sessions == 0 || limits.max_total_file_bytes == 0)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session list limits must be non-zero"));
  if (auto valid = validate_read_limits(limits.per_session); !valid)
    return std::unexpected(std::move(valid.error()));

  auto const directory = root_dir / project_key(workspace_dir);
  std::error_code exists_error;
  if (!std::filesystem::exists(directory, exists_error))
  {
    if (!exists_error)
      return std::vector<SessionSummary>{};
    return std::unexpected(path_io_error("failed to inspect session directory", directory, exists_error.value()));
  }

  std::vector<std::pair<std::string, std::filesystem::path>> candidates;
  std::size_t total_file_bytes = 0;
  std::error_code iter_error;
  for (std::filesystem::directory_iterator iter(directory, iter_error), end; iter != end; iter.increment(iter_error))
  {
    if (cancel_requested && cancel_requested())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session list canceled"));
    if (iter_error)
      return std::unexpected(path_io_error("failed to list sessions", directory, iter_error.value()));
    auto const& entry = *iter;
    std::error_code status_error;
    auto const status = entry.symlink_status(status_error);
    if (status_error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) || entry.path().extension() != ".jsonl")
      continue;
    auto id = entry.path().stem().string();
    if (!validate_session_id(id))
      continue;
    if (candidates.size() >= limits.max_sessions)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session count exceeds bounded list limit");
      error.with_context("max_sessions", std::to_string(limits.max_sessions));
      return std::unexpected(std::move(error));
    }
    std::error_code size_error;
    auto const size = entry.file_size(size_error);
    if (size_error || size > limits.per_session.max_file_bytes || size > limits.max_total_file_bytes - total_file_bytes)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session files exceed bounded list byte limit");
      error.with_context("max_total_file_bytes", std::to_string(limits.max_total_file_bytes));
      return std::unexpected(std::move(error));
    }
    total_file_bytes += static_cast<std::size_t>(size);
    candidates.emplace_back(std::move(id), entry.path());
  }
  if (iter_error)
    return std::unexpected(path_io_error("failed to list sessions", directory, iter_error.value()));

  std::vector<SessionSummary> summaries;
  summaries.reserve(candidates.size());
  for (auto const& [id, path] : candidates)
  {
    (void)path;
    SessionStore store(SessionStoreOptions{.root_dir = root_dir, .workspace_dir = workspace_dir, .session_id = id});
    auto summary = store.inspect_bounded_for_listing(limits.per_session, true, cancel_requested);
    if (!summary)
      return std::unexpected(std::move(summary.error()));
    summaries.push_back(std::move(*summary));
  }
  std::ranges::sort(summaries, [](SessionSummary const& left, SessionSummary const& right) {
    if (left.last_updated != right.last_updated)
      return left.last_updated > right.last_updated;
    return left.session_id > right.session_id;
  });
  return summaries;
}

std::filesystem::path SessionStore::default_root_dir()
{
  return ava::config::xdg_paths().sessions_dir;
}

}  // namespace ava::session
