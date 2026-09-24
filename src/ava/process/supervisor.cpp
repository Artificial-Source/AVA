#include "sys.h"
#include "ava/process/supervisor_internal.h"
#include "ava/process/supervisor_test_support.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif

namespace ava::process {

using detail::Clock;
using namespace std::chrono_literals;

PipeWatchV1::PipeWatchV1(std::weak_ptr<detail::PipeEndpointState> endpoint, PipeInterestV1 interest, std::uint32_t token) noexcept
    : endpoint_(std::move(endpoint)), interest_(interest), token_(token)
{
}

PipeEndpoint::PipeEndpoint() noexcept = default;
PipeEndpoint::PipeEndpoint(std::unique_ptr<Impl> implementation) noexcept : implementation_(std::move(implementation))
{
}
PipeEndpoint::PipeEndpoint(PipeEndpoint&&) noexcept = default;
PipeEndpoint& PipeEndpoint::operator=(PipeEndpoint&&) noexcept = default;
PipeEndpoint::~PipeEndpoint() = default;

bool PipeEndpoint::valid() const noexcept
{
#if !defined(_WIN32)
  if (!implementation_ || !implementation_->state)
    return false;
  std::lock_guard lock(implementation_->state->mutex);
  return implementation_->state->descriptor.get() >= 0;
#else
  return false;
#endif
}

void PipeEndpoint::close() noexcept
{
#if !defined(_WIN32)
  if (implementation_ && implementation_->state)
  {
    std::lock_guard lock(implementation_->state->mutex);
    implementation_->state->descriptor.reset();
  }
#endif
}

ava::core::Result<PipeIoResultV1> PipeEndpoint::read(std::span<std::byte> destination)
{
#if defined(_WIN32)
  static_cast<void>(destination);
  return std::unexpected(detail::unsupported_error());
#else
  if (!implementation_ || !implementation_->state)
    return std::unexpected(detail::invalid_error("process pipe endpoint is not readable"));
  auto state = implementation_->state;
  std::lock_guard lock(state->mutex);
  if (state->descriptor.get() < 0 || !state->readable)
    return std::unexpected(detail::invalid_error("process pipe endpoint is not readable"));
  if (destination.empty())
    return PipeIoResultV1{.bytes = 0, .state = PipeIoStateV1::Progress};
  while (true)
  {
    auto const result = ::read(state->descriptor.get(), destination.data(), destination.size());
    if (result > 0)
      return PipeIoResultV1{.bytes = static_cast<std::size_t>(result), .state = PipeIoStateV1::Progress};
    if (result == 0)
      return PipeIoResultV1{.bytes = 0, .state = PipeIoStateV1::EndOfStream};
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return PipeIoResultV1{.bytes = 0, .state = PipeIoStateV1::WouldBlock};
    return std::unexpected(detail::io_error("failed to read a process pipe endpoint", errno));
  }
#endif
}

ava::core::Result<PipeIoResultV1> PipeEndpoint::write(std::span<std::byte const> source)
{
#if defined(_WIN32)
  static_cast<void>(source);
  return std::unexpected(detail::unsupported_error());
#else
  if (!implementation_ || !implementation_->state)
    return std::unexpected(detail::invalid_error("process pipe endpoint is not writable"));
  auto state = implementation_->state;
  std::lock_guard lock(state->mutex);
  if (state->descriptor.get() < 0 || !state->writable)
    return std::unexpected(detail::invalid_error("process pipe endpoint is not writable"));
  if (source.empty())
    return PipeIoResultV1{.bytes = 0, .state = PipeIoStateV1::Progress};

  auto const result = ::write(state->descriptor.get(), source.data(), source.size());
  int const saved_errno = errno;
  if (result >= 0)
    return PipeIoResultV1{.bytes = static_cast<std::size_t>(result), .state = PipeIoStateV1::Progress};
  if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)
    return PipeIoResultV1{.bytes = 0, .state = PipeIoStateV1::WouldBlock};
  return std::unexpected(detail::io_error("failed to write a process pipe endpoint", saved_errno));
#endif
}

ava::core::Result<bool> PipeEndpoint::wait_readable(ProcessDeadline deadline) const
{
#if defined(_WIN32)
  static_cast<void>(deadline);
  return std::unexpected(detail::unsupported_error());
#else
  if (!implementation_ || !implementation_->state)
    return std::unexpected(detail::invalid_error("process pipe endpoint is not readable"));
  auto state = implementation_->state;
  std::lock_guard lock(state->mutex);
  if (state->descriptor.get() < 0 || !state->readable)
    return std::unexpected(detail::invalid_error("process pipe endpoint is not readable"));
  return detail::wait_descriptor(state->descriptor.get(), POLLIN, deadline);
#endif
}

ava::core::Result<bool> PipeEndpoint::wait_writable(ProcessDeadline deadline) const
{
#if defined(_WIN32)
  static_cast<void>(deadline);
  return std::unexpected(detail::unsupported_error());
#else
  if (!implementation_ || !implementation_->state)
    return std::unexpected(detail::invalid_error("process pipe endpoint is not writable"));
  auto state = implementation_->state;
  std::lock_guard lock(state->mutex);
  if (state->descriptor.get() < 0 || !state->writable)
    return std::unexpected(detail::invalid_error("process pipe endpoint is not writable"));
  return detail::wait_descriptor(state->descriptor.get(), POLLOUT, deadline);
#endif
}

ava::core::Result<PipeWatchV1> PipeEndpoint::watch(PipeInterestV1 interest, std::uint32_t token) const
{
  if (!is_valid(interest))
    return std::unexpected(detail::invalid_error("process pipe watch has an unknown interest"));
#if defined(_WIN32)
  static_cast<void>(token);
  return std::unexpected(detail::unsupported_error());
#else
  if (!implementation_ || !implementation_->state)
    return std::unexpected(detail::invalid_error("process pipe endpoint is closed"));
  auto state = implementation_->state;
  std::lock_guard lock(state->mutex);
  if (state->descriptor.get() < 0)
    return std::unexpected(detail::invalid_error("process pipe endpoint is closed"));
  if (interest == PipeInterestV1::Readable && !state->readable)
    return std::unexpected(detail::invalid_error("process pipe endpoint does not support readable watches"));
  if (interest == PipeInterestV1::Writable && !state->writable)
    return std::unexpected(detail::invalid_error("process pipe endpoint does not support writable watches"));
  return PipeWatchV1(state, interest, token);
#endif
}

Reservation::Reservation() noexcept = default;
Reservation::Reservation(std::shared_ptr<detail::SupervisorState> state, std::uint64_t record) noexcept : state_(std::move(state)), record_(record)
{
}
Reservation::Reservation(Reservation&& other) noexcept : state_(std::move(other.state_)), record_(std::exchange(other.record_, 0))
{
}
Reservation& Reservation::operator=(Reservation&& other) noexcept
{
  if (this != &other)
  {
    abandon();
    state_ = std::move(other.state_);
    record_ = std::exchange(other.record_, 0);
  }
  return *this;
}
Reservation::~Reservation()
{
  abandon();
}

bool Reservation::valid() const noexcept
{
  return state_ && record_ != 0;
}

void Reservation::abandon() noexcept
{
  detail::abandon_reservation(state_, record_);
  record_ = 0;
  state_.reset();
}

ProcessHandle::ProcessHandle() noexcept = default;
ProcessHandle::ProcessHandle(std::shared_ptr<detail::HandleState> state) noexcept : state_(std::move(state))
{
}
ProcessHandle::ProcessHandle(ProcessHandle&&) noexcept = default;
ProcessHandle& ProcessHandle::operator=(ProcessHandle&&) noexcept = default;
ProcessHandle::~ProcessHandle() = default;

bool ProcessHandle::valid() const noexcept
{
  return state_ && state_->record != 0;
}

Supervisor::Supervisor() : implementation_(std::make_unique<Impl>())
{
}

Supervisor::~Supervisor() noexcept
{
  if (!implementation_)
    return;
  static_cast<void>(shutdown(Clock::now() + detail::kDefaultCleanupBudget));
}

ava::core::Result<Reservation> Supervisor::reserve(OwnerPathV1 const& owner, ProcessRoleV1 role, LifecyclePolicyV1 policy)
{
  auto const validation_now = Clock::now();
  if (!owner.is_launch_owner())
    return std::unexpected(detail::invalid_error("process reservation requires a generated operation owner"));
  if (!is_valid(role))
    return std::unexpected(detail::invalid_error("process reservation has an unknown version-1 role"));
  if (policy.termination_grace < 0ms || policy.termination_grace > 5s)
    return std::unexpected(detail::invalid_error("process termination grace is outside the supported bound"));
  if (policy.startup_timeout <= 0ms || policy.startup_timeout > detail::kMaximumStartupTimeout)
    return std::unexpected(detail::invalid_error("process startup timeout is outside the supported bound"));
  if (policy.execution_deadline && *policy.execution_deadline <= validation_now)
    return std::unexpected(detail::invalid_error("process execution deadline is not in the future"));
  if (role == ProcessRoleV1::BrowserOpener && !policy.execution_deadline)
    return std::unexpected(detail::invalid_error("browser process reservations require an execution deadline"));
  if (role == ProcessRoleV1::BrowserOpener && policy.execution_deadline && *policy.execution_deadline > validation_now + detail::kMaximumBrowserExecutionWindow)
    return std::unexpected(detail::invalid_error("browser process execution deadline exceeds the supported bound"));

  try
  {
    auto key = owner.key();
    auto state = implementation_->state;
    std::uint64_t identity = 0;
    {
      std::lock_guard lock(state->mutex);
      if (!state->accepting || state->shutting_down)
        return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "process supervisor is not accepting reservations"));
      if (state->live_records >= kMaxLiveProcessRecordsV1)
        return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "process supervisor live-record capacity is exhausted"));
      if (policy.execution_deadline && *policy.execution_deadline <= Clock::now())
        return std::unexpected(detail::invalid_error("process execution deadline expired during reservation"));

      auto alias = state->owner_aliases.find(key);
      if (alias == state->owner_aliases.end())
        alias = state->owner_aliases.emplace(key, detail::OwnerAliasEntry{.alias = state->next_owner_alias++, .references = 0}).first;
      ++alias->second.references;
      identity = state->next_record++;
      auto record = std::make_unique<detail::Record>(identity, owner, key, alias->second.alias, role, policy);
      state->records.emplace(identity, std::move(record));
      ++state->live_records;
      state->changed.notify_all();
    }
    detail::notify_monitor_state(state);
    return Reservation(state, identity);
  }
  catch (std::exception const& error)
  {
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "failed to allocate a process reservation").with_context("cause", error.what()));
  }
  catch (...)
  {
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "failed to allocate a process reservation"));
  }
}

void Supervisor::stop_accepting() noexcept
{
  if (!implementation_)
    return;
  auto const state = implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->accepting = false;
  }
  state->changed.notify_all();
}

ava::core::Result<std::uint64_t> Supervisor::consume_reservation(Reservation& reservation)
{
  auto const expected = implementation_->state;
  if (!reservation.valid() || reservation.state_.get() != expected.get())
    return std::unexpected(detail::invalid_error("process reservation does not belong to this supervisor"));
  auto const identity = reservation.record_;
  bool deadline_expired = false;
  bool no_longer_launchable = false;
  {
    std::lock_guard lock(expected->mutex);
    auto found = expected->records.find(identity);
    if (found == expected->records.end())
      return std::unexpected(detail::invalid_error("process reservation is no longer launchable"));
    if (found->second->state == ProcessStateV1::Finished)
    {
      deadline_expired = found->second->reason == TerminationReasonV1::DeadlineExpired;
      no_longer_launchable = !deadline_expired;
    }
    else if (found->second->state != ProcessStateV1::Reserved)
    {
      return std::unexpected(detail::invalid_error("process reservation is no longer launchable"));
    }
    else if (detail::commit_due_execution_deadline_locked(*found->second, Clock::now()))
    {
      deadline_expired = true;
      detail::finalize_locked(*expected, *found->second, CleanupStateV1::NotRequired);
    }
    else
    {
      found->second->state = ProcessStateV1::Launching;
      found->second->cleanup = CleanupStateV1::Pending;
    }
  }
  if (deadline_expired || no_longer_launchable)
  {
    reservation.record_ = 0;
    reservation.state_.reset();
    detail::notify_monitor_state(expected);
    if (deadline_expired)
      return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "process reservation execution deadline expired before launch"));
    return std::unexpected(detail::invalid_error("process reservation is no longer launchable"));
  }
  reservation.record_ = 0;
  reservation.state_.reset();
  detail::notify_monitor_state(expected);
  return identity;
}

ava::core::Result<StopResultV1> Supervisor::request_stop(ProcessHandle const& handle, TerminationReasonV1 reason, ProcessDeadline deadline)
{
  if (!detail::requestable_reason(reason))
    return std::unexpected(detail::invalid_error("process stop request has a non-requestable termination reason"));
  auto state = implementation_->state;
  if (!handle.valid() || handle.state_->supervisor.lock().get() != state.get())
    return std::unexpected(detail::invalid_error("process handle does not belong to this supervisor"));

  StopResultV1 result;
  {
    std::lock_guard lock(state->mutex);
    auto found = state->records.find(handle.state_->record);
    if (found == state->records.end())
      return result;
    ++result.matched;
    auto& record = *found->second;
    if (record.state != ProcessStateV1::Finished)
    {
      auto const now = Clock::now();
      bool const deadline_committed = detail::commit_due_execution_deadline_locked(record, now);
      bool const new_reason = detail::commit_reason_locked(record, reason);
      result.newly_requested += deadline_committed || new_reason ? 1U : 0U;
      bool const earlier_deadline = detail::set_earlier_stop_deadline_locked(record, deadline);
#if !defined(_WIN32)
      if (!deadline_committed && (new_reason || earlier_deadline))
        detail::reset_record_probe_schedule_locked(record, now);
#endif
      if (record.registered && record.state != ProcessStateV1::Reaping)
        record.state = ProcessStateV1::StopRequested;
      else if (!record.registered)
        detail::finalize_locked(*state, record, CleanupStateV1::NotRequired);
    }
  }
  detail::notify_monitor_state(state);
  return result;
}

ava::core::Result<StopResultV1> Supervisor::request_stop(OwnerPathV1 const& owner_prefix, TerminationReasonV1 reason, ProcessDeadline deadline)
{
  if (!owner_prefix.is_valid_prefix())
    return std::unexpected(detail::invalid_error("process owner stop requires a valid generated owner prefix"));
  if (!detail::requestable_reason(reason))
    return std::unexpected(detail::invalid_error("process owner stop has a non-requestable termination reason"));
  auto state = implementation_->state;
  StopResultV1 result;
  {
    std::lock_guard lock(state->mutex);
    for (auto& [identity, record_pointer] : state->records)
    {
      static_cast<void>(identity);
      auto& record = *record_pointer;
      if (!record.owner.matches_prefix(owner_prefix))
        continue;
      ++result.matched;
      if (record.state == ProcessStateV1::Finished)
        continue;
      auto const now = Clock::now();
      bool const deadline_committed = detail::commit_due_execution_deadline_locked(record, now);
      bool const new_reason = detail::commit_reason_locked(record, reason);
      result.newly_requested += deadline_committed || new_reason ? 1U : 0U;
      bool const earlier_deadline = detail::set_earlier_stop_deadline_locked(record, deadline);
#if !defined(_WIN32)
      if (!deadline_committed && (new_reason || earlier_deadline))
        detail::reset_record_probe_schedule_locked(record, now);
#endif
      if ((record.registered || record.state == ProcessStateV1::Launching) && record.state != ProcessStateV1::Reaping)
        record.state = ProcessStateV1::StopRequested;
      else if (!record.registered && record.state != ProcessStateV1::Reaping)
        detail::finalize_locked(*state, record, CleanupStateV1::NotRequired);
    }
  }
  detail::notify_monitor_state(state);
  return result;
}

ava::core::Result<ExitStatusV1> Supervisor::wait(ProcessHandle const& handle, ProcessDeadline deadline) const
{
  auto state = implementation_->state;
  if (!handle.valid() || handle.state_->supervisor.lock().get() != state.get())
    return std::unexpected(detail::invalid_error("process handle does not belong to this supervisor"));
  auto handle_state = handle.state_;
  auto waiter = detail::register_active_waiter(state, handle_state->record);
  if (!waiter)
    return std::unexpected(std::move(waiter.error()));
  std::unique_lock lock(handle_state->mutex);
  if (!handle_state->changed.wait_until(lock, deadline, [&] { return handle_state->final_status.has_value(); }))
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "process wait deadline expired"));
  return *handle_state->final_status;
}

ava::core::Result<std::optional<ExitStatusV1>> Supervisor::try_wait(ProcessHandle const& handle) const
{
  auto state = implementation_->state;
  if (!handle.valid() || handle.state_->supervisor.lock().get() != state.get())
    return std::unexpected(detail::invalid_error("process handle does not belong to this supervisor"));
  auto handle_state = handle.state_;
  std::lock_guard lock(handle_state->mutex);
  return handle_state->final_status;
}

ava::core::VoidResult Supervisor::account_output(ProcessHandle const& handle, StreamKindV1 stream, std::uint64_t bytes, bool truncated)
{
  auto state = implementation_->state;
  if (!handle.valid() || handle.state_->supervisor.lock().get() != state.get())
    return std::unexpected(detail::invalid_error("process handle does not belong to this supervisor"));
  if (stream != StreamKindV1::StandardOutput && stream != StreamKindV1::StandardError)
    return std::unexpected(detail::invalid_error("process output accounting accepts only stdout or stderr"));
  std::lock_guard lock(state->mutex);
  auto found = state->records.find(handle.state_->record);
  if (found == state->records.end())
    return {};
  if (stream == StreamKindV1::StandardOutput)
  {
    found->second->stdout_bytes = detail::saturating_add(found->second->stdout_bytes, bytes);
    found->second->stdout_truncated = found->second->stdout_truncated || truncated;
  }
  else
  {
    found->second->stderr_bytes = detail::saturating_add(found->second->stderr_bytes, bytes);
    found->second->stderr_truncated = found->second->stderr_truncated || truncated;
  }
  return {};
}

ProcessSnapshotV1 Supervisor::snapshot() const
{
  ProcessSnapshotV1 result;
  if (!implementation_)
    return result;
  auto state = implementation_->state;
  std::lock_guard lock(state->mutex);
  result.accepting = state->accepting;
  result.monitor_started = state->monitor_started;
  result.live_records = state->live_records;
  result.retained_terminal_records = state->terminal_fifo.size();
  result.records.reserve(state->records.size());
  auto const now = Clock::now();
  for (auto const& [identity, record_pointer] : state->records)
  {
    static_cast<void>(identity);
    auto const& record = *record_pointer;
    auto const end = record.finished.value_or(now);
    result.records.push_back(ProcessSnapshotRecordV1{.record_alias = record.id,
                                                     .owner_alias = record.owner_alias,
                                                     .role = record.role,
                                                     .state = record.state,
                                                     .reason = record.reason,
                                                     .cleanup = record.cleanup,
                                                     .exit_kind = record.exit_kind,
                                                     .monotonic_milliseconds = detail::elapsed_milliseconds(record.created, end),
                                                     .stdout_bytes = record.stdout_bytes,
                                                     .stderr_bytes = record.stderr_bytes,
                                                     .settlement_count = record.settlement_count,
                                                     .exit_code = record.exit_code,
                                                     .signal_number = record.signal_number,
                                                     .group_verified = record.group_verified,
                                                     .stdout_truncated = record.stdout_truncated,
                                                     .stderr_truncated = record.stderr_truncated,
                                                     .has_exit_code = record.has_exit_code,
                                                     .has_signal_number = record.has_signal_number});
  }
  return result;
}

ShutdownResultV1 Supervisor::shutdown(ProcessDeadline deadline) noexcept
{
  if (!implementation_)
    return {};
  std::lock_guard shutdown_lock(implementation_->shutdown_mutex);
  if (implementation_->shutdown_called)
    return implementation_->shutdown_result;
  implementation_->shutdown_called = true;
  auto state = implementation_->state;
  std::size_t settled_before = 0;
  {
    std::lock_guard lock(state->mutex);
    state->accepting = false;
    state->shutting_down = true;
    settled_before = state->settled_records;
    for (auto& [identity, record_pointer] : state->records)
    {
      static_cast<void>(identity);
      auto& record = *record_pointer;
      if (record.state == ProcessStateV1::Finished)
        continue;
      record.included_in_shutdown = true;
      auto const now = Clock::now();
      bool const deadline_committed = detail::commit_due_execution_deadline_locked(record, now);
      bool const new_reason = detail::commit_reason_locked(record, TerminationReasonV1::ApplicationShutdown);
      bool const earlier_deadline = detail::set_earlier_stop_deadline_locked(record, deadline);
#if !defined(_WIN32)
      if (!deadline_committed && (new_reason || earlier_deadline))
        detail::reset_record_probe_schedule_locked(record, now);
      if (record.registered)
      {
        detail::begin_stop_locked(record, now);
        continue;
      }
#endif
      if (record.state == ProcessStateV1::Launching && record.state != ProcessStateV1::Reaping)
        record.state = ProcessStateV1::StopRequested;
      else if (!record.registered && record.state != ProcessStateV1::Reaping)
        detail::finalize_locked(*state, record, CleanupStateV1::NotRequired);
    }
  }
  detail::notify_monitor_state(state);

  {
    std::unique_lock lock(state->mutex);
    static_cast<void>(state->changed.wait_until(lock, deadline, [&] { return state->live_records == 0; }));
    if (state->live_records != 0)
    {
      // The common absolute budget is exhausted. Escalate every remaining
      // verified group in one nonblocking sweep, then make exact nonblocking
      // observations without adding a scheduling grace period.
#if !defined(_WIN32)
      for (auto& [identity, record_pointer] : state->records)
      {
        static_cast<void>(identity);
        auto& record = *record_pointer;
        if (record.state != ProcessStateV1::Finished && record.registered && !record.kill_sent)
        {
          static_cast<void>(detail::signal_verified_group(record, SIGKILL));
          record.kill_sent = true;
        }
      }
#endif
      for (auto& [identity, record_pointer] : state->records)
      {
        static_cast<void>(identity);
        auto& record = *record_pointer;
#if !defined(_WIN32)
        if (record.state != ProcessStateV1::Finished && record.registered)
        {
          static_cast<void>(detail::observe_member(record.leader, record.leader_observed, record, true));
          if (record.sentinel > 1)
            static_cast<void>(detail::observe_member(record.sentinel, record.sentinel_observed, record, false));
          bool const direct_children_observed = record.leader_observed && (record.sentinel <= 1 || record.sentinel_observed);
          if (record.quiet_group_observations >= 2 && direct_children_observed)
          {
            static_cast<void>(detail::reap_member(record.leader, true, record.leader_reaped, record));
            if (record.sentinel > 1)
              static_cast<void>(detail::reap_member(record.sentinel, true, record.sentinel_reaped, record));
          }
          bool const reaped = record.leader_reaped && (record.sentinel <= 1 || record.sentinel_reaped);
          detail::finalize_locked(*state, record, reaped && !record.cleanup_failed ? CleanupStateV1::Complete : CleanupStateV1::Incomplete);
          continue;
        }
#endif
        if (record.state != ProcessStateV1::Finished)
          detail::finalize_locked(*state, record, CleanupStateV1::Incomplete);
      }
    }
    std::size_t incomplete = 0;
    for (auto const& [identity, record] : state->records)
    {
      static_cast<void>(identity);
      if (record->included_in_shutdown && (record->state != ProcessStateV1::Finished || record->cleanup == CleanupStateV1::Incomplete))
        ++incomplete;
    }
    implementation_->shutdown_result = ShutdownResultV1{.complete = incomplete == 0,
                                                        .incomplete_count = std::min(incomplete, kMaxLiveProcessRecordsV1),
                                                        .settled_count = state->settled_records - settled_before};
    state->stop_monitor = true;
  }
  detail::notify_monitor_state(state);
#if !defined(_WIN32)
  detail::join_monitor(state);
#endif
  return implementation_->shutdown_result;
}

}  // namespace ava::process
