#include "sys.h"
#include "tests/backend_benchmark_cases.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(AVA_BENCHMARK_HAS_PROCESS_SUPERVISOR)
#include "ava/process/environment.h"
#include "ava/process/owner.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/process/supervisor_test_support.h"
#endif

namespace ava::benchmark {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

[[maybe_unused]] constexpr std::string_view kSourceArchitectureAbsentReason = "Process-supervision source architecture is absent from this build.";
[[maybe_unused]] constexpr std::string_view kFixtureUnavailableReason = "A required repository-owned benchmark fixture is unavailable.";
[[maybe_unused]] constexpr std::string_view kPidfdUnavailableReason = "Automatic monitoring did not select pidfd on this host.";
[[maybe_unused]] constexpr std::string_view kPlatformUnsupportedReason = "The process-supervision backend is unavailable on this platform.";

#if defined(AVA_BENCHMARK_HAS_PROCESS_SUPERVISOR)

[[noreturn]] void fail(std::string const& message)
{
  std::cerr << message << '\n';
  std::exit(2);
}

[[nodiscard]] bool executable_file(std::filesystem::path const& path)
{
  return !path.empty() && std::filesystem::is_regular_file(path) && ::access(path.c_str(), X_OK) == 0;
}

[[nodiscard]] bool no_waitable_children()
{
  errno = 0;
  int status = 0;
  auto const waited = ::waitpid(-1, &status, WNOHANG);
  return waited == -1 && errno == ECHILD;
}

[[nodiscard]] double elapsed_nanoseconds(Clock::time_point const started)
{
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

[[nodiscard]] std::int64_t signed_delta(std::uint64_t after, std::uint64_t before)
{
  if (after >= before)
    return static_cast<std::int64_t>(after - before);
  return -static_cast<std::int64_t>(before - after);
}

using ava::process::CleanupStateV1;
using ava::process::ExitKindV1;
using ava::process::ExitStatusV1;
using ava::process::PipeEndpoint;
using ava::process::PipeIoStateV1;
using ava::process::ProcessRoleV1;
using ava::process::ProcessStateV1;
using ava::process::SpawnResultV1;
using ava::process::StreamModeV1;
using ava::process::Supervisor;
using ava::process::TerminationReasonV1;
using ava::process::testing::ProcessMonitorBackendMode;
using ava::process::testing::ProcessMonitorSnapshot;
using ava::process::testing::SupervisorTestAccess;

struct DriverChecks
{
  bool shutdown_complete = false;
  bool live_records_zero = false;
  bool records_finished = false;
  bool settlement_once = false;
  bool immediate_child_guard = false;
};

struct CurrentRss
{
  double kib = 0.0;
};

[[nodiscard]] std::optional<CurrentRss> current_rss()
{
#if defined(__linux__)
  std::ifstream input("/proc/self/statm");
  std::uint64_t total_pages = 0;
  std::uint64_t resident_pages = 0;
  auto const page_size = ::sysconf(_SC_PAGESIZE);
  if (!(input >> total_pages >> resident_pages) || page_size <= 0)
    return std::nullopt;
  static_cast<void>(total_pages);
  return CurrentRss{.kib = static_cast<double>(resident_pages) * static_cast<double>(page_size) / 1024.0};
#else
  return std::nullopt;
#endif
}

[[nodiscard]] std::size_t current_thread_count()
{
#if defined(__linux__)
  std::error_code error;
  std::size_t count = 0;
  for (std::filesystem::directory_iterator iterator("/proc/self/task", error), end; !error && iterator != end; iterator.increment(error))
    ++count;
  return error ? 0 : count;
#else
  return 0;
#endif
}

[[nodiscard]] ava::process::OwnerPathV1 application_owner()
{
  auto owner = ava::process::OwnerPathV1::application();
  if (!owner)
    fail(owner.error().format());
  return std::move(*owner);
}

[[nodiscard]] ava::process::OwnerPathV1 operation_owner(ava::process::OwnerPathV1 const& application)
{
  auto owner = application.operation();
  if (!owner)
    fail(owner.error().format());
  return std::move(*owner);
}

[[nodiscard]] ava::process::SpawnSpecV1 fixture_spec(BackendBenchmarkOptions const& options, std::string mode, StreamModeV1 input = StreamModeV1::Discard,
                                                     StreamModeV1 output = StreamModeV1::Discard)
{
  auto environment = ava::process::make_plugin_environment_v1("/");
  if (!environment)
    fail(environment.error().format());
  return {.executable = options.fake_process_child.string(),
          .argv = {options.fake_process_child.string(), std::move(mode)},
          .environment = std::move(*environment),
          .cwd = "/",
          .stdin_mode = input,
          .stdout_mode = output,
          .stderr_mode = StreamModeV1::Discard};
}

[[nodiscard]] SpawnResultV1 spawn_fixture(Supervisor& supervisor, ava::process::OwnerPathV1 const& application, BackendBenchmarkOptions const& options,
                                          std::string mode, StreamModeV1 input = StreamModeV1::Discard, StreamModeV1 output = StreamModeV1::Discard,
                                          std::chrono::milliseconds grace = 75ms)
{
  auto reservation = supervisor.reserve(operation_owner(application), ProcessRoleV1::Plugin,
                                        {.termination_grace = grace, .startup_timeout = 5s, .execution_deadline = std::nullopt});
  if (!reservation)
    fail(reservation.error().format());
  auto child = supervisor.spawn(std::move(*reservation), fixture_spec(options, std::move(mode), input, output));
  if (!child)
    fail(child.error().format());
  return std::move(*child);
}

[[nodiscard]] SpawnResultV1 timed_spawn(Supervisor& supervisor, ava::process::OwnerPathV1 const& application, BackendBenchmarkOptions const& options,
                                        double& elapsed)
{
  auto const started = Clock::now();
  auto environment = ava::process::make_plugin_environment_v1("/");
  if (!environment)
    fail(environment.error().format());
  auto reservation = supervisor.reserve(operation_owner(application), ProcessRoleV1::Plugin);
  if (!reservation)
    fail(reservation.error().format());
  auto child = supervisor.spawn(std::move(*reservation), {.executable = options.fake_process_child.string(),
                                                          .argv = {options.fake_process_child.string(), "normal"},
                                                          .environment = std::move(*environment),
                                                          .cwd = "/",
                                                          .stdin_mode = StreamModeV1::Discard,
                                                          .stdout_mode = StreamModeV1::Discard,
                                                          .stderr_mode = StreamModeV1::Discard});
  elapsed = elapsed_nanoseconds(started);
  if (!child)
    fail(child.error().format());
  return std::move(*child);
}

[[nodiscard]] std::span<std::byte> writable_bytes(std::array<char, 256>& buffer)
{
  return {reinterpret_cast<std::byte*>(buffer.data()), buffer.size()};
}

[[nodiscard]] bool read_until(PipeEndpoint& endpoint, std::string& received, std::string_view marker, Clock::time_point deadline)
{
  std::array<char, 256> buffer{};
  while (received.find(marker) == std::string::npos && received.size() < 4096)
  {
    auto result = endpoint.read(writable_bytes(buffer));
    if (!result)
      return false;
    if (result->state == PipeIoStateV1::Progress)
    {
      received.append(buffer.data(), result->bytes);
      continue;
    }
    if (result->state == PipeIoStateV1::EndOfStream)
      return received.find(marker) != std::string::npos;
    auto ready = endpoint.wait_readable(deadline);
    if (!ready || !*ready)
      return false;
  }
  return received.find(marker) != std::string::npos;
}

[[nodiscard]] bool write_release(PipeEndpoint& endpoint, Clock::time_point deadline)
{
  std::array<std::byte, 1> value{};
  while (true)
  {
    auto result = endpoint.write(value);
    if (!result)
      return false;
    if (result->state == PipeIoStateV1::Progress)
      return result->bytes == 1;
    auto ready = endpoint.wait_writable(deadline);
    if (!ready || !*ready)
      return false;
  }
}

[[nodiscard]] bool read_to_eof(PipeEndpoint& endpoint, Clock::time_point deadline)
{
  std::array<char, 256> buffer{};
  while (true)
  {
    auto result = endpoint.read(writable_bytes(buffer));
    if (!result)
      return false;
    if (result->state == PipeIoStateV1::EndOfStream)
      return true;
    if (result->state == PipeIoStateV1::Progress)
      continue;
    auto ready = endpoint.wait_readable(deadline);
    if (!ready || !*ready)
      return false;
  }
}

void close_endpoints(SpawnResultV1& child)
{
  if (child.standard_input)
    child.standard_input->close();
  if (child.standard_output)
    child.standard_output->close();
  if (child.standard_error)
    child.standard_error->close();
  child.standard_input.reset();
  child.standard_output.reset();
  child.standard_error.reset();
}

[[nodiscard]] bool terminal_status_ok(ExitStatusV1 const& status, TerminationReasonV1 reason, ExitKindV1 kind)
{
  return status.reason == reason && status.kind == kind && status.cleanup == CleanupStateV1::Complete;
}

[[nodiscard]] DriverChecks finalize_driver(std::unique_ptr<Supervisor>& supervisor, std::vector<SpawnResultV1>& children, std::size_t expected_records,
                                           ava::process::ProcessDeadline deadline)
{
  for (auto& child : children)
    close_endpoints(child);
  children.clear();
  auto const shutdown = supervisor->shutdown(deadline);
  auto const snapshot = supervisor->snapshot();
  DriverChecks checks;
  checks.shutdown_complete = shutdown.complete && shutdown.incomplete_count == 0;
  checks.live_records_zero = snapshot.live_records == 0;
  checks.records_finished = snapshot.records.size() == expected_records && std::ranges::all_of(snapshot.records, [](auto const& record) {
                              return record.state == ProcessStateV1::Finished && record.cleanup == CleanupStateV1::Complete;
                            });
  checks.settlement_once =
      snapshot.records.size() == expected_records && std::ranges::all_of(snapshot.records, [](auto const& record) { return record.settlement_count == 1; });
  supervisor.reset();
  checks.immediate_child_guard = no_waitable_children();
  return checks;
}

[[nodiscard]] JsonFields driver_check_fields(DriverChecks const& checks)
{
  return {{"shutdown_complete", checks.shutdown_complete},
          {"live_records_zero", checks.live_records_zero},
          {"records_finished", checks.records_finished},
          {"settlement_once", checks.settlement_once},
          {"immediate_child_guard", checks.immediate_child_guard}};
}

void require_fixture_or_emit(BackendBenchmarkOptions const& options, bool& available)
{
  available = executable_file(options.fake_process_child);
  if (!available)
  {
    emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "fixture_unavailable", kFixtureUnavailableReason,
                            {{"process_backend", std::string("event_driven_posix")}});
  }
}

void benchmark_idle_scope(BackendBenchmarkOptions const& options)
{
  auto const rss_before = current_rss();
  auto const threads_before = current_thread_count();
  bool const children_before = no_waitable_children();
  auto const started = Clock::now();
  auto supervisor = std::make_shared<Supervisor>();
  auto scope_result = ava::process::ProcessScopeV1::application(supervisor);
  auto const elapsed = elapsed_nanoseconds(started);
  if (!scope_result)
    fail(scope_result.error().format());
  std::optional<ava::process::ProcessScopeV1> scope(std::move(*scope_result));
  auto const snapshot = supervisor->snapshot();
  auto const rss_after = current_rss();
  auto const threads_after = current_thread_count();
  scope.reset();
  auto shutdown = supervisor->shutdown(Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto final_snapshot = supervisor->snapshot();
  supervisor.reset();
  bool const child_guard = no_waitable_children();
  auto const rss_delta = rss_before && rss_after ? rss_after->kib - rss_before->kib : 0.0;
  auto const thread_delta = static_cast<std::int64_t>(threads_after) - static_cast<std::int64_t>(threads_before);
  emit_helper_measurement(
      options.benchmark_case, "scope_construction_ns", "ns",
      {{.ordinal = 1,
        .value = elapsed,
        .metrics = {{"rss_delta_kib", rss_delta}, {"linux_thread_delta", thread_delta}, {"live_records", static_cast<std::uint64_t>(snapshot.live_records)}},
        .checks = {{"monitor_not_started", !snapshot.monitor_started},
                   {"live_records_zero", snapshot.live_records == 0},
                   {"linux_thread_delta_zero", threads_before == 0 || thread_delta == 0},
                   {"immediate_child_delta_zero", children_before && child_guard},
                   {"shutdown_complete", shutdown.complete},
                   {"final_live_records_zero", final_snapshot.live_records == 0}}}},
      {{"process_backend", std::string("event_driven_posix")}, {"authority", std::string("neutral_supervisor")}});
}

void benchmark_first_spawn(BackendBenchmarkOptions const& options)
{
  bool available = false;
  require_fixture_or_emit(options, available);
  if (!available)
    return;
  auto supervisor = std::make_unique<Supervisor>();
  auto application = application_owner();
  double elapsed = 0.0;
  std::vector<SpawnResultV1> children;
  children.push_back(timed_spawn(*supervisor, application, options, elapsed));
  auto status = supervisor->wait(children.front().handle, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  bool const natural =
      status && terminal_status_ok(*status, TerminationReasonV1::NaturalExit, ExitKindV1::Exited) && status->has_exit_code && status->exit_code == 0;
  auto const checks = finalize_driver(supervisor, children, 1, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto fields = driver_check_fields(checks);
  fields.emplace_back("confirmed_exec", true);
  fields.emplace_back("natural_exit", natural);
  emit_helper_measurement(options.benchmark_case, "spawn_commit_ns", "ns", {{.ordinal = 1, .value = elapsed, .metrics = {}, .checks = std::move(fields)}},
                          {{"process_backend", std::string("event_driven_posix")}, {"authority", std::string("neutral_supervisor")}});
}

void benchmark_warm_sequential(BackendBenchmarkOptions const& options)
{
  bool available = false;
  require_fixture_or_emit(options, available);
  if (!available)
    return;
  auto supervisor = std::make_unique<Supervisor>();
  auto application = application_owner();
  std::vector<SpawnResultV1> children;
  double warmup_elapsed = 0.0;
  children.push_back(timed_spawn(*supervisor, application, options, warmup_elapsed));
  auto warmup_status = supervisor->wait(children.back().handle, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  if (!warmup_status || !terminal_status_ok(*warmup_status, TerminationReasonV1::NaturalExit, ExitKindV1::Exited))
    fail("warm sequential benchmark warmup did not settle naturally");
  std::vector<Observation> observations;
  observations.reserve(options.iterations);
  bool statuses_ok = true;
  for (std::size_t index = 0; index < options.iterations; ++index)
  {
    double elapsed = 0.0;
    children.push_back(timed_spawn(*supervisor, application, options, elapsed));
    auto status = supervisor->wait(children.back().handle, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
    statuses_ok = statuses_ok && status && terminal_status_ok(*status, TerminationReasonV1::NaturalExit, ExitKindV1::Exited) && status->exit_code == 0;
    observations.push_back({.ordinal = index + 1, .value = elapsed, .metrics = {}, .checks = {{"confirmed_exec", true}, {"natural_exit", statuses_ok}}});
  }
  auto const checks = finalize_driver(supervisor, children, options.iterations + 1, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto final_fields = driver_check_fields(checks);
  for (auto& observation : observations)
    observation.checks.insert(observation.checks.end(), final_fields.begin(), final_fields.end());
  emit_helper_measurement(options.benchmark_case, "spawn_commit_ns", "ns", observations,
                          {{"process_backend", std::string("event_driven_posix")},
                           {"authority", std::string("neutral_supervisor")},
                           {"warmup_count", std::uint64_t{1}},
                           {"iteration_count", static_cast<std::uint64_t>(options.iterations)}});
}

class StartGate final
{
 public:
  void await()
  {
    std::unique_lock lock(mutex_);
    ++ready_;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }

  Clock::time_point release(std::size_t expected)
  {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return ready_ == expected; });
    released_at_ = Clock::now();
    released_ = true;
    changed_.notify_all();
    return released_at_;
  }

  [[nodiscard]] Clock::time_point released_at() const
  {
    std::lock_guard lock(mutex_);
    return released_at_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t ready_ = 0;
  bool released_ = false;
  Clock::time_point released_at_{};
};

void benchmark_concurrent(BackendBenchmarkOptions const& options)
{
  bool available = false;
  require_fixture_or_emit(options, available);
  if (!available)
    return;
  auto supervisor = std::make_unique<Supervisor>();
  auto application = application_owner();
  std::vector<ava::process::OwnerPathV1> owners;
  owners.reserve(options.records);
  for (std::size_t index = 0; index < options.records; ++index)
    owners.push_back(operation_owner(application));
  std::vector<std::optional<SpawnResultV1>> slots(options.records);
  std::vector<double> latencies(options.records, 0.0);
  std::vector<std::thread> threads;
  threads.reserve(options.records);
  StartGate gate;
  std::atomic_bool failed = false;
  std::mutex error_mutex;
  std::string first_error;
  for (std::size_t index = 0; index < options.records; ++index)
  {
    threads.emplace_back([&, index] {
      gate.await();
      auto environment = ava::process::make_plugin_environment_v1("/");
      auto reservation = environment ? supervisor->reserve(owners[index], ProcessRoleV1::Plugin)
                                     : ava::core::Result<ava::process::Reservation>(std::unexpected(environment.error()));
      auto child = reservation ? supervisor->spawn(std::move(*reservation), {.executable = options.fake_process_child.string(),
                                                                             .argv = {options.fake_process_child.string(), "input-gate"},
                                                                             .environment = std::move(*environment),
                                                                             .cwd = "/",
                                                                             .stdin_mode = StreamModeV1::Capture,
                                                                             .stdout_mode = StreamModeV1::Discard,
                                                                             .stderr_mode = StreamModeV1::Discard})
                               : ava::core::Result<SpawnResultV1>(std::unexpected(reservation.error()));
      auto const ended = Clock::now();
      latencies[index] = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(ended - gate.released_at()).count());
      if (!child)
      {
        {
          std::lock_guard lock(error_mutex);
          if (first_error.empty())
            first_error = child.error().format();
        }
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      slots[index].emplace(std::move(*child));
    });
  }
  auto const released = gate.release(options.records);
  for (auto& thread : threads)
    thread.join();
  auto const batch_elapsed = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - released).count());
  if (failed.load(std::memory_order_relaxed))
    fail("concurrent spawn benchmark failed to commit every child: " + first_error);
  std::vector<SpawnResultV1> children;
  children.reserve(options.records);
  bool statuses_ok = true;
  for (auto& slot : slots)
  {
    if (!slot)
      fail("concurrent spawn benchmark has an empty child slot");
    children.push_back(std::move(*slot));
  }
  bool released_children = true;
  for (auto& child : children)
  {
    if (child.standard_input)
    {
      released_children = released_children && write_release(*child.standard_input, Clock::now() + 2s);
      child.standard_input->close();
      child.standard_input.reset();
    }
    else
      released_children = false;
    auto status = supervisor->wait(child.handle, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
    statuses_ok = statuses_ok && status && terminal_status_ok(*status, TerminationReasonV1::NaturalExit, ExitKindV1::Exited) && status->exit_code == 0;
  }
  auto const checks = finalize_driver(supervisor, children, options.records, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto common_checks = driver_check_fields(checks);
  std::vector<Observation> observations;
  observations.reserve(options.records);
  for (std::size_t index = 0; index < options.records; ++index)
  {
    auto item_checks = common_checks;
    item_checks.emplace_back("confirmed_exec", true);
    item_checks.emplace_back("natural_exit", statuses_ok);
    item_checks.emplace_back("children_released", released_children);
    item_checks.emplace_back("latency_bounded", latencies[index] < 30'000'000'000.0);
    observations.push_back(
        {.ordinal = index + 1, .value = latencies[index], .metrics = {{"batch_spawn_commit_ns", batch_elapsed}}, .checks = std::move(item_checks)});
  }
  emit_helper_measurement(options.benchmark_case, "spawn_commit_ns", "ns", observations,
                          {{"process_backend", std::string("event_driven_posix")},
                           {"authority", std::string("neutral_supervisor")},
                           {"record_count", static_cast<std::uint64_t>(options.records)},
                           {"batch_spawn_commit_ns", batch_elapsed}});
}

void benchmark_natural_exit(BackendBenchmarkOptions const& options)
{
  bool available = false;
  require_fixture_or_emit(options, available);
  if (!available)
    return;
  auto supervisor = std::make_unique<Supervisor>();
  auto application = application_owner();
  auto const started = Clock::now();
  std::vector<SpawnResultV1> children;
  children.push_back(spawn_fixture(*supervisor, application, options, "normal"));
  auto status = supervisor->wait(children.front().handle, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto const elapsed = elapsed_nanoseconds(started);
  bool const expected =
      status && terminal_status_ok(*status, TerminationReasonV1::NaturalExit, ExitKindV1::Exited) && status->has_exit_code && status->exit_code == 0;
  auto const checks = finalize_driver(supervisor, children, 1, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto fields = driver_check_fields(checks);
  fields.emplace_back("expected_reason", expected);
  emit_helper_measurement(options.benchmark_case, "settlement_ns", "ns", {{.ordinal = 1, .value = elapsed, .metrics = {}, .checks = std::move(fields)}},
                          {{"process_backend", std::string("event_driven_posix")}, {"authority", std::string("neutral_supervisor")}});
}

void benchmark_leader_first(BackendBenchmarkOptions const& options)
{
  bool available = false;
  require_fixture_or_emit(options, available);
  if (!available)
    return;
  auto supervisor = std::make_unique<Supervisor>();
  auto application = application_owner();
  std::vector<SpawnResultV1> children;
  children.push_back(spawn_fixture(*supervisor, application, options, "leader-exits-first", StreamModeV1::Discard, StreamModeV1::Capture,
                                   std::chrono::milliseconds(options.grace_milliseconds)));
  auto& child = children.front();
  if (!child.standard_output)
    fail("leader-first benchmark lacks its inherited endpoint");
  std::string received;
  bool const descendant_ready = read_until(*child.standard_output, received, "DESCENDANT_READY\n", Clock::now() + 2s);
  auto const started = Clock::now();
  bool const leader_phase = read_until(*child.standard_output, received, "LEADER_EXITING\n", Clock::now() + 2s);
  bool const eof = read_to_eof(*child.standard_output, Clock::now() + 5s);
  auto status = supervisor->wait(child.handle, Clock::now() + 5s);
  auto const elapsed = elapsed_nanoseconds(started);
  bool const expected = status && terminal_status_ok(*status, TerminationReasonV1::NaturalExit, ExitKindV1::Exited) && status->exit_code == 0;
  auto const checks = finalize_driver(supervisor, children, 1, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto fields = driver_check_fields(checks);
  fields.emplace_back("descendant_ready", descendant_ready);
  fields.emplace_back("leader_exit_phase", leader_phase);
  fields.emplace_back("inherited_endpoint_eof", eof);
  fields.emplace_back("expected_reason", expected);
  emit_helper_measurement(options.benchmark_case, "cleanup_settlement_ns", "ns", {{.ordinal = 1, .value = elapsed, .metrics = {}, .checks = std::move(fields)}},
                          {{"process_backend", std::string("event_driven_posix")}, {"authority", std::string("neutral_supervisor")}});
}

void benchmark_term_refusal(BackendBenchmarkOptions const& options)
{
  bool available = false;
  require_fixture_or_emit(options, available);
  if (!available)
    return;
  auto supervisor = std::make_unique<Supervisor>();
  auto application = application_owner();
  std::vector<SpawnResultV1> children;
  children.push_back(spawn_fixture(*supervisor, application, options, "ignore-term", StreamModeV1::Discard, StreamModeV1::Capture,
                                   std::chrono::milliseconds(options.grace_milliseconds)));
  auto& child = children.front();
  if (!child.standard_output)
    fail("TERM-refusal benchmark lacks its readiness endpoint");
  std::string received;
  bool const ready = read_until(*child.standard_output, received, "READY\n", Clock::now() + 2s);
  auto const started = Clock::now();
  auto stopped = supervisor->request_stop(child.handle, TerminationReasonV1::Canceled, Clock::now() + 5s);
  auto status = supervisor->wait(child.handle, Clock::now() + 5s);
  bool const eof = read_to_eof(*child.standard_output, Clock::now() + 5s);
  auto const elapsed = elapsed_nanoseconds(started);
  bool const expected = stopped && stopped->matched == 1 && stopped->newly_requested == 1 && status &&
                        terminal_status_ok(*status, TerminationReasonV1::Canceled, ExitKindV1::Signaled) && status->has_signal_number &&
                        status->signal_number == SIGKILL;
  auto const checks = finalize_driver(supervisor, children, 1, Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto fields = driver_check_fields(checks);
  fields.emplace_back("child_ready", ready);
  fields.emplace_back("expected_reason", expected);
  fields.emplace_back("expected_escalation", expected);
  fields.emplace_back("endpoint_eof", eof);
  emit_helper_measurement(options.benchmark_case, "stop_settlement_ns", "ns", {{.ordinal = 1, .value = elapsed, .metrics = {}, .checks = std::move(fields)}},
                          {{"process_backend", std::string("event_driven_posix")}, {"authority", std::string("neutral_supervisor")}});
}

void benchmark_shared_shutdown(BackendBenchmarkOptions const& options)
{
  bool available = false;
  require_fixture_or_emit(options, available);
  if (!available)
    return;
  auto supervisor = std::make_unique<Supervisor>();
  auto application = application_owner();
  std::vector<SpawnResultV1> children;
  children.reserve(options.records);
  bool ready = true;
  for (std::size_t index = 0; index < options.records; ++index)
  {
    children.push_back(spawn_fixture(*supervisor, application, options, "ignore-term", StreamModeV1::Discard, StreamModeV1::Capture,
                                     std::chrono::milliseconds(options.grace_milliseconds)));
    std::string received;
    bool const child_ready = children.back().standard_output && read_until(*children.back().standard_output, received, "READY\n",
                                                                           Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
    ready = child_ready && ready;
  }
  auto const started = Clock::now();
  auto const shutdown = supervisor->shutdown(Clock::now() + std::chrono::milliseconds(options.deadline_milliseconds));
  auto const elapsed = elapsed_nanoseconds(started);
  bool statuses = true;
  bool endpoints_eof = true;
  for (auto& child : children)
  {
    auto status = supervisor->wait(child.handle, Clock::now() + 500ms);
    statuses = statuses && status && terminal_status_ok(*status, TerminationReasonV1::ApplicationShutdown, ExitKindV1::Signaled);
    endpoints_eof = endpoints_eof && child.standard_output && read_to_eof(*child.standard_output, Clock::now() + 2s);
  }
  auto const snapshot = supervisor->snapshot();
  for (auto& child : children)
    close_endpoints(child);
  children.clear();
  bool const finished =
      snapshot.live_records == 0 && snapshot.records.size() == options.records && std::ranges::all_of(snapshot.records, [](auto const& record) {
        return record.state == ProcessStateV1::Finished && record.cleanup == CleanupStateV1::Complete && record.settlement_count == 1;
      });
  supervisor.reset();
  bool const child_guard = no_waitable_children();
  emit_helper_measurement(options.benchmark_case, "shutdown_ns", "ns",
                          {{.ordinal = 1,
                            .value = elapsed,
                            .metrics = {{"record_count", static_cast<std::uint64_t>(options.records)}},
                            .checks = {{"children_ready", ready},
                                       {"shutdown_complete", shutdown.complete && shutdown.incomplete_count == 0},
                                       {"records_finished", finished},
                                       {"expected_reason", statuses},
                                       {"settlement_once", finished},
                                       {"endpoint_eof", endpoints_eof},
                                       {"immediate_child_guard", child_guard}}}},
                          {{"process_backend", std::string("event_driven_posix")},
                           {"authority", std::string("neutral_supervisor")},
                           {"record_count", static_cast<std::uint64_t>(options.records)}});
}

[[nodiscard]] std::uint64_t process_cpu_nanoseconds()
{
  timespec value{};
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0)
    fail("CLOCK_PROCESS_CPUTIME_ID is unavailable");
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(value.tv_nsec);
}

[[nodiscard]] bool fallback_stabilized(ProcessMonitorSnapshot const& snapshot, std::size_t records)
{
  return snapshot.records.size() == records &&
         std::ranges::all_of(snapshot.records, [](auto const& record) { return record.current_delay_milliseconds == 1000; });
}

void cleanup_monitor_children(std::unique_ptr<Supervisor>& supervisor, std::vector<SpawnResultV1>& children, std::size_t records, DriverChecks& checks,
                              bool& endpoints_eof)
{
  auto const shutdown = supervisor->shutdown(Clock::now() + 3s);
  bool statuses = true;
  endpoints_eof = true;
  for (auto& child : children)
  {
    auto status = supervisor->wait(child.handle, Clock::now() + 500ms);
    statuses = statuses && status && status->cleanup == CleanupStateV1::Complete;
    endpoints_eof = endpoints_eof && child.standard_output && read_to_eof(*child.standard_output, Clock::now() + 2s);
  }
  for (auto& child : children)
    close_endpoints(child);
  children.clear();
  auto const snapshot = supervisor->snapshot();
  auto const monitor = SupervisorTestAccess::monitor_snapshot(*supervisor);
  checks.shutdown_complete = shutdown.complete && statuses;
  checks.live_records_zero = snapshot.live_records == 0;
  checks.records_finished = snapshot.records.size() == records && std::ranges::all_of(snapshot.records, [](auto const& record) {
                              return record.state == ProcessStateV1::Finished && record.cleanup == CleanupStateV1::Complete;
                            });
  checks.settlement_once =
      snapshot.records.size() == records && std::ranges::all_of(snapshot.records, [](auto const& record) { return record.settlement_count == 1; });
  bool const monitor_released =
      monitor.counters.current_monitor_threads == 0 && monitor.counters.current_watches == 0 && monitor.counters.current_wake_descriptors == 0;
  checks.records_finished = checks.records_finished && monitor_released;
  supervisor.reset();
  checks.immediate_child_guard = no_waitable_children();
}

void benchmark_monitor(BackendBenchmarkOptions const& options, bool force_fallback)
{
  bool available = false;
  require_fixture_or_emit(options, available);
  if (!available)
    return;
  auto supervisor = std::make_unique<Supervisor>();
  auto application = application_owner();
  if (force_fallback && !SupervisorTestAccess::set_monitor_backend_mode(*supervisor, ProcessMonitorBackendMode::ForceFallbackUnavailable))
    fail("failed to force the monitor fallback before startup");
  std::vector<SpawnResultV1> children;
  children.reserve(options.records);
  bool ready = true;
  for (std::size_t index = 0; index < options.records; ++index)
  {
    children.push_back(spawn_fixture(*supervisor, application, options, "ready-gate", StreamModeV1::Capture, StreamModeV1::Capture));
    std::string received;
    ready = ready && children.back().standard_output && read_until(*children.back().standard_output, received, "READY\n", Clock::now() + 2s);
  }

  if (force_fallback)
  {
    auto const stabilization_deadline = Clock::now() + 4s;
    while (Clock::now() < stabilization_deadline && !fallback_stabilized(SupervisorTestAccess::monitor_snapshot(*supervisor), options.records))
      std::this_thread::sleep_for(2ms);
  }
  auto const before = SupervisorTestAccess::monitor_snapshot(*supervisor);
  if (!force_fallback && !before.pidfd_selected)
  {
    DriverChecks checks;
    bool eof = false;
    cleanup_monitor_children(supervisor, children, options.records, checks, eof);
    if (!checks.shutdown_complete || !checks.live_records_zero || !checks.records_finished || !checks.settlement_once || !checks.immediate_child_guard || !eof)
      fail("pidfd-unavailable probe did not complete exact cleanup");
    emit_helper_unsupported(options.benchmark_case, "cpu_ns_per_wall_second", "cpu_ns_per_wall_second", "pidfd_unavailable", kPidfdUnavailableReason,
                            {{"monitor_backend", std::string("automatic")}, {"record_count", static_cast<std::uint64_t>(options.records)}});
    return;
  }

  auto const cpu_before = process_cpu_nanoseconds();
  auto const wall_before = Clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(options.hold_milliseconds));
  auto const wall_after = Clock::now();
  auto const cpu_after = process_cpu_nanoseconds();
  auto const after = SupervisorTestAccess::monitor_snapshot(*supervisor);
  auto const wall_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall_after - wall_before).count());
  auto const cpu_ns = cpu_after - cpu_before;
  auto const normalized = wall_ns == 0 ? 0.0 : static_cast<double>(cpu_ns) * 1'000'000'000.0 / static_cast<double>(wall_ns);

  bool logarithmic_buckets = true;
  if (force_fallback)
  {
    for (std::size_t bucket = 0; bucket < 7; ++bucket)
      logarithmic_buckets = logarithmic_buckets && before.counters.fallback_delay_buckets[bucket] >= options.records;
  }
  bool const one_second_cap = !force_fallback || fallback_stabilized(before, options.records);
  bool const backend_exact = force_fallback ? (!after.pidfd_selected && after.backend_mode == ProcessMonitorBackendMode::ForceFallbackUnavailable)
                                            : (after.pidfd_selected && after.backend_mode == ProcessMonitorBackendMode::Automatic);
  bool const zero_periodic_fallback = force_fallback || (after.counters.fallback_probes == 0 && after.counters.exact_probes == 0);

  DriverChecks checks;
  bool endpoints_eof = false;
  cleanup_monitor_children(supervisor, children, options.records, checks, endpoints_eof);
  auto fields = driver_check_fields(checks);
  fields.emplace_back("children_ready", ready);
  fields.emplace_back("endpoint_eof", endpoints_eof);
  fields.emplace_back("backend_exact", backend_exact);
  fields.emplace_back("zero_periodic_fallback", zero_periodic_fallback);
  fields.emplace_back("logarithmic_buckets", logarithmic_buckets);
  fields.emplace_back("one_second_cap", one_second_cap);
  emit_helper_measurement(options.benchmark_case, "cpu_ns_per_wall_second", "cpu_ns_per_wall_second",
                          {{.ordinal = 1,
                            .value = normalized,
                            .metrics = {{"cpu_time_ns", cpu_ns},
                                        {"wall_time_ns", wall_ns},
                                        {"poll_calls_delta", signed_delta(after.counters.poll_calls, before.counters.poll_calls)},
                                        {"poll_timeouts_delta", signed_delta(after.counters.poll_timeouts, before.counters.poll_timeouts)},
                                        {"fallback_probes_delta", signed_delta(after.counters.fallback_probes, before.counters.fallback_probes)},
                                        {"exact_probes_delta", signed_delta(after.counters.exact_probes, before.counters.exact_probes)},
                                        {"monitor_cycles_delta", signed_delta(after.counters.monitor_cycles, before.counters.monitor_cycles)}},
                            .checks = std::move(fields)}},
                          {{"monitor_backend", std::string(force_fallback ? "posix_fallback" : "automatic_pidfd")},
                           {"process_backend", std::string("event_driven_posix")},
                           {"authority", std::string("neutral_supervisor")},
                           {"record_count", static_cast<std::uint64_t>(options.records)},
                           {"hold_milliseconds", static_cast<std::uint64_t>(options.hold_milliseconds)}});
}

#endif

void emit_capabilities(BackendBenchmarkOptions const& options)
{
  JsonFields metrics;
#if defined(AVA_BENCHMARK_HAS_PROCESS_SUPERVISOR)
  bool const process_present = true;
  bool const platform_present = ava::process::platform_support_v1() == ava::process::PlatformSupportV1::Posix;
  metrics.emplace_back("process_supervisor", process_present);
  metrics.emplace_back("process_fixture", executable_file(options.fake_process_child));
  metrics.emplace_back("platform_backend", std::string(platform_present ? "posix" : "unsupported"));
  metrics.emplace_back("process_backend", std::string(platform_present ? "event_driven_posix" : "unsupported"));
#else
  metrics.emplace_back("process_supervisor", false);
  metrics.emplace_back("process_fixture", false);
  metrics.emplace_back("platform_backend", std::string("unsupported"));
  metrics.emplace_back("process_backend", std::string("unsupported"));
#endif
  metrics.emplace_back("helper_contract", std::string("m1_process_supervision_v1"));
  append_family_authorities(metrics);
  emit_helper_measurement(options.benchmark_case, "capability_probe", "count",
                          {{.ordinal = 1, .value = 1.0, .metrics = {}, .checks = {{"metadata_closed", true}}}}, metrics);
}

}  // namespace

bool is_process_benchmark_case(std::string_view const benchmark_case) noexcept
{
  return benchmark_case == "process-capabilities" || benchmark_case == "process-idle-scope" || benchmark_case == "process-first-spawn" ||
         benchmark_case == "process-warm-sequential" || benchmark_case == "process-concurrent" || benchmark_case == "process-natural-exit" ||
         benchmark_case == "process-leader-first-descendant" || benchmark_case == "process-term-refusal" || benchmark_case == "process-shared-shutdown" ||
         benchmark_case == "process-monitor-pidfd" || benchmark_case == "process-monitor-fallback";
}

void run_process_benchmark(BackendBenchmarkOptions const& options)
{
  if (options.benchmark_case == "process-capabilities")
  {
    emit_capabilities(options);
    return;
  }
#if !defined(AVA_BENCHMARK_HAS_PROCESS_SUPERVISOR)
  emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "source_architecture_absent", kSourceArchitectureAbsentReason,
                          {{"process_backend", std::string("unsupported")}, {"authority", std::string("neutral_supervisor")}});
#else
  if (ava::process::platform_support_v1() != ava::process::PlatformSupportV1::Posix)
  {
    emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "platform_unsupported", kPlatformUnsupportedReason,
                            {{"process_backend", std::string("unsupported")}, {"authority", std::string("neutral_supervisor")}});
    return;
  }
  if (options.benchmark_case == "process-idle-scope")
    benchmark_idle_scope(options);
  else if (options.benchmark_case == "process-first-spawn")
    benchmark_first_spawn(options);
  else if (options.benchmark_case == "process-warm-sequential")
    benchmark_warm_sequential(options);
  else if (options.benchmark_case == "process-concurrent")
    benchmark_concurrent(options);
  else if (options.benchmark_case == "process-natural-exit")
    benchmark_natural_exit(options);
  else if (options.benchmark_case == "process-leader-first-descendant")
    benchmark_leader_first(options);
  else if (options.benchmark_case == "process-term-refusal")
    benchmark_term_refusal(options);
  else if (options.benchmark_case == "process-shared-shutdown")
    benchmark_shared_shutdown(options);
  else if (options.benchmark_case == "process-monitor-pidfd")
    benchmark_monitor(options, false);
  else if (options.benchmark_case == "process-monitor-fallback")
    benchmark_monitor(options, true);
  else
    fail("unknown process benchmark case");
#endif
}

}  // namespace ava::benchmark
