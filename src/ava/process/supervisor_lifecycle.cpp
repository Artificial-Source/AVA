#include "sys.h"
#include "ava/process/deadline.h"
#include "ava/process/supervisor_internal.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace ava::process::detail {

using namespace std::chrono_literals;

namespace {

std::optional<ProcessDeadline> cleanup_horizon(LifecyclePolicyV1 const& policy) noexcept
{
  if (!policy.execution_deadline)
    return std::nullopt;
  return ava::process::saturating_deadline_add(*policy.execution_deadline, std::chrono::duration_cast<Clock::duration>(kDefaultCleanupBudget));
}

}  // namespace

Record::Record(std::uint64_t identity, OwnerPathV1 owner_path, std::string key, std::uint64_t aliased_owner, ProcessRoleV1 process_role,
               LifecyclePolicyV1 lifecycle_policy)
    : id(identity),
      owner(std::move(owner_path)),
      owner_key(std::move(key)),
      owner_alias(aliased_owner),
      role(process_role),
      policy(lifecycle_policy),
      created(Clock::now()),
      execution_cleanup_horizon(cleanup_horizon(lifecycle_policy))
{
}

ava::core::Error process_error(ava::core::ErrorCategory category, std::string message)
{
  return ava::core::Error(category, std::move(message));
}

ava::core::Error unsupported_error()
{
  return process_error(ava::core::ErrorCategory::Io, "process supervision is unsupported on this platform");
}

ava::core::Error invalid_error(std::string message)
{
  return process_error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
}

ava::core::Error io_error(std::string message, int error_number)
{
  auto error = process_error(ava::core::ErrorCategory::Io, std::move(message));
  error.with_context("cause", std::strerror(error_number));
  return error;
}

bool requestable_reason(TerminationReasonV1 reason) noexcept
{
  switch (reason)
  {
    case TerminationReasonV1::Canceled:
    case TerminationReasonV1::DeadlineExpired:
    case TerminationReasonV1::OwnerShutdown:
    case TerminationReasonV1::ApplicationShutdown:
    case TerminationReasonV1::OutputLimit:
    case TerminationReasonV1::ProtocolFailure:
    case TerminationReasonV1::UnsupportedSuspension:
      return true;
    case TerminationReasonV1::NaturalExit:
    case TerminationReasonV1::LaunchFailed:
    case TerminationReasonV1::ExecFailed:
      return false;
  }
  return false;
}

std::uint64_t elapsed_milliseconds(Clock::time_point begin, Clock::time_point end) noexcept
{
  if (end <= begin)
    return 0;
  auto const value = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
  return value <= 0 ? 0 : static_cast<std::uint64_t>(value);
}

void release_owner_alias_locked(SupervisorState& state, Record const& record)
{
  auto found = state.owner_aliases.find(record.owner_key);
  if (found == state.owner_aliases.end())
    return;
  if (found->second.references > 1)
    --found->second.references;
  else
    state.owner_aliases.erase(found);
}

void prune_terminal_locked(SupervisorState& state)
{
  while (state.terminal_fifo.size() > kMaxRetainedProcessRecordsV1)
  {
    auto const identity = state.terminal_fifo.front();
    state.terminal_fifo.pop_front();
    auto found = state.records.find(identity);
    if (found == state.records.end() || found->second->state != ProcessStateV1::Finished)
      continue;
    release_owner_alias_locked(state, *found->second);
    state.records.erase(found);
  }
}

void await_internal_settlement(std::shared_ptr<HandleState> const& handle, ProcessDeadline deadline) noexcept
{
  if (!handle)
    return;
  std::unique_lock lock(handle->mutex);
  static_cast<void>(handle->changed.wait_until(lock, deadline, [&] { return handle->final_status.has_value(); }));
}

void signal_completion_locked(HandleState& handle) noexcept
{
#if !defined(_WIN32)
  if (!handle.completion_channel || handle.completion_signaled)
    return;
  char const signal = 'F';
  ssize_t result = -1;
  do
    result = ::write(handle.completion_channel->write_end.get(), &signal, 1);
  while (result < 0 && errno == EINTR);
  if (result == 1)
    handle.completion_signaled = true;
  else
    handle.completion_channel->write_end.reset();
#else
  static_cast<void>(handle);
#endif
}

void publish_final_locked(Record& record, ExitStatusV1 status)
{
  if (!record.handle)
    return;
  auto handle = record.handle;
  {
    std::lock_guard handle_lock(handle->mutex);
    if (!handle->final_status)
    {
      handle->final_status = status;
      signal_completion_locked(*handle);
    }
  }
  handle->changed.notify_all();
  // Once publication is complete, the caller's capability is the only reason
  // to retain the bounded value cell and its optional completion channel.
  record.handle.reset();
}

void finalize_locked(SupervisorState& state, Record& record, CleanupStateV1 cleanup)
{
  if (record.state == ProcessStateV1::Finished)
    return;
#if !defined(_WIN32)
  // Bounded settlement is not the end of native-child ownership. Transfer only
  // exact unreaped identities to the joinable reaper before terminal pruning.
  if (record.deferred_cleanup)
  {
    record.deferred_cleanup->children = {record.leader_reaped ? -1 : record.leader, record.sentinel_reaped ? -1 : record.sentinel};
    state.child_reaper.retain(std::move(record.deferred_cleanup));
  }
  record.leader = -1;
  record.sentinel = -1;
  // In-flight poll batches keep their shared descriptor identities alive.
  detach_record_watches_locked(record);
#endif
  if (!record.reason)
    record.reason = TerminationReasonV1::LaunchFailed;
  record.cleanup = cleanup;
  if (cleanup == CleanupStateV1::Incomplete && record.exit_kind == ExitKindV1::None)
    record.exit_kind = ExitKindV1::CleanupIncomplete;
  record.state = ProcessStateV1::Finished;
  record.finished = Clock::now();
  ++record.settlement_count;
  if (state.live_records > 0)
    --state.live_records;
  ++state.settled_records;

  ExitStatusV1 status{.reason = *record.reason,
                      .kind = record.exit_kind,
                      .cleanup = cleanup,
                      .exit_code = record.exit_code,
                      .signal_number = record.signal_number,
                      .has_exit_code = record.has_exit_code,
                      .has_signal_number = record.has_signal_number};
  publish_final_locked(record, status);
  state.terminal_fifo.push_back(record.id);
  prune_terminal_locked(state);
  state.changed.notify_all();
}

bool commit_reason_locked(Record& record, TerminationReasonV1 reason) noexcept
{
  if (record.reason)
    return false;
  record.reason = reason;
  return true;
}

bool set_earlier_stop_deadline_locked(Record& record, ProcessDeadline deadline) noexcept
{
  if (record.stop_deadline && *record.stop_deadline <= deadline)
    return false;
  record.stop_deadline = deadline;
  return true;
}

bool commit_due_execution_deadline_locked(Record& record, Clock::time_point now) noexcept
{
  if (record.state == ProcessStateV1::Finished || record.execution_deadline_processed || !record.policy.execution_deadline ||
      now < *record.policy.execution_deadline)
    return false;

  record.execution_deadline_processed = true;
  bool const earlier_stop = record.execution_cleanup_horizon && set_earlier_stop_deadline_locked(record, *record.execution_cleanup_horizon);
  bool const new_reason = commit_reason_locked(record, TerminationReasonV1::DeadlineExpired);
  if (new_reason && record.registered)
  {
    record.cleanup = CleanupStateV1::Pending;
    if (record.state != ProcessStateV1::Reaping)
      record.state = ProcessStateV1::StopRequested;
  }
#if !defined(_WIN32)
  if (new_reason || earlier_stop)
    reset_record_probe_schedule_locked(record, now);
#endif
  return new_reason;
}

static void mark_launch_error_locked(Record& record) noexcept
{
  if (record.reason != TerminationReasonV1::LaunchFailed && record.reason != TerminationReasonV1::ExecFailed)
    return;
  record.exit_kind = ExitKindV1::LaunchError;
  record.exit_code = 0;
  record.signal_number = 0;
  record.has_exit_code = false;
  record.has_signal_number = false;
}

void abandon_reservation(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity) noexcept
{
  if (!state || identity == 0)
    return;
  {
    std::lock_guard lock(state->mutex);
    auto found = state->records.find(identity);
    if (found == state->records.end() || found->second->state != ProcessStateV1::Reserved || found->second->registered)
      return;
    if (commit_due_execution_deadline_locked(*found->second, Clock::now()))
    {
      finalize_locked(*state, *found->second, CleanupStateV1::NotRequired);
    }
    else
    {
      release_owner_alias_locked(*state, *found->second);
      state->records.erase(found);
      if (state->live_records > 0)
        --state->live_records;
      state->changed.notify_all();
    }
  }
  notify_monitor_state(state);
}

std::optional<ProcessRoleV1> record_role(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity) noexcept
{
  if (!state || identity == 0)
    return std::nullopt;
  std::lock_guard lock(state->mutex);
  auto const found = state->records.find(identity);
  return found == state->records.end() ? std::nullopt : std::optional(found->second->role);
}

TerminationReasonV1 finish_unregistered(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity, TerminationReasonV1 fallback,
                                        CleanupStateV1 cleanup) noexcept
{
  if (!state || identity == 0)
    return fallback;
  TerminationReasonV1 committed = fallback;
  {
    std::lock_guard lock(state->mutex);
    auto found = state->records.find(identity);
    if (found == state->records.end())
      return fallback;
    auto& record = *found->second;
    if (record.state == ProcessStateV1::Finished)
      return record.reason.value_or(fallback);
    static_cast<void>(commit_due_execution_deadline_locked(record, Clock::now()));
    static_cast<void>(commit_reason_locked(record, fallback));
    committed = record.reason.value_or(fallback);
    mark_launch_error_locked(record);
    finalize_locked(*state, record, cleanup);
  }
  notify_monitor_state(state);
  return committed;
}

ProcessDeadline startup_deadline_for_record(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity) noexcept
{
  auto const now = Clock::now();
  std::lock_guard lock(state->mutex);
  auto const found = state->records.find(identity);
  if (found == state->records.end())
    return now;
  auto deadline = now + found->second->policy.startup_timeout;
  if (found->second->policy.execution_deadline && *found->second->policy.execution_deadline < deadline)
    deadline = *found->second->policy.execution_deadline;
  return deadline;
}

static ava::core::VoidResult invoke_launch_hook(std::shared_ptr<AfterForkBeforeReleaseHook> const& hook)
{
  if (!hook || !*hook)
    return {};
  try
  {
    (*hook)();
    return {};
  }
  catch (...)
  {
    return std::unexpected(process_error(ava::core::ErrorCategory::Io, "process launch test seam failed"));
  }
}

ava::core::VoidResult invoke_after_fork_before_release_hook(std::shared_ptr<SupervisorState> const& state)
{
  std::shared_ptr<AfterForkBeforeReleaseHook> hook;
  {
    std::lock_guard lock(state->mutex);
    hook = state->after_fork_before_release_for_test;
  }
  return invoke_launch_hook(hook);
}

ava::core::VoidResult invoke_after_gate_release_hook(std::shared_ptr<SupervisorState> const& state)
{
  std::shared_ptr<AfterForkBeforeReleaseHook> hook;
  {
    std::lock_guard lock(state->mutex);
    hook = state->after_gate_release_for_test;
  }
  return invoke_launch_hook(hook);
}

PreForkDecision check_pre_fork_launch_locked(SupervisorState& state, std::uint64_t identity, ProcessDeadline startup_deadline) noexcept
{
  auto const now = Clock::now();
  auto found = state.records.find(identity);
  if (found == state.records.end())
    return {};
  auto& record = *found->second;
  static_cast<void>(commit_due_execution_deadline_locked(record, now));
  bool const launchable = record.state == ProcessStateV1::Launching && !record.reason && !state.shutting_down && now < startup_deadline;
  auto const reason = record.reason.value_or(state.shutting_down ? TerminationReasonV1::ApplicationShutdown : TerminationReasonV1::LaunchFailed);
  return {.launchable = launchable, .reason = reason};
}

GateReleaseDecision commit_gate_release_locked(SupervisorState& state, std::uint64_t identity, ProcessDeadline startup_deadline) noexcept
{
  auto const now = Clock::now();
  auto found = state.records.find(identity);
  if (found == state.records.end())
    return GateReleaseDecision{.cleanup_deadline = now + kDefaultCleanupBudget};
  auto& record = *found->second;
  static_cast<void>(commit_due_execution_deadline_locked(record, now));
  bool const launchable = record.registered && record.state == ProcessStateV1::Launching && !record.reason && !state.shutting_down &&
                          !record.release_committed && now < startup_deadline;
  if (launchable)
  {
    record.release_committed = true;
    record.state = ProcessStateV1::Running;
    return GateReleaseDecision{.committed = true, .reason = TerminationReasonV1::LaunchFailed, .cleanup_deadline = startup_deadline};
  }

  bool const new_reason =
      !record.reason && commit_reason_locked(record, state.shutting_down ? TerminationReasonV1::ApplicationShutdown : TerminationReasonV1::LaunchFailed);
  mark_launch_error_locked(record);
  if (record.state != ProcessStateV1::Finished)
  {
    bool const new_deadline = set_earlier_stop_deadline_locked(record, now + kDefaultCleanupBudget);
#if !defined(_WIN32)
    if (new_reason || new_deadline)
      reset_record_probe_schedule_locked(record, now);
#endif
    if (record.registered && record.state != ProcessStateV1::Reaping)
      record.state = ProcessStateV1::StopRequested;
  }
  return GateReleaseDecision{.committed = false,
                             .reason = record.reason.value_or(TerminationReasonV1::LaunchFailed),
                             .cleanup_deadline = std::min(record.stop_deadline.value_or(now), now + kDefaultCleanupBudget)};
}

ava::core::Error canceled_launch_error(std::string operation, TerminationReasonV1 reason)
{
  auto error = process_error(ava::core::ErrorCategory::Io, std::move(operation) + " was canceled before its registered gate release");
  error.with_context("reason", std::string(to_string(reason)));
  return error;
}

ava::core::Error startup_stopped_error(std::string operation, TerminationReasonV1 reason)
{
  auto error = process_error(ava::core::ErrorCategory::Io, std::move(operation) + " stopped before exec confirmation");
  error.with_context("reason", std::string(to_string(reason)));
  return error;
}

GateReleaseDecision fail_registered_launch(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity, TerminationReasonV1 fallback) noexcept
{
  auto const now = Clock::now();
  std::lock_guard lock(state->mutex);
  auto found = state->records.find(identity);
  if (found == state->records.end())
    return GateReleaseDecision{.reason = fallback, .cleanup_deadline = now};
  auto& record = *found->second;
  if (record.state == ProcessStateV1::Finished)
  {
    return GateReleaseDecision{.reason = record.reason.value_or(fallback),
                               .cleanup_deadline = std::min(record.stop_deadline.value_or(now), now + kDefaultCleanupBudget)};
  }
  bool const deadline_committed = commit_due_execution_deadline_locked(record, now);
  bool const new_reason = commit_reason_locked(record, fallback);
  mark_launch_error_locked(record);
  bool const new_deadline = set_earlier_stop_deadline_locked(record, now + kDefaultCleanupBudget);
#if !defined(_WIN32)
  if (!deadline_committed && (new_reason || new_deadline))
    reset_record_probe_schedule_locked(record, now);
#endif
  if (record.registered && record.state != ProcessStateV1::Reaping)
    record.state = ProcessStateV1::StopRequested;
  return GateReleaseDecision{.reason = record.reason.value_or(fallback), .cleanup_deadline = std::min(*record.stop_deadline, now + kDefaultCleanupBudget)};
}

ProcessDeadline provisional_cleanup_deadline(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity, TerminationReasonV1 fallback,
                                             ProcessDeadline proposed_deadline) noexcept
{
  auto const now = Clock::now();
  std::lock_guard lock(state->mutex);
  auto found = state->records.find(identity);
  if (found == state->records.end())
    return proposed_deadline;
  auto& record = *found->second;
  bool const execution_due = record.policy.execution_deadline && now >= *record.policy.execution_deadline;
  if (record.state == ProcessStateV1::Finished)
  {
    if (execution_due && record.execution_cleanup_horizon)
      return record.stop_deadline ? std::min(*record.stop_deadline, *record.execution_cleanup_horizon) : *record.execution_cleanup_horizon;
    return proposed_deadline;
  }
  bool const deadline_committed = commit_due_execution_deadline_locked(record, now);
  bool const new_reason = commit_reason_locked(record, fallback);
  mark_launch_error_locked(record);
  bool const new_deadline = !record.execution_deadline_processed && set_earlier_stop_deadline_locked(record, proposed_deadline);
#if !defined(_WIN32)
  if (!deadline_committed && (new_reason || new_deadline))
    reset_record_probe_schedule_locked(record, now);
#endif
  return record.stop_deadline.value_or(proposed_deadline);
}

}  // namespace ava::process::detail
