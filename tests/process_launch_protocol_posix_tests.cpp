#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/process/environment.h"
#include "ava/process/launch_protocol_posix.h"
#include "ava/process/supervisor.h"
#include "ava/process/supervisor_test_support.h"
#include "ava/core/AnchorSet.h"
#if defined(__linux__)
#include "tests/support/syscall_errno_injection.h"
#endif

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef AVA_FAKE_PROCESS_CHILD_PATH
#define AVA_FAKE_PROCESS_CHILD_PATH ""
#endif

namespace {

using namespace std::chrono_literals;

#if !defined(_WIN32)

class TestPipe final
{
 public:
  TestPipe()
  {
    std::array<int, 2> descriptors{-1, -1};
#if defined(__linux__)
    valid_ = ::pipe2(descriptors.data(), O_CLOEXEC) == 0;
#else
    valid_ = ::pipe(descriptors.data()) == 0;
#endif
    if (valid_)
    {
      read_ = descriptors[0];
      write_ = descriptors[1];
    }
  }

  TestPipe(TestPipe const&) = delete;
  TestPipe& operator=(TestPipe const&) = delete;
  ~TestPipe()
  {
    close_read();
    close_write();
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] int read_descriptor() const noexcept { return read_; }
  [[nodiscard]] int write_descriptor() const noexcept { return write_; }
  void close_read() noexcept
  {
    if (read_ >= 0)
      static_cast<void>(::close(read_));
    read_ = -1;
  }
  void close_write() noexcept
  {
    if (write_ >= 0)
      static_cast<void>(::close(write_));
    write_ = -1;
  }

 private:
  int read_ = -1;
  int write_ = -1;
  bool valid_ = false;
};

class CallbackLatch final
{
 public:
  ~CallbackLatch() { release(); }

  void arrive_and_wait()
  {
    std::unique_lock lock(mutex_);
    arrived_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }

  bool wait_until(std::chrono::steady_clock::time_point deadline)
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

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool arrived_ = false;
  bool released_ = false;
};

void wait_until(std::chrono::steady_clock::time_point deadline)
{
  std::mutex mutex;
  std::condition_variable changed;
  std::unique_lock lock(mutex);
  while (std::chrono::steady_clock::now() < deadline)
    changed.wait_until(lock, deadline);
}

bool write_all(int descriptor, void const* data, std::size_t size) noexcept
{
  auto const* bytes = static_cast<unsigned char const*>(data);
  std::size_t offset = 0;
  while (offset < size)
  {
    auto const written = ::write(descriptor, bytes + offset, size - offset);
    if (written > 0)
    {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

ava::process::detail::LaunchProtocolOutcomeV1 finish_exec_sequence(TestPipe& channel)
{
  channel.close_write();
  return ava::process::detail::await_launch_exec_confirmation(channel.read_descriptor(), std::chrono::steady_clock::now() + 1s, false);
}

void test_fixed_protocol_success_and_failures()
{
  using namespace ava::process::detail;
  static_assert(std::is_trivially_copyable_v<LaunchFrameV1>);
  static_assert(sizeof(LaunchFrameV1) == 16);
  static_assert(sizeof(LaunchFrameV1) < 512);

  TestPipe success;
  bool const attempt_written = success.valid() && child_write_exec_attempt(success.write_descriptor());
  auto const confirmed = finish_exec_sequence(success);

  TestPipe exec_failure;
  bool const failure_written =
      exec_failure.valid() && child_write_exec_attempt(exec_failure.write_descriptor()) && child_write_exec_failed(exec_failure.write_descriptor(), ENOEXEC);
  auto const failed = finish_exec_sequence(exec_failure);

  TestPipe setup_failure;
  bool const setup_written =
      setup_failure.valid() && child_write_launch_failed(setup_failure.write_descriptor(), LaunchFailureStageV1::WorkingDirectory, EACCES);
  auto const setup = finish_exec_sequence(setup_failure);

  TestPipe no_attempt;
  no_attempt.close_write();
  auto const eof = await_launch_exec_confirmation(no_attempt.read_descriptor(), std::chrono::steady_clock::now() + 1s, false);

  expect(attempt_written && confirmed.disposition == LaunchProtocolDispositionV1::ExecConfirmed,
         "the fixed launch protocol confirms only an explicit exec attempt followed by CLOEXEC EOF");
  expect(failure_written && failed.disposition == LaunchProtocolDispositionV1::ExecFailed && failed.error_number == ENOEXEC,
         "ExecAttempt followed by ExecFailed retains the typed exec-syscall classification");
  expect(setup_written && setup.disposition == LaunchProtocolDispositionV1::LaunchFailed && setup.stage == LaunchFailureStageV1::WorkingDirectory,
         "a typed pre-attempt setup frame remains a launch failure");
  expect(eof.disposition == LaunchProtocolDispositionV1::LaunchFailed && eof.problem == LaunchProtocolProblemV1::EndBeforeAttempt,
         "EOF before an explicit exec attempt is never launch success");
}

void test_absolute_deadline_timeout()
{
  using namespace ava::process::detail;

  TestPipe channel;
  auto const timed_out = await_launch_exec_confirmation(channel.read_descriptor(), std::chrono::steady_clock::now(), false);
  expect(channel.valid() && timed_out.disposition == LaunchProtocolDispositionV1::LaunchFailed && timed_out.problem == LaunchProtocolProblemV1::TimedOut,
         "the launch protocol classifies an exhausted absolute deadline as a timeout");
}

void test_early_poll_timeout_preserves_launch_deadline()
{
#if defined(__linux__) && defined(SYS_poll)
  auto const status = ava::tests::run_in_child_with_syscall_errno(SYS_poll, std::nullopt, 0, [] {
    TestPipe channel;
    if (!channel.valid())
      return 1;
    auto const deadline = std::chrono::steady_clock::now() + 15ms;
    auto const result = ava::process::detail::await_launch_exec_confirmation(channel.read_descriptor(), deadline, false);
    return result.problem == ava::process::detail::LaunchProtocolProblemV1::TimedOut && std::chrono::steady_clock::now() >= deadline ? 0 : 2;
  });
  static_cast<void>(ava::tests::expect_child_success(status, "an early poll timeout cannot shorten the absolute launch deadline"));
#endif
}

void test_parent_cancellation_is_observed_in_bounded_slices()
{
  using namespace ava::process::detail;

  TestPipe channel;
  std::size_t observations = 0;
  auto const started = std::chrono::steady_clock::now();
  auto const canceled = await_launch_exec_confirmation(channel.read_descriptor(), started + 1s, false, -1, [&] { return ++observations >= 3; });
  auto const elapsed = std::chrono::steady_clock::now() - started;
  TestPipe throwing_channel;
  auto const throwing =
      await_launch_exec_confirmation(throwing_channel.read_descriptor(), std::chrono::steady_clock::now() + 1s, false, -1, []() -> bool { throw 1; });
  expect(channel.valid() && canceled.disposition == LaunchProtocolDispositionV1::LaunchFailed && canceled.problem == LaunchProtocolProblemV1::Canceled &&
             observations == 3 && elapsed < 250ms && throwing_channel.valid() && throwing.disposition == LaunchProtocolDispositionV1::LaunchFailed &&
             throwing.problem == LaunchProtocolProblemV1::Canceled,
         "common spawn exec confirmation observes true or throwing parent cancellation in bounded slices before its deadline");
}

void test_deadline_precedes_parent_cancellation()
{
  using namespace ava::process::detail;

  TestPipe channel;
  std::size_t observations = 0;
  auto const timed_out = await_launch_exec_confirmation(channel.read_descriptor(), std::chrono::steady_clock::now() - 1ms, false, -1, [&] {
    ++observations;
    return true;
  });
  expect(channel.valid() && timed_out.disposition == LaunchProtocolDispositionV1::LaunchFailed && timed_out.problem == LaunchProtocolProblemV1::TimedOut &&
             observations == 0,
         "an already-exhausted launch deadline wins before a true parent-cancellation callback is observed");
}

void test_deadline_crossing_during_parent_cancellation_callback()
{
  using namespace ava::process::detail;

  TestPipe channel;
  CallbackLatch callback;
  auto const deadline = std::chrono::steady_clock::now() + 100ms;
  std::optional<LaunchProtocolOutcomeV1> outcome;
  std::thread waiter([&] {
    outcome = await_launch_exec_confirmation(channel.read_descriptor(), deadline, false, -1, [&]() -> bool {
      callback.arrive_and_wait();
      throw 1;
    });
  });
  bool const entered = callback.wait_until(deadline);
  if (entered)
    wait_until(deadline);
  callback.release();
  waiter.join();
  expect(entered && outcome && outcome->disposition == LaunchProtocolDispositionV1::LaunchFailed && outcome->problem == LaunchProtocolProblemV1::TimedOut,
         "a launch deadline crossed during a blocking, throwing cancellation callback wins over cancellation");
}

void test_malformed_truncated_and_out_of_order_protocol()
{
  using namespace ava::process::detail;

  TestPipe malformed_pipe;
  LaunchFrameV1 malformed{.magic = 0,
                          .version = kLaunchFrameVersionV1,
                          .kind = static_cast<std::uint8_t>(LaunchFrameKindV1::ExecAttempt),
                          .stage = static_cast<std::uint8_t>(LaunchFailureStageV1::None),
                          .error_number = 0,
                          .reserved = 0};
  bool const malformed_written = malformed_pipe.valid() && write_all(malformed_pipe.write_descriptor(), &malformed, sizeof(malformed));
  auto const malformed_result = finish_exec_sequence(malformed_pipe);

  TestPipe truncated_pipe;
  LaunchFrameV1 frame{.magic = kLaunchFrameMagicV1,
                      .version = kLaunchFrameVersionV1,
                      .kind = static_cast<std::uint8_t>(LaunchFrameKindV1::ExecAttempt),
                      .stage = static_cast<std::uint8_t>(LaunchFailureStageV1::None),
                      .error_number = 0,
                      .reserved = 0};
  bool const truncated_written = truncated_pipe.valid() && write_all(truncated_pipe.write_descriptor(), &frame, sizeof(frame) / 2);
  auto const truncated_result = finish_exec_sequence(truncated_pipe);

  TestPipe out_of_order_pipe;
  bool const out_of_order_written = out_of_order_pipe.valid() && child_write_exec_failed(out_of_order_pipe.write_descriptor(), ENOEXEC);
  auto const out_of_order_result = finish_exec_sequence(out_of_order_pipe);

  expect(malformed_written && malformed_result.disposition == LaunchProtocolDispositionV1::LaunchFailed &&
             malformed_result.problem == LaunchProtocolProblemV1::MalformedFrame,
         "a malformed versioned launch frame fails closed before success");
  expect(truncated_written && truncated_result.disposition == LaunchProtocolDispositionV1::LaunchFailed &&
             truncated_result.problem == LaunchProtocolProblemV1::TruncatedFrame,
         "a truncated fixed launch frame fails closed before success");
  expect(out_of_order_written && out_of_order_result.disposition == LaunchProtocolDispositionV1::LaunchFailed &&
             out_of_order_result.problem == LaunchProtocolProblemV1::OutOfOrderFrame,
         "a valid frame in an invalid semantic order is a launch failure rather than an exec failure");
}

void test_containment_checkpoint_sequence()
{
  using namespace ava::process::detail;

  TestPipe status;
  TestPipe continuation;
  bool const frames_written = status.valid() && continuation.valid() && child_write_bash_containment_applied(status.write_descriptor()) &&
                              child_write_exec_attempt(status.write_descriptor());
  status.close_write();
  auto const confirmed = await_launch_exec_confirmation(status.read_descriptor(), std::chrono::steady_clock::now() + 1s, true, continuation.write_descriptor());
  char continued = '\0';
  auto const continuation_read = ::read(continuation.read_descriptor(), &continued, 1);

  TestPipe unexpected;
  bool const unexpected_written = unexpected.valid() && child_write_bash_containment_applied(unexpected.write_descriptor());
  auto const rejected = finish_exec_sequence(unexpected);

  expect(frames_written && confirmed.disposition == LaunchProtocolDispositionV1::ExecConfirmed && continuation_read == 1 && continued == 'C',
         "required Bash containment continues only after its valid checkpoint and still requires exec confirmation");
  expect(
      unexpected_written && rejected.disposition == LaunchProtocolDispositionV1::LaunchFailed && rejected.problem == LaunchProtocolProblemV1::OutOfOrderFrame,
      "a Bash containment checkpoint is a protocol failure when the closed specification selected None");
}

// Releasing a copied monitor callback must precede Supervisor destruction, even
// when a launch or snapshot throws. The latch itself outlives the supervisor.
struct ReleaseMonitorHook final
{
  ava::process::Supervisor& supervisor;
  CallbackLatch& observed;

  ~ReleaseMonitorHook()
  {
    ava::process::testing::SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
    observed.release();
  }
};

std::string describe_launch_snapshot(ava::process::ProcessSnapshotV1 const& snapshot)
{
  std::string description = " records=" + std::to_string(snapshot.records.size()) + " live=" + std::to_string(snapshot.live_records);
  for (auto const& record : snapshot.records)
  {
    description += " state=" + std::string(ava::process::to_string(record.state)) +
                   " reason=" + (record.reason ? std::string(ava::process::to_string(*record.reason)) : "unset") +
                   " kind=" + std::string(ava::process::to_string(record.exit_kind)) + " cleanup=" + std::string(ava::process::to_string(record.cleanup)) +
                   " settlements=" + std::to_string(record.settlement_count) + " has_exit=" + std::to_string(record.has_exit_code) +
                   " exit=" + std::to_string(record.exit_code);
  }
  return description;
}

template <typename Launch>
void check_exit_before_launch_confirmation(std::string const& label, ava::process::ProcessRoleV1 role, ava::process::TerminationReasonV1 expected_reason,
                                           std::string_view expected_protocol, std::string_view expected_stage, std::filesystem::path const& marker,
                                           Launch launch)
{
  using namespace ava::process;
  using testing::SupervisorTestAccess;
  auto application = OwnerPathV1::application();
  expect(application.has_value(), label + ": create application owner");
  if (!application)
    return;
  auto operation = application->operation();
  expect(operation.has_value(), label + ": create operation owner");
  if (!operation)
    return;

  CallbackLatch observed;
  Supervisor supervisor;
  ReleaseMonitorHook release_on_exit{supervisor, observed};
  auto reservation = supervisor.reserve(*operation, role, {.startup_timeout = 30s, .execution_deadline = std::nullopt});
  expect(reservation.has_value(), label + ": reserve launch" + (reservation ? "" : ": " + reservation.error().format()));
  if (!reservation)
    return;

  bool gate_released = false;
  bool exit_observed = false;
  ProcessSnapshotV1 held;
  testing::ProcessMonitorSnapshot monitor;
  bool const stop_before_confirmation = expected_reason == TerminationReasonV1::Canceled || expected_reason == TerminationReasonV1::DeadlineExpired;
  SupervisorTestAccess::set_after_poll_snapshot_hook(supervisor, [&] {
    auto const snapshot = supervisor.snapshot();
    if (snapshot.records.size() == 1 && snapshot.records.front().has_exit_code)
      observed.arrive_and_wait();
  });
  SupervisorTestAccess::set_after_gate_release_hook(supervisor, [&] {
    ReleaseMonitorHook release_after_observation{supervisor, observed};
    gate_released = true;
    exit_observed = observed.wait_until(std::chrono::steady_clock::now() + 10s);
    held = supervisor.snapshot();
    monitor = SupervisorTestAccess::monitor_snapshot(supervisor);
    if (stop_before_confirmation)
    {
      auto const stopped = supervisor.request_stop(*application, expected_reason, std::chrono::steady_clock::now() + 10s);
      expect(stopped && stopped->newly_requested == 1, label + ": commit the competing stop before classifying exec");
    }
  });

  // The monitor hook runs after process_record_locked, outside its mutex.
  // has_exit_code proves an exact leader-exit observation, not merely a cycle
  // count or a poll-set installation. Freezing that turn also makes an early
  // natural group observation a deterministic failure, without a child sleep.
  auto result = launch(supervisor, std::move(*reservation));
  SupervisorTestAccess::clear_after_gate_release_hook(supervisor);
  SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
  observed.release();
  expect(gate_released && exit_observed, label + ": gate released and leader exit observed before confirmation" + describe_launch_snapshot(held));
  expect(held.records.size() == 1 && held.records.front().state != ProcessStateV1::Finished && !held.records.front().reason &&
             held.records.front().settlement_count == 0 && monitor.counters.group_observations == 0,
         label + ": unclassified launch cannot start natural finalization; group_observations=" + std::to_string(monitor.counters.group_observations) +
             " quiet_proofs=" + std::to_string(monitor.counters.quiet_group_proofs) + describe_launch_snapshot(held));

  bool const successful_exec = expected_reason == TerminationReasonV1::NaturalExit || stop_before_confirmation;
  expect(result.has_value() == (successful_exec && !stop_before_confirmation),
         label + ": exact launch result" + (result ? ": accepted" : ": " + result.error().format()));
  if (result)
  {
    auto const status = supervisor.wait(*result, std::chrono::steady_clock::now() + 10s);
    expect(status && status->reason == expected_reason && status->kind == ExitKindV1::Exited && status->has_exit_code && status->exit_code == 0,
           label + ": successful fast exec publishes its exact final status" + (status ? "" : ": " + status.error().format()));
  }
  else
  {
    auto context_value = [&](std::string_view key) -> std::string_view {
      for (auto const& context : result.error().context())
        if (context.key == key)
          return context.value;
      return {};
    };
    if (stop_before_confirmation)
      expect(context_value("reason") == to_string(expected_reason), label + ": the earlier stop remains authoritative: " + result.error().format());
    else
      expect(context_value("protocol") == expected_protocol && context_value("stage") == expected_stage,
             label + ": exact child-error protocol/stage: " + result.error().format());
  }

  auto const settled = supervisor.snapshot();
  auto const expected_kind = successful_exec ? ExitKindV1::Exited : ExitKindV1::LaunchError;
  expect(settled.live_records == 0 && settled.records.size() == 1 && settled.records.front().state == ProcessStateV1::Finished &&
             settled.records.front().reason == expected_reason && settled.records.front().exit_kind == expected_kind &&
             settled.records.front().cleanup == CleanupStateV1::Complete && settled.records.front().settlement_count == 1 &&
             settled.records.front().has_exit_code == successful_exec && !settled.records.front().has_signal_number,
         label + ": one settlement with exact reason, exit kind and cleanup" + describe_launch_snapshot(settled));

  bool const marker_exists = std::filesystem::exists(marker);
  std::ifstream input(marker, std::ios::binary);
  std::string const marker_text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  expect(marker_exists == successful_exec && marker_text == (successful_exec ? "EXECUTED\n" : ""),
         label + ": expected exec marker; exists=" + std::to_string(marker_exists) + " contents=" + marker_text);
  expect(supervisor.shutdown(std::chrono::steady_clock::now() + 10s).complete, label + ": supervisor cleanup completes");
}

void test_exit_before_launch_confirmation()
{
  using namespace ava::process;
  auto root = ScopedTestDirectory::create_unique(temp_root(), "launch-confirmation-race");
  auto const invalid_image = root / "invalid-image";
  {
    std::ofstream output(invalid_image, std::ios::binary);
    output << "not an executable image\n";
    output.close();
    expect(!output.fail(), "launch confirmation race: write invalid executable image");
    if (output.fail())
      return;
  }
  bool const executable = ::chmod(invalid_image.c_str(), 0700) == 0;
  expect(executable, "launch confirmation race: make invalid image executable");
  if (!executable)
    return;

  struct AdoptionCase
  {
    char const* name;
    std::optional<AdoptionChildFailureStageV1> stage;
    TerminationReasonV1 reason;
    char const* protocol;
    char const* protocol_stage;
    bool eof = false;
  };
  std::array const cases{
      AdoptionCase{"success", {}, TerminationReasonV1::NaturalExit, "", ""},
      AdoptionCase{"eof", {}, TerminationReasonV1::LaunchFailed, "eof_before_exec_attempt", "", true},
      AdoptionCase{"streams", AdoptionChildFailureStageV1::Streams, TerminationReasonV1::LaunchFailed, "child_reported", "streams"},
      AdoptionCase{"working-directory", AdoptionChildFailureStageV1::WorkingDirectory, TerminationReasonV1::LaunchFailed, "child_reported",
                   "working_directory"},
      AdoptionCase{"containment", AdoptionChildFailureStageV1::Containment, TerminationReasonV1::LaunchFailed, "child_reported", "containment"},
      AdoptionCase{"descriptor-validation", AdoptionChildFailureStageV1::DescriptorValidation, TerminationReasonV1::LaunchFailed, "child_reported",
                   "descriptor_validation"},
      AdoptionCase{"exec-failed", {}, TerminationReasonV1::ExecFailed, "child_reported", ""},
  };
  for (auto const& item : cases)
  {
    auto const marker = root / (std::string("adopt-") + item.name);
    check_exit_before_launch_confirmation(
        std::string("adopt/") + item.name, ProcessRoleV1::Bash, item.reason, item.protocol, item.protocol_stage, marker,
        [&](Supervisor& supervisor, Reservation&& reservation) -> ava::core::Result<ProcessHandle> {
          auto environment = validate_bash_environment_v1(kBashEnvironmentProfileIdV1, "/",
                                                          {{"LANG", "C.UTF-8"},
                                                           {"LC_ALL", "C.UTF-8"},
                                                           {"LC_CTYPE", "C.UTF-8"},
                                                           {"TZ", "UTC"},
                                                           {"USER", "ava-test"},
                                                           {"LOGNAME", "ava-test"},
                                                           {"PWD", "/"},
                                                           {"PATH", "/usr/bin:/bin"},
                                                           {"HOME", root.path().string()},
                                                           {"XDG_CONFIG_HOME", root.path().string()},
                                                           {"XDG_CACHE_HOME", root.path().string()},
                                                           {"XDG_DATA_HOME", root.path().string()},
                                                           {"XDG_STATE_HOME", root.path().string()},
                                                           {"TMPDIR", root.path().string()}});
          if (!environment)
            return std::unexpected(environment.error());
          auto anchors = ava::core::AnchorSet::open({"/"});
          if (!anchors)
            return std::unexpected(anchors.error());
          auto cwd = mint_anchored_working_directory(std::move(*anchors), "/");
          if (!cwd)
            return std::unexpected(cwd.error());
          auto const image = item.reason == TerminationReasonV1::ExecFailed ? invalid_image : std::filesystem::path(AVA_FAKE_PROCESS_CHILD_PATH);
          auto gate = supervisor.begin_secure_adoption(
              std::move(reservation),
              {.environment = std::move(*environment), .argv = {image.string(), "exec-marker", marker.string()}, .cwd = "/", .anchored_cwd = std::move(*cwd)});
          if (!gate)
            return std::unexpected(gate.error());
          int const descriptor = ::open(image.c_str(), O_RDONLY | O_CLOEXEC);
          if (descriptor < 0)
            return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "cannot open race fixture executable"));
          auto branch = gate->fork_leader();
          if (branch && *branch == AdoptionForkBranchV1::Child)
          {
            if (item.eof)
              _exit(0);
            if (item.stage)
              gate->child_launch_failed(*item.stage, EACCES);
            gate->child_exec_descriptor(descriptor);
          }
          static_cast<void>(::close(descriptor));
          if (!branch)
            return std::unexpected(branch.error());
          return supervisor.adopt(std::move(*gate));
        });
  }

  for (auto reason : {TerminationReasonV1::NaturalExit, TerminationReasonV1::LaunchFailed, TerminationReasonV1::ExecFailed, TerminationReasonV1::Canceled,
                      TerminationReasonV1::DeadlineExpired})
  {
    auto const name = std::string(to_string(reason));
    auto const marker = root / ("spawn-" + name);
    check_exit_before_launch_confirmation(
        "spawn/" + name, ProcessRoleV1::Plugin, reason, reason == TerminationReasonV1::NaturalExit ? "" : "child_reported",
        reason == TerminationReasonV1::LaunchFailed ? "working_directory" : "", marker,
        [&](Supervisor& supervisor, Reservation&& reservation) -> ava::core::Result<ProcessHandle> {
          auto environment = make_plugin_environment_v1("/");
          if (!environment)
            return std::unexpected(environment.error());
          if (reason == TerminationReasonV1::LaunchFailed)
            testing::SupervisorTestAccess::fail_next_common_child_working_directory(supervisor);
          auto const image = reason == TerminationReasonV1::ExecFailed ? invalid_image.string() : std::string(AVA_FAKE_PROCESS_CHILD_PATH);
          auto child =
              supervisor.spawn(std::move(reservation),
                               {.executable = image, .argv = {image, "exec-marker", marker.string()}, .environment = std::move(*environment), .cwd = "/"});
          if (!child)
            return std::unexpected(child.error());
          return std::move(child->handle);
        });
  }
}

#endif

}  // namespace

void run_process_launch_protocol_posix_tests()
{
#if defined(_WIN32)
  ava::tests::request_skip("process launch framing is compile-time unsupported on Windows");
#else
  test_fixed_protocol_success_and_failures();
  test_absolute_deadline_timeout();
  test_early_poll_timeout_preserves_launch_deadline();
  test_parent_cancellation_is_observed_in_bounded_slices();
  test_deadline_precedes_parent_cancellation();
  test_deadline_crossing_during_parent_cancellation_callback();
  test_malformed_truncated_and_out_of_order_protocol();
  test_containment_checkpoint_sequence();
  test_exit_before_launch_confirmation();
#endif
}
