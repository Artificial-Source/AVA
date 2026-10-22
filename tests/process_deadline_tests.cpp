#include "sys.h"
#include "tests/support/process_test_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/process/environment.h"
#include "ava/process/supervisor.h"
#include "ava/process/supervisor_test_support.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <signal.h>
#endif

#ifndef AVA_FAKE_PROCESS_CHILD_PATH
#define AVA_FAKE_PROCESS_CHILD_PATH ""
#endif

#if !defined(_WIN32)
namespace {

using namespace std::chrono_literals;
using ava::process::testing::SupervisorTestAccess;
using Clock = std::chrono::steady_clock;

ava::process::OwnerPathV1 application_owner()
{
  auto owner = ava::process::OwnerPathV1::application();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::process::OwnerPathV1 operation_owner(ava::process::OwnerPathV1 const& prefix)
{
  auto owner = prefix.operation();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::core::Result<ava::process::SpawnResultV1> spawn_fixture(ava::process::Supervisor& supervisor, ava::process::OwnerPathV1 const& owner, std::string mode,
                                                             ava::process::ProcessDeadline execution_deadline,
                                                             std::chrono::milliseconds termination_grace = 100ms, bool capture_input = false,
                                                             bool capture_output = false, std::vector<std::string> extra = {},
                                                             std::function<bool()> cancel_requested = {})
{
  auto reservation =
      supervisor.reserve(owner, ava::process::ProcessRoleV1::Plugin, {.termination_grace = termination_grace, .execution_deadline = execution_deadline});
  if (!reservation)
    return std::unexpected(std::move(reservation.error()));
  auto environment = ava::process::make_plugin_environment_v1("/");
  if (!environment)
    return std::unexpected(std::move(environment.error()));
  std::vector<std::string> argv{AVA_FAKE_PROCESS_CHILD_PATH, std::move(mode)};
  argv.insert(argv.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));
  return supervisor.spawn(std::move(*reservation), {.executable = AVA_FAKE_PROCESS_CHILD_PATH,
                                                    .argv = std::move(argv),
                                                    .environment = std::move(*environment),
                                                    .cwd = "/",
                                                    .stdin_mode = capture_input ? ava::process::StreamModeV1::Capture : ava::process::StreamModeV1::Discard,
                                                    .stdout_mode = capture_output ? ava::process::StreamModeV1::Capture : ava::process::StreamModeV1::Discard,
                                                    .stderr_mode = ava::process::StreamModeV1::Discard,
                                                    .cancel_requested = std::move(cancel_requested)});
}

void wait_until(ava::process::ProcessDeadline deadline)
{
  std::mutex mutex;
  std::condition_variable changed;
  std::unique_lock lock(mutex);
  while (Clock::now() < deadline)
    changed.wait_until(lock, deadline);
}

template <typename Predicate>
std::optional<ava::process::ProcessSnapshotV1> wait_for_snapshot(ava::process::Supervisor& supervisor, Predicate predicate,
                                                                 ava::process::ProcessDeadline deadline)
{
  while (Clock::now() < deadline)
  {
    auto snapshot = supervisor.snapshot();
    if (predicate(snapshot))
      return snapshot;
    auto const cycle = SupervisorTestAccess::monitor_cycle(supervisor);
    static_cast<void>(SupervisorTestAccess::wait_for_monitor_cycle(supervisor, cycle, deadline));
  }
  auto snapshot = supervisor.snapshot();
  return predicate(snapshot) ? std::optional(std::move(snapshot)) : std::nullopt;
}

std::string snapshot_text(ava::process::ProcessSnapshotV1 const& snapshot)
{
  std::ostringstream output;
  output << snapshot.schema_version << ' ' << snapshot.accepting << ' ' << snapshot.monitor_started << ' ' << snapshot.live_records << ' '
         << snapshot.retained_terminal_records;
  for (auto const& record : snapshot.records)
  {
    output << ' ' << record.schema_version << ' ' << record.record_alias << ' ' << record.owner_alias << ' ' << ava::process::to_string(record.role) << ' '
           << ava::process::to_string(record.state) << ' ' << ava::process::to_string(record.cleanup) << ' ' << ava::process::to_string(record.exit_kind) << ' '
           << record.monotonic_milliseconds << ' ' << record.settlement_count;
    if (record.reason)
      output << ' ' << ava::process::to_string(*record.reason);
  }
  return output.str();
}

bool write_byte(ava::process::PipeEndpoint& endpoint, ava::process::ProcessDeadline deadline)
{
  std::byte const value{'P'};
  while (Clock::now() < deadline)
  {
    auto written = endpoint.write(std::span(&value, 1));
    if (!written)
      return false;
    if (written->state == ava::process::PipeIoStateV1::Progress)
      return written->bytes == 1;
    auto ready = endpoint.wait_writable(deadline);
    if (!ready || !*ready)
      return false;
  }
  return false;
}

bool read_to_eof(ava::process::PipeEndpoint& endpoint, ava::process::ProcessDeadline deadline)
{
  std::array<std::byte, 256> buffer{};
  while (Clock::now() < deadline)
  {
    auto read = endpoint.read(buffer);
    if (!read)
      return false;
    if (read->state == ava::process::PipeIoStateV1::EndOfStream)
      return true;
    if (read->state == ava::process::PipeIoStateV1::Progress)
      continue;
    auto ready = endpoint.wait_readable(deadline);
    if (!ready || !*ready)
      return false;
  }
  return false;
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

bool complete_deadline_status(ava::process::ExitStatusV1 const& status)
{
  return status.reason == ava::process::TerminationReasonV1::DeadlineExpired && status.cleanup == ava::process::CleanupStateV1::Complete;
}

std::pair<std::shared_ptr<HookLatch>, bool> freeze_monitor(ava::process::Supervisor& supervisor, ava::process::ProcessDeadline deadline);

void test_reservation_policy_validation()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const past_instant = Clock::now();
  auto past = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin, {.execution_deadline = past_instant});
  auto browser_missing = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::BrowserOpener);
  auto const far_instant = ava::tests::now_plus_seconds(11s);
  auto browser_far = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::BrowserOpener, {.execution_deadline = far_instant});

  bool boundary_valid = false;
  auto const boundary_base = Clock::now();
  {
    auto boundary = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::BrowserOpener, {.execution_deadline = boundary_base + 10s});
    boundary_valid = boundary.has_value();
  }
  bool optional_for_other_roles = false;
  {
    auto optional = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin);
    optional_for_other_roles = optional.has_value();
  }
  bool saturating_horizon_valid = false;
  {
    auto saturated =
        supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin, {.execution_deadline = ava::process::ProcessDeadline::max()});
    saturating_horizon_valid = saturated.has_value();
  }

  std::string errors;
  if (!past)
    errors += past.error().format();
  if (!browser_missing)
    errors += browser_missing.error().format();
  if (!browser_far)
    errors += browser_far.error().format();
  auto const snapshot = supervisor.snapshot();
  auto const past_text = std::to_string(past_instant.time_since_epoch().count());
  auto const far_text = std::to_string(far_instant.time_since_epoch().count());
  expect(!ava::process::LifecyclePolicyV1{}.execution_deadline && !past && !browser_missing && !browser_far && boundary_valid && optional_for_other_roles &&
             saturating_horizon_valid && snapshot.records.empty() && snapshot.live_records == 0 && !snapshot.monitor_started,
         "past deadlines and missing/overlong Browser deadlines fail before records while valid general deadlines include a saturating cleanup horizon");
  expect(errors.find(past_text) == std::string::npos && errors.find(far_text) == std::string::npos,
         "execution-deadline policy errors do not disclose absolute clock values");
}

void test_reserved_deadline_expires_before_spawn()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const root = create_empty_root("process-deadline-reserved");
  auto const marker = root / "EXECUTION_CONTENT_CANARY";
  auto const deadline = ava::tests::now_plus_seconds(80ms);
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin, {.execution_deadline = deadline});
  wait_until(deadline + 5ms);
  auto environment = ava::process::make_plugin_environment_v1("/");
  auto launched =
      reservation && environment
          ? supervisor.spawn(std::move(*reservation), {.executable = AVA_FAKE_PROCESS_CHILD_PATH,
                                                       .argv = {AVA_FAKE_PROCESS_CHILD_PATH, "exec-marker", marker.string()},
                                                       .environment = std::move(*environment),
                                                       .cwd = "/",
                                                       .stdin_mode = ava::process::StreamModeV1::Discard,
                                                       .stdout_mode = ava::process::StreamModeV1::Discard,
                                                       .stderr_mode = ava::process::StreamModeV1::Discard})
          : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "deadline fixture setup failed")));
  auto const snapshot = supervisor.snapshot();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().state == ava::process::ProcessStateV1::Finished &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                     snapshot.records.front().cleanup == ava::process::CleanupStateV1::NotRequired && snapshot.records.front().settlement_count == 1;
  expect(reservation && environment && !launched && exact && snapshot.live_records == 0 && !snapshot.monitor_started && !std::filesystem::exists(marker),
         "a reservation that expires before spawn settles once as content-free DeadlineExpired without a fork or monitor");
}

void test_due_deadline_precedes_unregistered_spawn_cancellation()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const root = create_empty_root("process-deadline-unregistered-cancel");
  auto const marker = root / "executed";
  auto const deadline = ava::tests::now_plus_seconds(100ms);
  auto launched = spawn_fixture(supervisor, operation_owner(application), "exec-marker", deadline, 100ms, false, false, {marker.string()}, [deadline] {
    wait_until(deadline);
    return true;
  });
  auto const snapshot = supervisor.snapshot();
  auto const error = !launched ? launched.error().format() : std::string();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().state == ava::process::ProcessStateV1::Finished &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                     snapshot.records.front().cleanup == ava::process::CleanupStateV1::NotRequired && snapshot.records.front().settlement_count == 1;
  expect(!launched && exact && snapshot.live_records == 0 && !snapshot.monitor_started && !std::filesystem::exists(marker) &&
             snapshot_text(snapshot).find("deadline_expired") != std::string::npos && error.find("deadline_expired") != std::string::npos,
         "an execution deadline made due inside the first common-spawn cancellation callback settles and reports unregistered as DeadlineExpired once");
}

void test_pre_deadline_unregistered_spawn_cancellation()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const root = create_empty_root("process-deadline-unregistered-pre-deadline-cancel");
  auto const marker = root / "executed";
  auto launched = spawn_fixture(supervisor, operation_owner(application), "exec-marker", ava::tests::now_plus_seconds(5s), 100ms, false, false,
                                {marker.string()}, [] { return true; });
  auto const snapshot = supervisor.snapshot();
  auto const error = !launched ? launched.error().format() : std::string();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().state == ava::process::ProcessStateV1::Finished &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::Canceled &&
                     snapshot.records.front().cleanup == ava::process::CleanupStateV1::NotRequired && snapshot.records.front().settlement_count == 1;
  expect(!launched && exact && snapshot.live_records == 0 && !snapshot.monitor_started && !std::filesystem::exists(marker) &&
             snapshot_text(snapshot).find("canceled") != std::string::npos && error.find("reason: canceled") != std::string::npos,
         "a common-spawn cancellation before its execution deadline settles and reports unregistered as Canceled once");
}

void test_due_deadline_precedes_registered_gate_cancellation()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const root = create_empty_root("process-deadline-registered-cancel");
  auto const marker = root / "executed";
  auto gate_latch = std::make_shared<HookLatch>();
  auto monitor_latch = std::make_shared<HookLatch>();
  SupervisorTestAccess::set_after_fork_before_release_hook(supervisor, [gate_latch] { gate_latch->arrive_and_wait(); });
  SupervisorTestAccess::set_after_poll_snapshot_hook(supervisor, [monitor_latch] { monitor_latch->arrive_and_wait(); });

  std::atomic_bool canceled = false;
  auto const deadline = ava::tests::now_plus_seconds(1s);
  std::optional<ava::core::Result<ava::process::SpawnResultV1>> launch;
  std::thread launcher([&] {
    launch.emplace(spawn_fixture(supervisor, operation_owner(application), "exec-marker", deadline, 100ms, false, false, {marker.string()},
                                 [&] { return canceled.load(std::memory_order_relaxed); }));
  });
  bool const gate_reached = gate_latch->wait_until(deadline);
  bool const monitor_frozen = monitor_latch->wait_until(deadline);
  if (gate_reached && monitor_frozen)
  {
    wait_until(deadline);
    canceled.store(true, std::memory_order_relaxed);
  }
  gate_latch->release();

  bool deadline_visible = false;
  auto const observation_deadline = ava::tests::now_plus_seconds(500ms);
  while (gate_reached && monitor_frozen && Clock::now() < observation_deadline)
  {
    auto const snapshot = supervisor.snapshot();
    deadline_visible = snapshot.records.size() == 1 && snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired;
    if (deadline_visible)
      break;
    std::this_thread::yield();
  }

  monitor_latch->release();
  launcher.join();
  SupervisorTestAccess::clear_after_fork_before_release_hook(supervisor);
  SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
  auto const shutdown = supervisor.shutdown(ava::tests::now_plus_seconds(2s));
  auto const snapshot = supervisor.snapshot();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().state == ava::process::ProcessStateV1::Finished &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                     snapshot.records.front().cleanup == ava::process::CleanupStateV1::Complete && snapshot.records.front().settlement_count == 1;
  bool const deadline_error = launch && !launch->has_value() && launch->error().format().find("deadline_expired") != std::string::npos;
  expect(gate_reached && monitor_frozen && deadline_visible && deadline_error && shutdown.complete && exact && snapshot.live_records == 0 &&
             SupervisorTestAccess::retained_native_child_count(supervisor) == 0 && !std::filesystem::exists(marker),
         "a due execution deadline wins registered common-spawn cancellation while a frozen monitor cannot race, keeps the gate closed, and cleans once");
}

void test_after_fork_before_release_deadline()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const root = create_empty_root("process-deadline-gated");
  auto const marker = root / "executed";
  auto latch = std::make_shared<HookLatch>();
  SupervisorTestAccess::set_after_fork_before_release_hook(supervisor, [latch] { latch->arrive_and_wait(); });
  auto const deadline = ava::tests::now_plus_seconds(500ms);
  std::optional<ava::core::Result<ava::process::SpawnResultV1>> launch;
  std::thread launcher(
      [&] { launch.emplace(spawn_fixture(supervisor, operation_owner(application), "exec-marker", deadline, 5s, false, false, {marker.string()})); });
  bool const reached = latch->wait_until(deadline);
  auto expired = reached ? wait_for_snapshot(
                               supervisor,
                               [](ava::process::ProcessSnapshotV1 const& snapshot) {
                                 return snapshot.records.size() == 1 && snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                                        snapshot.records.front().state == ava::process::ProcessStateV1::Finished;
                               },
                               deadline + 1s)
                         : std::nullopt;
  latch->release();
  launcher.join();
  SupervisorTestAccess::clear_after_fork_before_release_hook(supervisor);
  auto const snapshot = supervisor.snapshot();
  bool const once = snapshot.records.size() == 1 && snapshot.records.front().settlement_count == 1;
  expect(reached && expired && launch && !*launch && once && !std::filesystem::exists(marker),
         "a deadline after exact-child registration but before release keeps the gate closed and performs one exact DeadlineExpired settlement");
}

void test_running_deadlines_reap_within_cleanup_horizon()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const deadline = ava::tests::now_plus_seconds(800ms);
  auto cooperative = spawn_fixture(supervisor, operation_owner(application), "input-gate", deadline, 75ms, true, true);
  auto refusing = spawn_fixture(supervisor, operation_owner(application), "ignore-term", deadline, 75ms, false, true);
  std::string ready_text;
  bool const ready = refusing && refusing->standard_output && ava::tests::read_until(*refusing->standard_output, ready_text, "READY\n", deadline, 4096);
  auto cooperative_status =
      cooperative ? supervisor.wait(cooperative->handle, deadline + 1s) : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(cooperative.error()));
  auto refusing_status =
      refusing ? supervisor.wait(refusing->handle, deadline + 1s) : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(refusing.error()));
  bool const cooperative_eof = cooperative && cooperative->standard_output && read_to_eof(*cooperative->standard_output, deadline + 1s);
  bool const refusing_eof = refusing && refusing->standard_output && read_to_eof(*refusing->standard_output, deadline + 1s);
  auto const snapshot = supervisor.snapshot();
  bool settled_once = snapshot.records.size() == 2;
  for (auto const& record : snapshot.records)
    settled_once = settled_once && record.state == ava::process::ProcessStateV1::Finished && record.cleanup == ava::process::CleanupStateV1::Complete &&
                   record.settlement_count == 1;
  bool const cooperative_signal =
      cooperative_status && cooperative_status->kind == ava::process::ExitKindV1::Signaled && cooperative_status->signal_number == SIGTERM;
  bool const refusing_signal = refusing_status && refusing_status->kind == ava::process::ExitKindV1::Signaled && refusing_status->signal_number == SIGKILL;
  expect(cooperative && refusing && ready && cooperative_status && refusing_status && complete_deadline_status(*cooperative_status) &&
             complete_deadline_status(*refusing_status) && cooperative_signal && refusing_signal && cooperative_eof && refusing_eof && settled_once &&
             SupervisorTestAccess::retained_native_child_count(supervisor) == 0,
         "cooperative and TERM-refusing deadline children are proven quiet, reaped, and settled Complete once");
}

void test_natural_exit_before_trigger_is_reaped_after_trigger()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const deadline = ava::tests::now_plus_seconds(3s);
  auto child = spawn_fixture(supervisor, operation_owner(application), "input-gate", deadline, 100ms, true, true);
  auto [latch, frozen] = freeze_monitor(supervisor, deadline - 1500ms);
  bool const released = child && child->standard_input && write_byte(*child->standard_input, deadline);
  bool const eof_before_trigger = child && child->standard_output && read_to_eof(*child->standard_output, deadline);
  wait_until(deadline + 5ms);
  latch->release();
  SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
  auto status = child ? supervisor.wait(child->handle, deadline + 1s) : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
  auto const snapshot = supervisor.snapshot();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().state == ava::process::ProcessStateV1::Finished &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::NaturalExit &&
                     snapshot.records.front().cleanup == ava::process::CleanupStateV1::Complete && snapshot.records.front().settlement_count == 1;
  ava::tests::expect_all("a child exiting before the execution trigger keeps NaturalExit when the frozen monitor resumes after the trigger",
                         {{"child launched", child.has_value()},
                          {"monitor frozen with exit margin", frozen},
                          {"child released", released},
                          {"EOF before trigger", eof_before_trigger},
                          {"status available", status.has_value()},
                          {"natural reason", status && status->reason == ava::process::TerminationReasonV1::NaturalExit},
                          {"cleanup complete", status && status->cleanup == ava::process::CleanupStateV1::Complete},
                          {"exact snapshot", exact},
                          {"no retained native children", SupervisorTestAccess::retained_native_child_count(supervisor) == 0}});
}

void test_cancellation_before_trigger_keeps_its_cleanup_deadline()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const deadline = ava::tests::now_plus_seconds(3s);
  auto child = spawn_fixture(supervisor, operation_owner(application), "ignore-term", deadline, 75ms, false, true);
  std::string ready_text;
  bool const ready = child && child->standard_output && ava::tests::read_until(*child->standard_output, ready_text, "READY\n", deadline, 4096);
  auto [latch, frozen] = freeze_monitor(supervisor, deadline - 1500ms);
  auto stopped = child ? supervisor.request_stop(child->handle, ava::process::TerminationReasonV1::Canceled, deadline + 1s)
                       : ava::core::Result<ava::process::StopResultV1>(std::unexpected(child.error()));
  wait_until(deadline + 5ms);
  latch->release();
  SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
  auto status = child ? supervisor.wait(child->handle, deadline + 1s) : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
  bool const eof = child && child->standard_output && read_to_eof(*child->standard_output, deadline + 1s);
  auto const snapshot = supervisor.snapshot();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().state == ava::process::ProcessStateV1::Finished &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::Canceled &&
                     snapshot.records.front().cleanup == ava::process::CleanupStateV1::Complete && snapshot.records.front().settlement_count == 1;
  ava::tests::expect_all("cancellation committed before the execution trigger keeps its earlier cleanup deadline after the monitor resumes",
                         {{"child ready", ready},
                          {"monitor frozen with cancellation margin", frozen},
                          {"cancellation committed", stopped && stopped->newly_requested == 1},
                          {"status available", status.has_value()},
                          {"canceled first reason", status && status->reason == ava::process::TerminationReasonV1::Canceled},
                          {"cleanup complete", status && status->cleanup == ava::process::CleanupStateV1::Complete},
                          {"TERM refusal escalated", status && status->kind == ava::process::ExitKindV1::Signaled && status->signal_number == SIGKILL},
                          {"EOF observed", eof},
                          {"exact snapshot", exact},
                          {"no retained native children", SupervisorTestAccess::retained_native_child_count(supervisor) == 0}});
}

void test_cleanup_horizon_exhaustion_is_incomplete()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const deadline = ava::tests::now_plus_seconds(350ms);
  auto child = spawn_fixture(supervisor, operation_owner(application), "ignore-term", deadline, 5s, false, true);
  std::string ready_text;
  bool const ready = child && child->standard_output && ava::tests::read_until(*child->standard_output, ready_text, "READY\n", deadline, 4096);
  auto status =
      child ? supervisor.wait(child->handle, ava::tests::now_plus_seconds(10)) : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
  auto const completed = Clock::now();
  auto const snapshot = supervisor.snapshot();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().state == ava::process::ProcessStateV1::Finished &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                     snapshot.records.front().cleanup == ava::process::CleanupStateV1::Incomplete && snapshot.records.front().settlement_count == 1;
  expect(child && ready && status && status->reason == ava::process::TerminationReasonV1::DeadlineExpired &&
             status->cleanup == ava::process::CleanupStateV1::Incomplete && exact && completed >= deadline + 1800ms,
         "exhausting the reservation-time cleanup horizon reports one honest Incomplete settlement rather than claiming cleanup succeeded");
}

void test_expiry_during_launch_framing()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto latch = std::make_shared<HookLatch>();
  SupervisorTestAccess::set_after_gate_release_hook(supervisor, [latch] { latch->arrive_and_wait(); });
  auto const deadline = ava::tests::now_plus_seconds(500ms);
  std::optional<ava::core::Result<ava::process::SpawnResultV1>> launch;
  std::thread launcher([&] { launch.emplace(spawn_fixture(supervisor, operation_owner(application), "input-gate", deadline, 5s, true)); });
  bool const reached = latch->wait_until(deadline);
  auto expired = reached ? wait_for_snapshot(
                               supervisor,
                               [](ava::process::ProcessSnapshotV1 const& snapshot) {
                                 return snapshot.records.size() == 1 && snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired;
                               },
                               deadline + 1s)
                         : std::nullopt;
  latch->release();
  launcher.join();
  SupervisorTestAccess::clear_after_gate_release_hook(supervisor);
  std::string error = launch && !*launch ? launch->error().format() : std::string();
  expect(reached && expired && launch && !*launch && error.find("stopped before exec confirmation") != std::string::npos &&
             error.find("deadline_expired") != std::string::npos,
         "expiry after gate release but during parent launch framing prevents a successful launch return and reports stopped-before-confirmation");
}

std::pair<std::shared_ptr<HookLatch>, bool> freeze_monitor(ava::process::Supervisor& supervisor, ava::process::ProcessDeadline deadline)
{
  auto latch = std::make_shared<HookLatch>();
  SupervisorTestAccess::set_after_poll_snapshot_hook(supervisor, [latch] { latch->arrive_and_wait(); });
  static_cast<void>(SupervisorTestAccess::pulse_monitor(supervisor));
  return {latch, latch->wait_until(deadline)};
}

void test_first_reason_and_due_mutator_ordering()
{
  auto application = application_owner();
  {
    ava::process::Supervisor supervisor;
    auto const deadline = ava::tests::now_plus_seconds(2s);
    auto child = spawn_fixture(supervisor, operation_owner(application), "ignore-term", deadline, 50ms, false, true);
    std::string ready_text;
    bool const ready =
        child && child->standard_output && ava::tests::read_until(*child->standard_output, ready_text, "READY\n", ava::tests::now_plus_seconds(1), 4096);
    auto stopped = child ? supervisor.request_stop(child->handle, ava::process::TerminationReasonV1::Canceled, ava::tests::now_plus_seconds(500ms))
                         : ava::core::Result<ava::process::StopResultV1>(std::unexpected(child.error()));
    auto status = child ? supervisor.wait(child->handle, ava::tests::now_plus_seconds(1s))
                        : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
    expect(ready && stopped && status && status->reason == ava::process::TerminationReasonV1::Canceled,
           "cancellation committed before an execution deadline remains the immutable first reason");
  }

  {
    ava::process::Supervisor supervisor;
    auto session = application.session();
    auto const deadline = ava::tests::now_plus_seconds(2s);
    auto child = session
                     ? spawn_fixture(supervisor, operation_owner(*session), "ignore-term", deadline, 50ms)
                     : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "owner fixture failed")));
    auto stopped = session
                       ? supervisor.request_stop(*session, ava::process::TerminationReasonV1::OwnerShutdown, ava::tests::now_plus_seconds(500ms))
                       : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "owner fixture failed")));
    auto status = child ? supervisor.wait(child->handle, ava::tests::now_plus_seconds(1s))
                        : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
    expect(session && child && stopped && status && status->reason == ava::process::TerminationReasonV1::OwnerShutdown,
           "owner shutdown committed before an execution deadline remains the immutable first reason");
  }

  {
    ava::process::Supervisor supervisor;
    auto const deadline = ava::tests::now_plus_seconds(650ms);
    auto child = spawn_fixture(supervisor, operation_owner(application), "ignore-term", deadline, 50ms);
    auto [latch, frozen] = freeze_monitor(supervisor, deadline);
    if (frozen)
      wait_until(deadline + 5ms);
    auto stopped = child ? supervisor.request_stop(child->handle, ava::process::TerminationReasonV1::Canceled, ava::tests::now_plus_seconds(500ms))
                         : ava::core::Result<ava::process::StopResultV1>(std::unexpected(child.error()));
    auto before_release = supervisor.snapshot();
    latch->release();
    SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
    auto status = child ? supervisor.wait(child->handle, ava::tests::now_plus_seconds(1s))
                        : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
    bool const due_visible = before_release.records.size() == 1 && before_release.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired;
    expect(child && frozen && stopped && stopped->newly_requested == 1 && due_visible && status && complete_deadline_status(*status),
           "a handle stop mutator commits an already-due execution deadline before its requested reason even while the monitor is delayed");
  }

  {
    ava::process::Supervisor supervisor;
    auto session = application.session();
    auto const deadline = ava::tests::now_plus_seconds(650ms);
    auto child = session
                     ? spawn_fixture(supervisor, operation_owner(*session), "ignore-term", deadline, 50ms)
                     : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "owner fixture failed")));
    auto [latch, frozen] = freeze_monitor(supervisor, deadline);
    if (frozen)
      wait_until(deadline + 5ms);
    auto stopped = session
                       ? supervisor.request_stop(*session, ava::process::TerminationReasonV1::OwnerShutdown, ava::tests::now_plus_seconds(500ms))
                       : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "owner fixture failed")));
    auto before_release = supervisor.snapshot();
    latch->release();
    SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
    auto status = child ? supervisor.wait(child->handle, ava::tests::now_plus_seconds(1s))
                        : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
    bool const due_visible = before_release.records.size() == 1 && before_release.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired;
    expect(session && child && frozen && stopped && due_visible && status && complete_deadline_status(*status),
           "an owner stop mutator gives an already-due execution deadline the same first-reason ordering");
  }

  {
    ava::process::Supervisor supervisor;
    auto const deadline = ava::tests::now_plus_seconds(650ms);
    auto child = spawn_fixture(supervisor, operation_owner(application), "ignore-term", deadline, 5s);
    auto [latch, frozen] = freeze_monitor(supervisor, deadline);
    if (frozen)
      wait_until(deadline + 5ms);
    std::optional<ava::process::ShutdownResultV1> shutdown;
    std::thread shutdown_thread([&] { shutdown = supervisor.shutdown(ava::tests::now_plus_seconds(750ms)); });
    bool due_visible = false;
    auto const observation_deadline = ava::tests::now_plus_seconds(300ms);
    while (Clock::now() < observation_deadline)
    {
      auto snapshot = supervisor.snapshot();
      due_visible = snapshot.records.size() == 1 && snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired;
      if (due_visible)
        break;
      std::this_thread::yield();
    }
    latch->release();
    shutdown_thread.join();
    SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
    auto snapshot = supervisor.snapshot();
    bool const exact = snapshot.records.size() == 1 && snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                       snapshot.records.front().settlement_count == 1;
    expect(child && frozen && due_visible && shutdown && exact,
           "shutdown commits an already-due execution deadline before ApplicationShutdown while the monitor is delayed");
  }
}

void test_progress_and_eintr_allow_deadline_cleanup()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const deadline = ava::tests::now_plus_seconds(850ms);
  auto child = spawn_fixture(supervisor, operation_owner(application), "progress-gate", deadline, 5s, true, true);
  std::string ready_text;
  bool const ready = child && child->standard_output && ava::tests::read_until(*child->standard_output, ready_text, "READY\n", deadline, 4096);

  auto [latch, frozen] = freeze_monitor(supervisor, deadline);
  if (frozen)
    SupervisorTestAccess::interrupt_next_monitor_poll(supervisor);
  latch->release();
  SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);

  bool progressed = ready && child->standard_input && child->standard_output;
  auto watch = progressed
                   ? child->standard_output->watch(ava::process::PipeInterestV1::Readable, 17)
                   : ava::core::Result<ava::process::PipeWatchV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "progress fixture failed")));
  for (auto const instant : {deadline - 260ms, deadline - 80ms})
  {
    wait_until(instant);
    progressed = progressed && write_byte(*child->standard_input, deadline);
    if (progressed && watch)
    {
      std::array<ava::process::PipeWatchV1, 1> watches{*watch};
      auto activity = supervisor.wait_for_activity(child->handle, watches, deadline);
      std::string progress_text;
      progressed = activity && !activity->deadline_expired && !activity->ready.empty() &&
                   ava::tests::read_until(*child->standard_output, progress_text, "PROGRESS\n", deadline, 4096);
      if (progressed)
        progressed = supervisor.account_output(child->handle, ava::process::StreamKindV1::StandardOutput, 9, false).has_value();
    }
  }
  auto status =
      child ? supervisor.wait(child->handle, ava::tests::now_plus_seconds(10)) : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
  auto const monitor = SupervisorTestAccess::monitor_snapshot(supervisor);
  expect(child && ready && frozen && progressed && status && complete_deadline_status(*status) && monitor.counters.poll_interruptions >= 1,
         "output progress, readiness waits, accounting, and an interrupted monitor poll still permit complete deadline cleanup");
}

void test_dropped_handle_deadline_settles_snapshot()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  auto const deadline = ava::tests::now_plus_seconds(500ms);
  bool launched = false;
  {
    auto child = spawn_fixture(supervisor, operation_owner(application), "ignore-term", deadline, 50ms);
    launched = child.has_value();
  }
  auto settled = wait_for_snapshot(
      supervisor,
      [](ava::process::ProcessSnapshotV1 const& snapshot) {
        return snapshot.records.size() == 1 && snapshot.records.front().state == ava::process::ProcessStateV1::Finished;
      },
      deadline + 1s);
  bool const exact = settled && settled->records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                     settled->records.front().cleanup == ava::process::CleanupStateV1::Complete && settled->records.front().settlement_count == 1;
  expect(launched && exact && SupervisorTestAccess::retained_native_child_count(supervisor) == 0,
         "dropping the only caller handle leaves complete deadline cleanup and one terminal snapshot settlement owned by the supervisor");
}

void test_nearest_deadline_order_and_shared_shutdown()
{
  auto application = application_owner();
  {
    ava::process::Supervisor supervisor;
    auto const base = Clock::now();
    auto late = spawn_fixture(supervisor, operation_owner(application), "ignore-term", base + 1s, 50ms);
    auto early = spawn_fixture(supervisor, operation_owner(application), "ignore-term", base + 550ms, 50ms);
    auto first = wait_for_snapshot(
        supervisor,
        [](ava::process::ProcessSnapshotV1 const& snapshot) {
          return snapshot.records.size() == 2 && snapshot.records[1].reason == ava::process::TerminationReasonV1::DeadlineExpired;
        },
        base + 900ms);
    bool nearest_first = first && !first->records[0].reason && first->records[0].state != ava::process::ProcessStateV1::Finished;
    auto both = wait_for_snapshot(
        supervisor,
        [](ava::process::ProcessSnapshotV1 const& snapshot) {
          return snapshot.records.size() == 2 && snapshot.records[0].state == ava::process::ProcessStateV1::Finished &&
                 snapshot.records[1].state == ava::process::ProcessStateV1::Finished;
        },
        base + 1500ms);
    bool exact = both && both->records[0].reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                 both->records[1].reason == ava::process::TerminationReasonV1::DeadlineExpired &&
                 both->records[1].monotonic_milliseconds <= both->records[0].monotonic_milliseconds;
    expect(late && early && nearest_first && exact,
           "the event-driven monitor fires the nearest immutable deadline first even when that record registered second");
  }

  {
    ava::process::Supervisor supervisor;
    constexpr std::size_t child_count = 16;
    std::vector<ava::process::SpawnResultV1> children;
    children.reserve(child_count);
    for (std::size_t index = 0; index < child_count; ++index)
    {
      auto child = spawn_fixture(supervisor, operation_owner(application), "ignore-term", ava::tests::now_plus_seconds(30), 5s, false, true);
      if (!ava::tests::require(child && child->standard_output, "shared-budget child launches with stdout: index=" + std::to_string(index)))
        return;
      std::string ready_text;
      if (!ava::tests::require(ava::tests::read_until(*child->standard_output, ready_text, "READY\n", ava::tests::now_plus_seconds(10), 4096),
                               "shared-budget child acknowledges TERM refusal: index=" + std::to_string(index)))
        return;
      children.push_back(std::move(*child));
    }
    auto const started = Clock::now();
    auto shutdown = supervisor.shutdown(started + 350ms);
    auto const elapsed = Clock::now() - started;
    auto const elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    auto const snapshot = supervisor.snapshot();
    bool reasons = snapshot.records.size() == child_count;
    for (auto const& record : snapshot.records)
      reasons = reasons && record.reason == ava::process::TerminationReasonV1::ApplicationShutdown && record.settlement_count == 1;
    ava::tests::expect_all("shutdown shares one 350ms budget instead of sixteen serial budgets; elapsed_ms=" + std::to_string(elapsed_ms),
                           {{"sixteen ready children", children.size() == child_count},
                            {"sixteen settlements", shutdown.settled_count == child_count},
                            {"exact first reasons", reasons},
                            {"elapsed below half the serial budgets (2500ms)", elapsed < 2500ms}});
  }
}

void test_browser_deadline_content_free_terminal()
{
  ava::process::Supervisor supervisor;
  auto application = application_owner();
  constexpr std::string_view canary = "BROWSER_DEADLINE_CONTENT_CANARY_61df";
  auto const deadline = ava::tests::now_plus_seconds(90ms);
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::BrowserOpener, {.execution_deadline = deadline});
  wait_until(deadline + 5ms);
  auto launched =
      reservation ? supervisor.spawn(std::move(*reservation), {.executable = std::string(canary), .argv = {std::string(canary)}, .environment = {}, .cwd = "/"})
                  : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(reservation.error()));
  auto const snapshot = supervisor.snapshot();
  auto const serialized = snapshot_text(snapshot);
  auto const raw_deadline = std::to_string(deadline.time_since_epoch().count());
  auto const error = !launched ? launched.error().format() : std::string();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().role == ava::process::ProcessRoleV1::BrowserOpener &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired && snapshot.records.front().settlement_count == 1;
  expect(reservation && !launched && exact && !snapshot.monitor_started && error.find(canary) == std::string::npos &&
             error.find(raw_deadline) == std::string::npos && serialized.find(canary) == std::string::npos &&
             serialized.find(raw_deadline) == std::string::npos,
         "Browser deadline failures and terminal snapshots retain only closed content-free fields");
}

}  // namespace
#endif

void run_process_deadline_tests()
{
#if defined(_WIN32)
  ava::tests::request_skip("absolute process execution deadlines require the POSIX lifecycle backend");
#else
  test_reservation_policy_validation();
  test_reserved_deadline_expires_before_spawn();
  test_due_deadline_precedes_unregistered_spawn_cancellation();
  test_pre_deadline_unregistered_spawn_cancellation();
  test_due_deadline_precedes_registered_gate_cancellation();
  test_after_fork_before_release_deadline();
  test_running_deadlines_reap_within_cleanup_horizon();
  test_natural_exit_before_trigger_is_reaped_after_trigger();
  test_cancellation_before_trigger_keeps_its_cleanup_deadline();
  test_cleanup_horizon_exhaustion_is_incomplete();
  test_expiry_during_launch_framing();
  test_first_reason_and_due_mutator_ordering();
  test_progress_and_eintr_allow_deadline_cleanup();
  test_dropped_handle_deadline_settles_snapshot();
  test_nearest_deadline_order_and_shared_shutdown();
  test_browser_deadline_content_free_terminal();
#endif
}
