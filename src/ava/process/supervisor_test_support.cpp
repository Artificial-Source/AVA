#include "sys.h"
#include "ava/process/supervisor_internal.h"
#include "ava/process/supervisor_test_support.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace ava::process::testing {
namespace {

#if !defined(_WIN32)
detail::MonitorBackendMode to_internal(ProcessMonitorBackendMode mode) noexcept
{
  switch (mode)
  {
    case ProcessMonitorBackendMode::Automatic:
      return detail::MonitorBackendMode::Automatic;
    case ProcessMonitorBackendMode::ForceFallbackUnavailable:
      return detail::MonitorBackendMode::ForceFallbackUnavailable;
    case ProcessMonitorBackendMode::ForceFallbackDenied:
      return detail::MonitorBackendMode::ForceFallbackDenied;
  }
  return detail::MonitorBackendMode::Automatic;
}

ProcessMonitorBackendMode from_internal(detail::MonitorBackendMode mode) noexcept
{
  switch (mode)
  {
    case detail::MonitorBackendMode::Automatic:
      return ProcessMonitorBackendMode::Automatic;
    case detail::MonitorBackendMode::ForceFallbackUnavailable:
      return ProcessMonitorBackendMode::ForceFallbackUnavailable;
    case detail::MonitorBackendMode::ForceFallbackDenied:
      return ProcessMonitorBackendMode::ForceFallbackDenied;
  }
  return ProcessMonitorBackendMode::Automatic;
}

std::uint64_t current_record_delay(detail::Record const& record) noexcept
{
  std::uint64_t delay = 0;
  if (record.leader_monitor.fallback_enabled && !record.leader_observed)
    delay = static_cast<std::uint64_t>(std::max<std::int64_t>(0, record.leader_monitor.fallback_interval.count()));
  if (record.sentinel > 1 && record.sentinel_monitor.fallback_enabled && !record.sentinel_observed)
    delay = std::max(delay, static_cast<std::uint64_t>(std::max<std::int64_t>(0, record.sentinel_monitor.fallback_interval.count())));
  return delay;
}
#endif

}  // namespace

void SupervisorTestAccess::set_after_fork_before_release_hook(Supervisor& supervisor, std::function<void()> hook)
{
  auto owned_hook = std::make_shared<detail::AfterForkBeforeReleaseHook>(std::move(hook));
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->after_fork_before_release_for_test = std::move(owned_hook);
  }
  detail::notify_monitor_state(state);
}

void SupervisorTestAccess::clear_after_fork_before_release_hook(Supervisor& supervisor) noexcept
{
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->after_fork_before_release_for_test.reset();
  }
  detail::notify_monitor_state(state);
}

void SupervisorTestAccess::set_after_gate_release_hook(Supervisor& supervisor, std::function<void()> hook)
{
  auto owned_hook = std::make_shared<detail::AfterForkBeforeReleaseHook>(std::move(hook));
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->after_gate_release_for_test = std::move(owned_hook);
  }
  detail::notify_monitor_state(state);
}

void SupervisorTestAccess::clear_after_gate_release_hook(Supervisor& supervisor) noexcept
{
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->after_gate_release_for_test.reset();
  }
  detail::notify_monitor_state(state);
}

void SupervisorTestAccess::set_after_completion_channel_create_hook(Supervisor& supervisor, std::function<void()> hook)
{
  auto owned_hook = std::make_shared<detail::AfterForkBeforeReleaseHook>(std::move(hook));
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->after_completion_channel_create_for_test = std::move(owned_hook);
  }
  detail::notify_monitor_state(state);
}

void SupervisorTestAccess::clear_after_completion_channel_create_hook(Supervisor& supervisor) noexcept
{
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->after_completion_channel_create_for_test.reset();
  }
  detail::notify_monitor_state(state);
}

void SupervisorTestAccess::fail_next_common_child_working_directory(Supervisor& supervisor) noexcept
{
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->fail_next_common_child_working_directory_for_test = true;
  }
  detail::notify_monitor_state(state);
}

bool SupervisorTestAccess::set_monitor_backend_mode(Supervisor& supervisor, ProcessMonitorBackendMode mode) noexcept
{
#if defined(_WIN32)
  static_cast<void>(supervisor);
  static_cast<void>(mode);
  return false;
#else
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    if (state->monitor_started || state->monitor_wake || state->monitor_joined)
      return false;
    state->monitor_backend_mode = to_internal(mode);
    ++state->test_control_generation;
  }
  detail::notify_monitor_state(state);
  return true;
#endif
}

ProcessMonitorSnapshot SupervisorTestAccess::monitor_snapshot(Supervisor const& supervisor)
{
  ProcessMonitorSnapshot result;
  auto const state = supervisor.implementation_->state;
#if !defined(_WIN32)
  auto const telemetry = state->monitor_telemetry;
  auto load = [](std::atomic<std::uint64_t> const& value) { return value.load(std::memory_order_relaxed); };
  result.counters.wake_attempts = load(telemetry->wake_attempts);
  result.counters.wake_drains = load(telemetry->wake_drains);
  result.counters.wake_coalesces = load(telemetry->wake_coalesces);
  result.counters.wake_failures = load(telemetry->wake_failures);
  result.counters.poll_calls = load(telemetry->poll_calls);
  result.counters.poll_timeouts = load(telemetry->poll_timeouts);
  result.counters.poll_interruptions = load(telemetry->poll_interruptions);
  result.counters.poll_peak_entries = load(telemetry->poll_peak_entries);
  result.counters.pidfd_attempts = load(telemetry->pidfd_attempts);
  result.counters.pidfd_successes = load(telemetry->pidfd_successes);
  result.counters.pidfd_unavailable_failures = load(telemetry->pidfd_unavailable_failures);
  result.counters.pidfd_denied_failures = load(telemetry->pidfd_denied_failures);
  result.counters.pidfd_resource_failures = load(telemetry->pidfd_resource_failures);
  result.counters.pidfd_other_failures = load(telemetry->pidfd_other_failures);
  result.counters.exact_probes = load(telemetry->exact_probes);
  result.counters.fallback_probes = load(telemetry->fallback_probes);
  result.counters.short_probes = load(telemetry->short_probes);
  for (std::size_t index = 0; index < result.counters.fallback_delay_buckets.size(); ++index)
    result.counters.fallback_delay_buckets[index] = load(telemetry->fallback_delay_buckets[index]);
  result.counters.group_observations = load(telemetry->group_observations);
  result.counters.quiet_group_proofs = load(telemetry->quiet_group_proofs);
  result.counters.current_watches = load(telemetry->current_watches);
  result.counters.peak_watches = load(telemetry->peak_watches);
  result.counters.stale_events = load(telemetry->stale_events);
  result.counters.pollnval_events = load(telemetry->pollnval_events);
  result.counters.current_wake_descriptors = load(telemetry->current_wake_descriptors);
  result.counters.peak_wake_descriptors = load(telemetry->peak_wake_descriptors);
  result.counters.current_monitor_threads = load(telemetry->current_monitor_threads);
  result.counters.peak_monitor_threads = load(telemetry->peak_monitor_threads);
  result.counters.monitor_cycles = load(telemetry->monitor_cycles);
  result.counters.cloexec_checks = load(telemetry->cloexec_checks);
  result.counters.cloexec_failures = load(telemetry->cloexec_failures);
  result.counters.nonblocking_checks = load(telemetry->nonblocking_checks);
  result.counters.nonblocking_failures = load(telemetry->nonblocking_failures);
  result.counters.cloexec_invariant = result.counters.cloexec_failures == 0;
  result.counters.nonblocking_invariant = result.counters.nonblocking_failures == 0;

  std::lock_guard lock(state->mutex);
  result.backend_mode = from_internal(state->monitor_backend_mode);
  result.monitor_started = state->monitor_started;
  result.monitor_joined = state->monitor_joined;
  result.pidfd_selected = result.counters.pidfd_successes > 0;
  result.wake_uses_eventfd = state->monitor_wake && state->monitor_wake->eventfd;
  result.records.reserve(std::min(state->records.size(), kMaxLiveProcessRecordsV1));
  for (auto const& [identity, record] : state->records)
  {
    if (!record->registered || record->state == ProcessStateV1::Finished || result.records.size() == kMaxLiveProcessRecordsV1)
      continue;
    result.records.push_back(ProcessMonitorRecordRow{
        .record_alias = identity,
        .active_waiter_count = record->active_waiters,
        .current_delay_milliseconds = current_record_delay(*record),
        .probe_count = record->leader_monitor.probe_count + record->sentinel_monitor.probe_count,
        .reset_count = record->leader_monitor.reset_count + record->sentinel_monitor.reset_count,
    });
  }
#else
  std::lock_guard lock(state->mutex);
  result.monitor_started = state->monitor_started;
  result.monitor_joined = state->monitor_joined;
#endif
  return result;
}

std::uint64_t SupervisorTestAccess::monitor_cycle(Supervisor const& supervisor) noexcept
{
#if defined(_WIN32)
  static_cast<void>(supervisor);
  return 0;
#else
  return supervisor.implementation_->state->monitor_telemetry->monitor_cycles.load(std::memory_order_relaxed);
#endif
}

std::uint64_t SupervisorTestAccess::pulse_monitor(Supervisor& supervisor) noexcept
{
#if defined(_WIN32)
  static_cast<void>(supervisor);
  return 0;
#else
  auto const state = supervisor.implementation_->state;
  auto const previous = state->monitor_telemetry->monitor_cycles.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(state->mutex);
    ++state->test_control_generation;
  }
  detail::notify_monitor_state(state);
  return previous;
#endif
}

bool SupervisorTestAccess::wait_for_monitor_cycle(Supervisor& supervisor, std::uint64_t previous_cycle, ProcessDeadline deadline) noexcept
{
#if defined(_WIN32)
  static_cast<void>(supervisor);
  static_cast<void>(previous_cycle);
  static_cast<void>(deadline);
  return false;
#else
  auto const state = supervisor.implementation_->state;
  std::unique_lock lock(state->mutex);
  return state->changed.wait_until(
      lock, deadline, [&] { return state->monitor_telemetry->monitor_cycles.load(std::memory_order_relaxed) > previous_cycle || state->monitor_joined; });
#endif
}

std::size_t SupervisorTestAccess::retained_native_child_count(Supervisor const& supervisor) noexcept
{
#if defined(_WIN32)
  static_cast<void>(supervisor);
  return 0;
#else
  auto const state = supervisor.implementation_->state;
  std::lock_guard lock(state->mutex);
  std::size_t result = state->child_reaper.pending();
  for (auto const& [identity, record] : state->records)
  {
    static_cast<void>(identity);
    if (record->leader > 1 && !record->leader_reaped)
      ++result;
    if (record->sentinel > 1 && !record->sentinel_reaped)
      ++result;
  }
  return result;
#endif
}

void SupervisorTestAccess::interrupt_next_monitor_poll(Supervisor& supervisor) noexcept
{
#if !defined(_WIN32)
  auto const state = supervisor.implementation_->state;
  state->poll_interruptions_for_test.fetch_add(1, std::memory_order_relaxed);
  detail::notify_monitor_state(state);
#else
  static_cast<void>(supervisor);
#endif
}

void SupervisorTestAccess::set_after_poll_snapshot_hook(Supervisor& supervisor, std::function<void()> hook)
{
#if !defined(_WIN32)
  auto owned_hook = std::make_shared<detail::AfterForkBeforeReleaseHook>(std::move(hook));
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->after_poll_snapshot_for_test = std::move(owned_hook);
    ++state->test_control_generation;
  }
  detail::notify_monitor_state(state);
#else
  static_cast<void>(supervisor);
  static_cast<void>(hook);
#endif
}

void SupervisorTestAccess::clear_after_poll_snapshot_hook(Supervisor& supervisor) noexcept
{
#if !defined(_WIN32)
  auto const state = supervisor.implementation_->state;
  {
    std::lock_guard lock(state->mutex);
    state->after_poll_snapshot_for_test.reset();
    ++state->test_control_generation;
  }
  detail::notify_monitor_state(state);
#else
  static_cast<void>(supervisor);
#endif
}

}  // namespace ava::process::testing
