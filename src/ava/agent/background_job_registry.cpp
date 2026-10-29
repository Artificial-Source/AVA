#include "sys.h"
#include "ava/core/thread.h"
#include "ava/agent/background_job_registry.h"
#include "ava/session/session_store.h"
#include "ava/core/ids.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

namespace ava::agent {
namespace {

[[nodiscard]] bool is_terminal(BackgroundJobState state) noexcept
{
  return state == BackgroundJobState::Completed || state == BackgroundJobState::Failed || state == BackgroundJobState::Canceled;
}

[[nodiscard]] ava::core::Error job_not_found_error(std::string_view job_id)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "background job not found");
  error.with_context("job_id", std::string(job_id));
  return error;
}

[[nodiscard]] std::optional<std::string> formatted_error(std::optional<ava::core::Error> const& error)
{
  if (!error)
    return std::nullopt;
  return error->format();
}

[[nodiscard]] bool is_utf8_continuation_byte(char value) noexcept
{
  auto const byte = static_cast<unsigned char>(value);
  return (byte & 0xC0U) == 0x80U;
}

[[nodiscard]] bool truncate_to_bytes(std::string& value, std::size_t max_bytes)
{
  if (value.size() <= max_bytes)
    return false;
  auto truncated_size = max_bytes;
  while (truncated_size > 0 && is_utf8_continuation_byte(value[truncated_size])) --truncated_size;
  value.resize(truncated_size);
  return true;
}

[[nodiscard]] BackgroundJobCompletion worker_exception_completion(std::string const& job_id, std::stop_token const& stop_token, std::string cause)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "background job worker threw an exception");
  error.with_context("job_id", job_id);
  error.with_context("cause", std::move(cause));
  return BackgroundJobCompletion{.state = stop_token.stop_requested() ? BackgroundJobState::Canceled : BackgroundJobState::Failed,
                                 .final_text = "",
                                 .stop_reason = stop_token.stop_requested() ? "canceled" : "failed",
                                 .error = std::move(error)};
}

}  // namespace

struct BackgroundJobRegistry::JobRecord
{
  BackgroundJobSnapshot snapshot;
  // Published under the registry mutex, independently of the thread handle.
  // Stop requests invoke arbitrary callbacks inline and must run unlocked.
  std::stop_source stop_source{std::nostopstate};
  ava::core::JoinThread thread;
  std::mutex thread_mutex;
  bool joined = false;
  bool joining = false;
};

BackgroundJobRegistry::BackgroundJobRegistry(BackgroundJobRegistryOptions options) : options_(options)
{
}

namespace {

[[nodiscard]] ava::core::Error background_job_limit_error(std::size_t max_running_jobs)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "background job limit reached");
  error.with_context("max_running_jobs", std::to_string(max_running_jobs));
  return error;
}

[[nodiscard]] ava::core::Error duplicate_job_id_error(std::string const& job_id)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to allocate a unique background job id");
  error.with_context("job_id", job_id);
  return error;
}

[[nodiscard]] ava::core::Error missing_failure_error(std::string const& job_id)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "background job failed without an error");
  error.with_context("job_id", job_id);
  return error;
}

[[nodiscard]] BackgroundJobCompletion normalize_completion(std::string const& job_id, BackgroundJobCompletion completion)
{
  if (!is_terminal(completion.state))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "background job completed with a non-terminal state");
    error.with_context("job_id", job_id);
    error.with_context("state", to_string(completion.state));
    return BackgroundJobCompletion{.state = BackgroundJobState::Failed, .final_text = "", .stop_reason = "failed", .error = std::move(error)};
  }
  if (completion.state == BackgroundJobState::Completed)
  {
    completion.error = std::nullopt;
    if (completion.stop_reason.empty())
      completion.stop_reason = "completed";
    return completion;
  }
  if (completion.state == BackgroundJobState::Failed)
  {
    completion.final_text.clear();
    if (completion.stop_reason.empty())
      completion.stop_reason = "failed";
    if (!completion.error)
      completion.error = missing_failure_error(job_id);
    return completion;
  }
  completion.final_text.clear();
  completion.error = std::nullopt;
  if (completion.stop_reason.empty())
    completion.stop_reason = "canceled";
  return completion;
}

}  // namespace

std::string to_string(BackgroundJobState state)
{
  switch (state)
  {
    case BackgroundJobState::Running:
      return "running";
    case BackgroundJobState::Completed:
      return "completed";
    case BackgroundJobState::Failed:
      return "failed";
    case BackgroundJobState::Canceled:
      return "canceled";
  }
  return "unknown";
}

BackgroundJobRegistry::~BackgroundJobRegistry()
{
  shutdown();
}

ava::core::Result<BackgroundJobSnapshot> BackgroundJobRegistry::start(BackgroundJobStartOptions options, BackgroundJobWorker worker)
{
  if (!worker)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "background job worker is unavailable"));
  }
  static_cast<void>(join_finished());

  bool const caller_supplied_job_id = !options.job_id.empty();
  auto record = std::make_shared<JobRecord>();
  record->snapshot = BackgroundJobSnapshot{.job_id = std::move(options.job_id),
                                           .title = std::move(options.title),
                                           .description = std::move(options.description),
                                           .subagent_type = std::move(options.subagent_type),
                                           .child_session_id = std::move(options.child_session_id),
                                           .child_session_path = std::move(options.child_session_path),
                                           .state = BackgroundJobState::Running,
                                           .created_at = ava::session::now_timestamp()};
  record->snapshot.description_truncated = truncate_to_bytes(record->snapshot.description, options_.max_description_bytes);

  BackgroundJobSnapshot snapshot;
  std::string job_id;
  // Preflight is external code and may block or reenter the registry. Run it
  // before publishing a record, then recheck admission under mutex_ below.
  try
  {
    if (options_.thread_start_preflight)
    {
      if (auto allowed = options_.thread_start_preflight(); !allowed)
        return std::unexpected(std::move(allowed.error()));
    }
  }
  catch (std::exception const& error)
  {
    auto result_error = ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to prepare background job thread");
    result_error.with_context("cause", error.what());
    return std::unexpected(std::move(result_error));
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to prepare background job thread"));
  }

  // Shutdown may find the record immediately after publication, but cannot
  // finish joining it before this start has installed the worker or failed.
  std::unique_lock thread_lock(record->thread_mutex);
  {
    std::lock_guard lock(mutex_);
    if (!accepting_)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "background job registry is shutting down"));
    if (running_job_count_locked() >= options_.max_running_jobs)
    {
      return std::unexpected(background_job_limit_error(options_.max_running_jobs));
    }
    for (std::size_t attempt = 0; attempt < 8; ++attempt)
    {
      if (record->snapshot.job_id.empty())
        record->snapshot.job_id = ava::core::make_id("job");
      auto [_, inserted] = jobs_.emplace(record->snapshot.job_id, record);
      if (inserted)
      {
        job_id = record->snapshot.job_id;
        snapshot = record->snapshot;
        break;
      }
      // A caller-supplied durable ID must never be silently replaced.
      if (caller_supplied_job_id)
        break;
      record->snapshot.job_id.clear();
    }
    if (job_id.empty())
    {
      return std::unexpected(duplicate_job_id_error(record->snapshot.job_id));
    }
  }

  try
  {
    record->thread = ava::core::JoinThread::create("background_job", [this, job_id, worker = std::move(worker)](std::stop_token stop_token) mutable {
      BackgroundJobCompletion completion;
      try
      {
        completion = worker(BackgroundJobContext{.job_id = job_id, .stop_token = stop_token});
      }
      catch (std::exception const& error)
      {
        completion = worker_exception_completion(job_id, stop_token, error.what());
      }
      catch (...)
      {
        completion = worker_exception_completion(job_id, stop_token, "unknown exception");
      }
      complete(job_id, std::move(completion));
    });
  }
  catch (std::system_error const& error)
  {
    {
      std::lock_guard lock(mutex_);
      jobs_.erase(job_id);
    }
    changed_.notify_all();
    auto result_error = ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to start background job thread");
    result_error.with_context("job_id", job_id);
    result_error.with_context("cause", error.what());
    return std::unexpected(std::move(result_error));
  }
  catch (std::exception const& error)
  {
    {
      std::lock_guard lock(mutex_);
      jobs_.erase(job_id);
    }
    changed_.notify_all();
    auto result_error = ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to start background job");
    result_error.with_context("job_id", job_id);
    result_error.with_context("cause", error.what());
    return std::unexpected(std::move(result_error));
  }

  bool cancel_requested = false;
  auto stop_source = record->thread.get_stop_source();
  {
    std::lock_guard lock(mutex_);
    record->stop_source = stop_source;
    cancel_requested = record->snapshot.cancel_requested;
  }
  thread_lock.unlock();
  if (cancel_requested)
    stop_source.request_stop();
  changed_.notify_all();
  if (options_.wait_for_terminal_before_start_returns)
  {
    std::unique_lock lock(mutex_);
    static_cast<void>(changed_.wait_for(lock, std::chrono::seconds(2), [&] { return is_terminal(record->snapshot.state); }));
  }
  return snapshot;
}

std::vector<BackgroundJobSnapshot> BackgroundJobRegistry::snapshot() const
{
  std::vector<BackgroundJobSnapshot> snapshots;
  std::lock_guard lock(mutex_);
  snapshots.reserve(jobs_.size());
  for (auto const& [_, record] : jobs_)
  {
    snapshots.push_back(record->snapshot);
  }
  std::ranges::sort(snapshots, [](BackgroundJobSnapshot const& left, BackgroundJobSnapshot const& right) {
    if (left.created_at != right.created_at)
      return left.created_at < right.created_at;
    return left.job_id < right.job_id;
  });
  return snapshots;
}

ava::core::Result<BackgroundJobSnapshot> BackgroundJobRegistry::snapshot(std::string_view job_id) const
{
  std::lock_guard lock(mutex_);
  auto record = find_record_locked(job_id);
  if (!record)
    return std::unexpected(job_not_found_error(job_id));
  return record->snapshot;
}

ava::core::Result<BackgroundJobSnapshot> BackgroundJobRegistry::cancel(std::string_view job_id)
{
  std::stop_source stop_source{std::nostopstate};
  BackgroundJobSnapshot snapshot;
  {
    std::lock_guard lock(mutex_);
    auto record = find_record_locked(job_id);
    if (!record)
      return std::unexpected(job_not_found_error(job_id));
    if (record->snapshot.state == BackgroundJobState::Running)
      record->snapshot.cancel_requested = true;
    snapshot = record->snapshot;
    stop_source = record->stop_source;
  }

  stop_source.request_stop();
  changed_.notify_all();
  return snapshot;
}

ava::core::Result<BackgroundJobSnapshot> BackgroundJobRegistry::wait(std::string_view job_id, std::chrono::milliseconds timeout)
{
  auto snapshot = wait_snapshot(job_id, timeout);
  if (!snapshot)
    return std::unexpected(std::move(snapshot.error()));
  if (snapshot->timed_out)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "timed out waiting for background job");
    error.with_context("job_id", std::string(job_id));
    return std::unexpected(std::move(error));
  }
  return snapshot;
}

ava::core::Result<BackgroundJobSnapshot> BackgroundJobRegistry::wait_snapshot(std::string_view job_id, std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex_);
  auto record = find_record_locked(job_id);
  if (!record)
    return std::unexpected(job_not_found_error(job_id));
  auto const job_id_string = std::string(job_id);
  auto ready = [&]() {
    auto current = jobs_.find(job_id_string);
    return current == jobs_.end() || is_terminal(current->second->snapshot.state);
  };
  if (!changed_.wait_for(lock, timeout, ready))
  {
    auto snapshot = record->snapshot;
    snapshot.timed_out = true;
    return snapshot;
  }
  record = find_record_locked(job_id);
  if (!record)
    return std::unexpected(job_not_found_error(job_id));
  return record->snapshot;
}

std::size_t BackgroundJobRegistry::join_finished()
{
  std::vector<std::shared_ptr<JobRecord>> records;
  {
    std::lock_guard lock(mutex_);
    for (auto const& [_, record] : jobs_)
    {
      if (!record->joined && !record->joining && is_terminal(record->snapshot.state))
      {
        record->joining = true;
        records.push_back(record);
      }
    }
  }

  std::size_t joined = 0;
  std::vector<std::string> joined_job_ids;
  std::vector<std::string> skipped_job_ids;
  for (auto& record : records)
  {
    std::lock_guard thread_lock(record->thread_mutex);
    if (record->thread.joinable() && record->thread.get_id() == std::this_thread::get_id())
    {
      skipped_job_ids.push_back(record->snapshot.job_id);
      continue;
    }
    if (record->thread.joinable())
    {
      record->thread.join();
      ++joined;
    }
    joined_job_ids.push_back(record->snapshot.job_id);
  }
  if (!joined_job_ids.empty() || !skipped_job_ids.empty())
  {
    std::lock_guard lock(mutex_);
    for (auto const& job_id : joined_job_ids)
    {
      auto record = find_record_locked(job_id);
      if (record && is_terminal(record->snapshot.state))
      {
        record->joined = true;
        record->joining = false;
      }
    }
    for (auto const& job_id : skipped_job_ids)
    {
      auto record = find_record_locked(job_id);
      if (record)
        record->joining = false;
    }
    prune_retained_finished_jobs_locked();
    changed_.notify_all();
  }
  return joined;
}

void BackgroundJobRegistry::request_stop_all()
{
  std::vector<std::stop_source> stop_sources;
  {
    std::lock_guard lock(mutex_);
    stop_sources.reserve(jobs_.size());
    for (auto const& [_, record] : jobs_)
    {
      if (record->snapshot.state == BackgroundJobState::Running)
        record->snapshot.cancel_requested = true;
      stop_sources.push_back(record->stop_source);
    }
  }

  for (auto& stop_source : stop_sources)
    stop_source.request_stop();
  changed_.notify_all();
}

void BackgroundJobRegistry::shutdown()
{
  {
    std::lock_guard lock(mutex_);
    accepting_ = false;
  }
  request_stop_all();
  join_all();
  static_cast<void>(join_finished());
}

std::shared_ptr<BackgroundJobRegistry::JobRecord> BackgroundJobRegistry::find_record_locked(std::string_view job_id) const
{
  auto const match = jobs_.find(std::string(job_id));
  if (match == jobs_.end())
    return nullptr;
  return match->second;
}

std::size_t BackgroundJobRegistry::running_job_count_locked() const
{
  return static_cast<std::size_t>(std::ranges::count_if(jobs_, [](auto const& entry) { return entry.second->snapshot.state == BackgroundJobState::Running; }));
}

void BackgroundJobRegistry::prune_retained_finished_jobs_locked()
{
  std::vector<BackgroundJobSnapshot> retained;
  retained.reserve(jobs_.size());
  for (auto const& [_, record] : jobs_)
  {
    if (record->joined && is_terminal(record->snapshot.state))
      retained.push_back(record->snapshot);
  }
  if (retained.size() <= options_.max_retained_finished_jobs)
    return;
  std::ranges::sort(retained, [](BackgroundJobSnapshot const& left, BackgroundJobSnapshot const& right) {
    auto const left_completed = left.completed_at.value_or(left.created_at);
    auto const right_completed = right.completed_at.value_or(right.created_at);
    if (left_completed != right_completed)
      return left_completed < right_completed;
    return left.job_id < right.job_id;
  });
  auto const prune_count = retained.size() - options_.max_retained_finished_jobs;
  for (std::size_t index = 0; index < prune_count; ++index) jobs_.erase(retained[index].job_id);
}

void BackgroundJobRegistry::complete(std::string const& job_id, BackgroundJobCompletion completion)
{
  completion = normalize_completion(job_id, std::move(completion));
  {
    std::lock_guard lock(mutex_);
    auto record = find_record_locked(job_id);
    if (!record)
      return;
    record->snapshot.state = completion.state;
    record->snapshot.completed_at = ava::session::now_timestamp();
    record->snapshot.final_text = std::move(completion.final_text);
    record->snapshot.final_text_truncated = truncate_to_bytes(record->snapshot.final_text, options_.max_final_text_bytes);
    record->snapshot.stop_reason = std::move(completion.stop_reason);
    record->snapshot.error = formatted_error(completion.error);
    record->snapshot.provider_iterations = completion.provider_iterations;
    record->snapshot.tool_calls = completion.tool_calls;
    record->snapshot.tool_iterations = completion.tool_iterations;
  }
  changed_.notify_all();
}

void BackgroundJobRegistry::join_all()
{
  std::vector<std::shared_ptr<JobRecord>> records;
  {
    std::lock_guard lock(mutex_);
    records.reserve(jobs_.size());
    for (auto const& [_, record] : jobs_)
    {
      records.push_back(record);
    }
  }

  for (auto& record : records)
  {
    std::lock_guard thread_lock(record->thread_mutex);
    if (record->thread.joinable() && record->thread.get_id() != std::this_thread::get_id())
    {
      record->thread.join();
    }
  }
}

}  // namespace ava::agent
