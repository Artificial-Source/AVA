#include "sys.h"
#include "tests/support/process_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/process/environment.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/process/supervisor_test_support.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef AVA_FAKE_PROCESS_CHILD_PATH
#define AVA_FAKE_PROCESS_CHILD_PATH ""
#endif

#if !defined(_WIN32)
namespace {

using namespace std::chrono_literals;
using ava::process::testing::ProcessMonitorBackendMode;
using ava::process::testing::ProcessMonitorSnapshot;
using ava::process::testing::SupervisorTestAccess;

ava::process::OwnerPathV1 application_owner()
{
  auto owner = ava::process::OwnerPathV1::application();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::process::OwnerPathV1 operation_owner(ava::process::OwnerPathV1 const& application)
{
  auto owner = application.operation();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::core::Result<ava::process::SpawnResultV1> spawn_fixture(ava::process::Supervisor& supervisor, ava::process::OwnerPathV1 const& application,
                                                             std::string mode, ava::process::LifecyclePolicyV1 policy = {}, bool capture_output = false)
{
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin, policy);
  if (!reservation)
    return std::unexpected(std::move(reservation.error()));
  auto environment = ava::process::make_plugin_environment_v1("/");
  if (!environment)
    return std::unexpected(std::move(environment.error()));
  bool const gated = mode == "input-gate";
  return supervisor.spawn(std::move(*reservation), {.executable = AVA_FAKE_PROCESS_CHILD_PATH,
                                                    .argv = {AVA_FAKE_PROCESS_CHILD_PATH, std::move(mode)},
                                                    .environment = std::move(*environment),
                                                    .cwd = "/",
                                                    .stdin_mode = gated ? ava::process::StreamModeV1::Capture : ava::process::StreamModeV1::Discard,
                                                    .stdout_mode = capture_output ? ava::process::StreamModeV1::Capture : ava::process::StreamModeV1::Discard,
                                                    .stderr_mode = ava::process::StreamModeV1::Discard});
}

using ava::tests::now_plus_seconds;

bool wait_for_cycle(ava::process::Supervisor& supervisor, std::uint64_t previous, std::chrono::milliseconds timeout = 2s)
{
  return SupervisorTestAccess::wait_for_monitor_cycle(supervisor, previous, now_plus_seconds(timeout));
}

template <typename Predicate>
std::optional<ProcessMonitorSnapshot> wait_for_snapshot(ava::process::Supervisor& supervisor, Predicate predicate, std::chrono::milliseconds timeout = 10s,
                                                        bool wake_monitor = false)
{
  auto const deadline = now_plus_seconds(timeout);
  while (std::chrono::steady_clock::now() < deadline)
  {
    auto snapshot = SupervisorTestAccess::monitor_snapshot(supervisor);
    if (predicate(snapshot))
      return snapshot;
    auto const cycle = wake_monitor ? SupervisorTestAccess::pulse_monitor(supervisor) : snapshot.counters.monitor_cycles;
    if (!SupervisorTestAccess::wait_for_monitor_cycle(supervisor, cycle, deadline))
      break;
  }
  auto snapshot = SupervisorTestAccess::monitor_snapshot(supervisor);
  return predicate(snapshot) ? std::optional(std::move(snapshot)) : std::nullopt;
}

std::vector<ava::process::SpawnResultV1> spawn_idle_children(ava::process::Supervisor& supervisor, ava::process::OwnerPathV1 const& application,
                                                             std::size_t count)
{
  std::vector<ava::process::SpawnResultV1> children;
  children.reserve(count);
  for (std::size_t index = 0; index < count; ++index)
  {
    auto child = spawn_fixture(supervisor, application, "input-gate");
    if (!child)
      break;
    children.push_back(std::move(*child));
  }
  return children;
}

bool all_delays(ProcessMonitorSnapshot const& snapshot, std::size_t count, std::uint64_t delay)
{
  if (snapshot.records.size() != count)
    return false;
  for (auto const& row : snapshot.records)
  {
    if (row.current_delay_milliseconds != delay)
      return false;
  }
  return true;
}

class HookLatch final
{
 public:
  ~HookLatch() { release(); }

  void arrive_and_wait()
  {
    std::unique_lock lock(mutex_);
    arrived_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }

  bool wait_until(ava::process::ProcessDeadline deadline)
  {
    std::unique_lock lock(mutex_);
    return changed_.wait_until(lock, deadline, [&] { return arrived_; });
  }

  void release() noexcept
  {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool arrived_ = false;
  bool released_ = false;
};

void test_lazy_monitor_resources()
{
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto scope = ava::process::ProcessScopeV1::application(supervisor);
  std::vector<ava::process::Reservation> reservations;
  reservations.reserve(ava::process::kMaxLiveProcessRecordsV1);
  auto application = application_owner();
  for (std::size_t index = 0; index < ava::process::kMaxLiveProcessRecordsV1; ++index)
  {
    auto reservation = supervisor->reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin);
    if (!reservation)
      break;
    reservations.push_back(std::move(*reservation));
  }
  reservations.clear();
  auto before_shutdown = SupervisorTestAccess::monitor_snapshot(*supervisor);
  auto shutdown = supervisor->shutdown(ava::tests::now_plus_seconds(1s));
  auto after_shutdown = SupervisorTestAccess::monitor_snapshot(*supervisor);
  expect(scope && before_shutdown.records.empty() && !before_shutdown.monitor_started && before_shutdown.counters.current_wake_descriptors == 0 &&
             before_shutdown.counters.current_monitor_threads == 0 && shutdown.complete && after_shutdown.counters.current_wake_descriptors == 0 &&
             after_shutdown.counters.current_monitor_threads == 0,
         "Supervisor, process scopes, abandoned capacity, and never-started shutdown allocate no monitor descriptor or thread");
}

bool test_automatic_pidfd_counts()
{
  bool pidfd_available = true;
  for (std::size_t const count : {std::size_t(1), std::size_t(8), std::size_t(64)})
  {
    ava::process::Supervisor supervisor;
    auto application = application_owner();
    auto children = spawn_idle_children(supervisor, application, count);
    if (!ava::tests::require(children.size() == count, "automatic monitor fixture launches " + std::to_string(count) + " gated children"))
      return false;
    auto observed = wait_for_snapshot(
        supervisor,
        [count](ProcessMonitorSnapshot const& snapshot) {
          return snapshot.pidfd_selected ? snapshot.counters.current_watches == count && snapshot.counters.poll_peak_entries >= count + 1
                                         : snapshot.counters.pidfd_unavailable_failures + snapshot.counters.pidfd_denied_failures >= count;
        },
        10s, true);
    auto snapshot = observed ? std::move(*observed) : SupervisorTestAccess::monitor_snapshot(supervisor);
    pidfd_available = pidfd_available && snapshot.pidfd_selected;
    ava::tests::expect_all("automatic monitor publishes the complete poll set for " + std::to_string(count) + " children",
                           {{"settled snapshot", observed.has_value()},
                            {"bounded poll set", snapshot.counters.poll_peak_entries <= 1 + 2 * ava::process::kMaxLiveProcessRecordsV1},
                            {"no pidfd resource failures", snapshot.counters.pidfd_resource_failures == 0},
                            {"no other pidfd failures", snapshot.counters.pidfd_other_failures == 0}});
    if (snapshot.pidfd_selected)
    {
      ava::tests::expect_eq(snapshot.counters.current_watches, count, "pidfd backend owns one watch per gated child");
      ava::tests::expect_eq(snapshot.counters.exact_probes, 0U, "pidfd backend needs no periodic exact probes");
      ava::tests::expect_eq(snapshot.counters.fallback_probes, 0U, "pidfd backend needs no fallback probes");
      ava::tests::expect_all("pidfd backend descriptors retain close-on-exec and nonblocking invariants",
                             {{"close-on-exec", snapshot.counters.cloexec_invariant},
                              {"nonblocking", snapshot.counters.nonblocking_invariant},
                              {"close-on-exec checked", snapshot.counters.cloexec_checks >= count + 1},
                              {"nonblocking checked", snapshot.counters.nonblocking_checks >= count + 1}});
    }
    else
      expect(snapshot.counters.pidfd_unavailable_failures + snapshot.counters.pidfd_denied_failures >= count,
             "automatic backend explicitly observes unavailable or denied pidfds");

    // Resource ownership is checked only after the gated workload has settled.
    for (auto& child : children)
      if (child.standard_input)
        child.standard_input->close();
    auto const cleanup_deadline = now_plus_seconds(20);
    for (auto& child : children)
    {
      auto status = supervisor.wait(child.handle, cleanup_deadline);
      ava::tests::expect_all("released monitor census child settles naturally",
                             {{"status", status.has_value()},
                              {"natural exit", status && status->reason == ava::process::TerminationReasonV1::NaturalExit},
                              {"complete cleanup", status && status->cleanup == ava::process::CleanupStateV1::Complete}});
    }
    auto shutdown = supervisor.shutdown(now_plus_seconds(10));
    auto ended = SupervisorTestAccess::monitor_snapshot(supervisor);
    ava::tests::expect_all("quiescent automatic monitor shutdown releases all shared ownership",
                           {{"shutdown complete", shutdown.complete},
                            {"no live records", supervisor.snapshot().live_records == 0},
                            {"no watches", ended.counters.current_watches == 0},
                            {"no wake descriptors", ended.counters.current_wake_descriptors == 0},
                            {"no monitor threads", ended.counters.current_monitor_threads == 0}});
  }
  return pidfd_available;
}

void test_adaptive_fallback(ProcessMonitorBackendMode mode)
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  bool const selected = SupervisorTestAccess::set_monitor_backend_mode(supervisor, mode);
  auto children = spawn_idle_children(supervisor, application, 2);
  auto reached = wait_for_snapshot(supervisor, [](ProcessMonitorSnapshot const& snapshot) { return all_delays(snapshot, 2, 1000); });
  auto before = SupervisorTestAccess::monitor_snapshot(supervisor);
  auto const cycle = before.counters.monitor_cycles;
  bool const next_cycle = wait_for_cycle(supervisor, cycle, 1500ms);
  auto after = SupervisorTestAccess::monitor_snapshot(supervisor);
  bool buckets = true;
  for (std::size_t index = 0; index < 7; ++index)
    buckets = buckets && after.counters.fallback_delay_buckets[index] >= children.size();
  bool const failure_class = mode == ProcessMonitorBackendMode::ForceFallbackUnavailable ? after.counters.pidfd_unavailable_failures >= children.size()
                                                                                         : after.counters.pidfd_denied_failures >= children.size();
  expect(selected && children.size() == 2 && reached && next_cycle && all_delays(after, 2, 1000) && buckets && failure_class &&
             after.counters.poll_calls - before.counters.poll_calls <= 3,
         "forced unavailable and denied backends use exact 10-to-1000ms adaptive buckets without sustained 100Hz polling");
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(3s)));
}

void test_per_record_waiter_cap_and_wake()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  static_cast<void>(SupervisorTestAccess::set_monitor_backend_mode(supervisor, ProcessMonitorBackendMode::ForceFallbackUnavailable));
  auto children = spawn_idle_children(supervisor, application, 8);
  if (!ava::tests::require(children.size() == 8, "waiter-cap fixture launches all eight gated children"))
    return;
  auto reached = wait_for_snapshot(supervisor, [](ProcessMonitorSnapshot const& snapshot) { return all_delays(snapshot, 8, 1000); });
  auto before = SupervisorTestAccess::monitor_snapshot(supervisor);
  auto const pulse = SupervisorTestAccess::pulse_monitor(supervisor);
  bool const pulse_seen = wait_for_cycle(supervisor, pulse);
  auto latch = std::make_shared<HookLatch>();
  SupervisorTestAccess::set_after_poll_snapshot_hook(supervisor, [&supervisor, latch] {
    auto snapshot = SupervisorTestAccess::monitor_snapshot(supervisor);
    if (!snapshot.records.empty() && snapshot.records.front().active_waiter_count == 1)
      latch->arrive_and_wait();
  });
  std::optional<ava::core::Result<ava::process::ExitStatusV1>> waited;
  std::thread waiter([&] { waited.emplace(supervisor.wait(children.front().handle, now_plus_seconds(20))); });
  bool const frozen = latch->wait_until(now_plus_seconds(10));
  auto active = SupervisorTestAccess::monitor_snapshot(supervisor);
  bool unrelated_stayed_slow = active.records.size() == 8;
  for (std::size_t index = 1; index < active.records.size(); ++index)
    unrelated_stayed_slow = unrelated_stayed_slow && active.records[index].active_waiter_count == 0 && active.records[index].current_delay_milliseconds == 1000;
  ava::tests::expect_all("a registered waiter caps only its own record while the monitor is gated",
                         {{"adaptive backoff reached", reached.has_value()},
                          {"pulse observed", pulse_seen},
                          {"monitor frozen", frozen},
                          {"wake requested", active.counters.wake_attempts > before.counters.wake_attempts},
                          {"waiter registered", !active.records.empty() && active.records.front().active_waiter_count == 1},
                          {"waiter delay capped", !active.records.empty() && active.records.front().current_delay_milliseconds <= 50},
                          {"unrelated records unchanged", unrelated_stayed_slow}});
  if (children.front().standard_input)
    children.front().standard_input->close();
  latch->release();
  SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
  waiter.join();
  ava::tests::expect_all("explicit child release completes the registered waiter",
                         {{"wait returned", waited && waited->has_value()},
                          {"natural exit", waited && *waited && (**waited).reason == ava::process::TerminationReasonV1::NaturalExit}});
  static_cast<void>(supervisor.shutdown(now_plus_seconds(10)));
}

void test_quiet_proofs_and_stop_settlement()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  static_cast<void>(SupervisorTestAccess::set_monitor_backend_mode(supervisor, ProcessMonitorBackendMode::ForceFallbackDenied));

  auto const before_normal = SupervisorTestAccess::monitor_snapshot(supervisor);
  auto normal = spawn_fixture(supervisor, application, "normal");
  auto normal_status = normal ? supervisor.wait(normal->handle, ava::tests::now_plus_seconds(3s))
                              : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(normal.error()));
  auto after_normal = SupervisorTestAccess::monitor_snapshot(supervisor);

  auto leader_first = spawn_fixture(supervisor, application, "leader-exits-first", {.termination_grace = 30ms, .execution_deadline = std::nullopt});
  auto leader_status = leader_first ? supervisor.wait(leader_first->handle, ava::tests::now_plus_seconds(3s))
                                    : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(leader_first.error()));
  auto after_leader = SupervisorTestAccess::monitor_snapshot(supervisor);

  auto refusing = spawn_fixture(supervisor, application, "ignore-term", {.termination_grace = 30ms, .execution_deadline = std::nullopt}, true);
  std::string refusal_output;
  bool const refusing_ready =
      refusing && refusing->standard_output && ava::tests::read_until(*refusing->standard_output, refusal_output, "READY\n", now_plus_seconds(10), 4096);
  auto refusal_stop = refusing ? supervisor.request_stop(refusing->handle, ava::process::TerminationReasonV1::Canceled, ava::tests::now_plus_seconds(2s))
                               : ava::core::Result<ava::process::StopResultV1>(std::unexpected(refusing.error()));
  auto refusal_status = refusing ? supervisor.wait(refusing->handle, ava::tests::now_plus_seconds(2s))
                                 : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(refusing.error()));
  auto after_refusal = SupervisorTestAccess::monitor_snapshot(supervisor);

  auto idle = spawn_fixture(supervisor, application, "input-gate", {.termination_grace = 0ms, .execution_deadline = std::nullopt});
  auto stop = idle ? supervisor.request_stop(idle->handle, ava::process::TerminationReasonV1::Canceled, now_plus_seconds(10))
                   : ava::core::Result<ava::process::StopResultV1>(std::unexpected(idle.error()));
  auto stopped = idle ? supervisor.wait(idle->handle, now_plus_seconds(10)) : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(idle.error()));
  auto final = SupervisorTestAccess::monitor_snapshot(supervisor);
  auto const final_cycle = SupervisorTestAccess::pulse_monitor(supervisor);
  bool const final_cycle_seen = wait_for_cycle(supervisor, final_cycle);
  auto quiescent = SupervisorTestAccess::monitor_snapshot(supervisor);

  ava::tests::expect_all("natural exit, leader-first cleanup, TERM refusal, and explicit KILL settle with quiet-group proof",
                         {{"normal natural exit", normal_status && normal_status->reason == ava::process::TerminationReasonV1::NaturalExit},
                          {"normal quiet proofs", after_normal.counters.quiet_group_proofs - before_normal.counters.quiet_group_proofs == 2},
                          {"leader natural exit", leader_status && leader_status->reason == ava::process::TerminationReasonV1::NaturalExit},
                          {"leader quiet proofs", after_leader.counters.quiet_group_proofs - after_normal.counters.quiet_group_proofs == 2},
                          {"TERM refusal ready", refusing_ready},
                          {"refusal stop accepted", refusal_stop.has_value()},
                          {"refusal killed", refusal_status && refusal_status->signal_number == SIGKILL},
                          {"refusal quiet proofs", after_refusal.counters.quiet_group_proofs - after_leader.counters.quiet_group_proofs == 2},
                          {"idle stop accepted", stop.has_value()},
                          {"idle canceled", stopped && stopped->reason == ava::process::TerminationReasonV1::Canceled},
                          {"idle quiet proofs", final.counters.quiet_group_proofs - after_refusal.counters.quiet_group_proofs == 2},
                          {"quiescent pulse observed", final_cycle_seen},
                          {"no further group observations", quiescent.counters.group_observations == final.counters.group_observations}});
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_snapshot_lifetime_and_descriptor_churn()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto child = spawn_fixture(supervisor, application, "input-gate");
  auto latch = std::make_shared<HookLatch>();
  SupervisorTestAccess::set_after_poll_snapshot_hook(supervisor, [latch] { latch->arrive_and_wait(); });
  bool const frozen = latch->wait_until(ava::tests::now_plus_seconds(1s));

  ava::process::ShutdownResultV1 shutdown;
  std::thread shutdown_thread([&] { shutdown = supervisor.shutdown(ava::tests::now_plus_seconds(300ms)); });
  auto const finalization_deadline = ava::tests::now_plus_seconds(10);
  while (supervisor.snapshot().live_records != 0 && std::chrono::steady_clock::now() < finalization_deadline)
    std::this_thread::yield();
  bool const finalized_before_churn = supervisor.snapshot().live_records == 0;

  for (int index = 0; index < 512; ++index)
  {
    int const descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (descriptor >= 0)
      static_cast<void>(::close(descriptor));
  }
  latch->release();
  shutdown_thread.join();
  auto public_snapshot = supervisor.snapshot();
  auto monitor = SupervisorTestAccess::monitor_snapshot(supervisor);
  bool settled_once = public_snapshot.records.size() == 1 && public_snapshot.records.front().settlement_count == 1;
  ava::tests::expect_all("an in-flight snapshot owns descriptors across finalization/churn and ignores stale readiness",
                         {{"child launched", child.has_value()},
                          {"monitor frozen", frozen},
                          {"finalized before churn", finalized_before_churn},
                          {"one shutdown settlement", shutdown.settled_count == 1},
                          {"settled once", settled_once},
                          {"no invalid descriptor events", monitor.counters.pollnval_events == 0},
                          {"stale event ignored", monitor.counters.stale_events >= 1},
                          {"no watches", monitor.counters.current_watches == 0},
                          {"no wake descriptors", monitor.counters.current_wake_descriptors == 0},
                          {"no monitor threads", monitor.counters.current_monitor_threads == 0}});
}

}  // namespace
#endif

void run_process_monitor_tests()
{
#if defined(_WIN32)
  ava::tests::request_skip("event-driven process monitoring is compile-time unsupported on Windows");
#else
  test_lazy_monitor_resources();
  bool const pidfd_available = test_automatic_pidfd_counts();
  test_adaptive_fallback(ProcessMonitorBackendMode::ForceFallbackUnavailable);
  test_adaptive_fallback(ProcessMonitorBackendMode::ForceFallbackDenied);
  test_per_record_waiter_cap_and_wake();
  test_quiet_proofs_and_stop_settlement();
  test_snapshot_lifetime_and_descriptor_churn();
  if (!pidfd_available)
    ava::tests::request_skip("the host kernel or process sandbox does not provide pidfd_open for the automatic-backend assertion");
#endif
}
