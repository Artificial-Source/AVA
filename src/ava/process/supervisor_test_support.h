#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/process/supervisor.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ava::process::testing {

enum class ProcessMonitorBackendMode
{
  Automatic,
  ForceFallbackUnavailable,
  ForceFallbackDenied,
};

struct ProcessMonitorCounters
{
  std::uint64_t wake_attempts = 0;
  std::uint64_t wake_drains = 0;
  std::uint64_t wake_coalesces = 0;
  std::uint64_t wake_failures = 0;
  std::uint64_t poll_calls = 0;
  std::uint64_t poll_timeouts = 0;
  std::uint64_t poll_interruptions = 0;
  std::uint64_t poll_peak_entries = 0;
  std::uint64_t pidfd_attempts = 0;
  std::uint64_t pidfd_successes = 0;
  std::uint64_t pidfd_unavailable_failures = 0;
  std::uint64_t pidfd_denied_failures = 0;
  std::uint64_t pidfd_resource_failures = 0;
  std::uint64_t pidfd_other_failures = 0;
  std::uint64_t exact_probes = 0;
  std::uint64_t fallback_probes = 0;
  std::uint64_t short_probes = 0;
  std::array<std::uint64_t, 8> fallback_delay_buckets{};
  std::uint64_t group_observations = 0;
  std::uint64_t quiet_group_proofs = 0;
  std::uint64_t current_watches = 0;
  std::uint64_t peak_watches = 0;
  std::uint64_t stale_events = 0;
  std::uint64_t pollnval_events = 0;
  std::uint64_t current_wake_descriptors = 0;
  std::uint64_t peak_wake_descriptors = 0;
  std::uint64_t current_monitor_threads = 0;
  std::uint64_t peak_monitor_threads = 0;
  std::uint64_t monitor_cycles = 0;
  std::uint64_t cloexec_checks = 0;
  std::uint64_t cloexec_failures = 0;
  std::uint64_t nonblocking_checks = 0;
  std::uint64_t nonblocking_failures = 0;
  bool cloexec_invariant = true;
  bool nonblocking_invariant = true;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ProcessMonitorRecordRow
{
  std::uint64_t record_alias = 0;
  std::size_t active_waiter_count = 0;
  std::uint64_t current_delay_milliseconds = 0;
  std::uint64_t probe_count = 0;
  std::uint64_t reset_count = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ProcessMonitorSnapshot
{
  ProcessMonitorBackendMode backend_mode = ProcessMonitorBackendMode::Automatic;
  ProcessMonitorCounters counters;
  std::vector<ProcessMonitorRecordRow> records;
  bool monitor_started = false;
  bool monitor_joined = false;
  bool pidfd_selected = false;
  bool wake_uses_eventfd = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Narrow per-supervisor deterministic race seams and content-free telemetry.
// Production code does not include this header and a Supervisor owns installed
// callbacks. Backend selection is accepted only before lazy monitor startup.
class SupervisorTestAccess final
{
 public:
  static void set_after_fork_before_release_hook(Supervisor& supervisor, std::function<void()> hook);
  static void clear_after_fork_before_release_hook(Supervisor& supervisor) noexcept;
  // Runs on the launching thread after releasing the child, before classifying
  // exec confirmation, and without the supervisor mutex. May block the launcher
  // while the monitor independently observes the child.
  static void set_after_gate_release_hook(Supervisor& supervisor, std::function<void()> hook);
  static void clear_after_gate_release_hook(Supervisor& supervisor) noexcept;
  static void set_after_completion_channel_create_hook(Supervisor& supervisor, std::function<void()> hook);
  static void clear_after_completion_channel_create_hook(Supervisor& supervisor) noexcept;
  static void fail_next_common_child_working_directory(Supervisor& supervisor) noexcept;

  [[nodiscard]] static bool set_monitor_backend_mode(Supervisor& supervisor, ProcessMonitorBackendMode mode) noexcept;
  [[nodiscard]] static ProcessMonitorSnapshot monitor_snapshot(Supervisor const& supervisor);
  [[nodiscard]] static std::uint64_t monitor_cycle(Supervisor const& supervisor) noexcept;
  [[nodiscard]] static std::uint64_t pulse_monitor(Supervisor& supervisor) noexcept;
  [[nodiscard]] static bool wait_for_monitor_cycle(Supervisor& supervisor, std::uint64_t previous_cycle, ProcessDeadline deadline) noexcept;
  [[nodiscard]] static std::size_t retained_native_child_count(Supervisor const& supervisor) noexcept;
  static void interrupt_next_monitor_poll(Supervisor& supervisor) noexcept;
  // Runs on the monitor thread after processing records and constructing its
  // next poll batch, outside the supervisor mutex and before entering poll.
  // A snapshot taken here can acknowledge an observed record predicate; this
  // hook does not acknowledge that the next poll has been entered. Clearing a
  // hook does not release an invocation already blocked in the callback.
  static void set_after_poll_snapshot_hook(Supervisor& supervisor, std::function<void()> hook);
  static void clear_after_poll_snapshot_hook(Supervisor& supervisor) noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::process::testing
