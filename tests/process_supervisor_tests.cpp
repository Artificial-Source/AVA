#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/process/environment_test_support.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

ava::process::OwnerPathV1 make_application_owner()
{
  auto owner = ava::process::OwnerPathV1::application();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::process::OwnerPathV1 make_operation(ava::process::OwnerPathV1 const& prefix)
{
  auto owner = prefix.operation();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::process::SpawnSpecV1 invalid_spawn_spec(std::string canary = {})
{
  return {.executable = "/bin/true", .argv = {"/bin/true", std::move(canary), "DUPLICATE"}, .environment = {}, .cwd = "/"};
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
           << record.monotonic_milliseconds << ' ' << record.stdout_bytes << ' ' << record.stderr_bytes << ' ' << record.settlement_count;
    if (record.reason)
      output << ' ' << ava::process::to_string(*record.reason);
  }
  return output.str();
}

void test_closed_vocabulary()
{
  using namespace ava::process;
  expect(is_valid(ProcessRoleV1::Curl) && is_valid(ProcessRoleV1::ExternalEditor) && !is_valid(static_cast<ProcessRoleV1>(999)),
         "process roles form a validated closed version-1 vocabulary");
  expect(is_valid(ProcessStateV1::Reserved) && is_valid(ProcessStateV1::Finished) && !is_valid(static_cast<ProcessStateV1>(999)),
         "process states reject unknown serialized values");
  expect(is_valid(TerminationReasonV1::NaturalExit) && is_valid(TerminationReasonV1::UnsupportedSuspension) && !is_valid(static_cast<TerminationReasonV1>(999)),
         "process termination reasons reject unknown serialized values");
  expect(is_valid(CleanupStateV1::Incomplete) && !is_valid(static_cast<CleanupStateV1>(999)) && is_valid(ExitKindV1::CleanupIncomplete) &&
             !is_valid(static_cast<ExitKindV1>(999)) && is_valid(StreamModeV1::Discard) && !is_valid(static_cast<StreamModeV1>(999)) &&
             is_valid(BashContainmentHandshakeV1::Required) && !is_valid(static_cast<BashContainmentHandshakeV1>(999)) &&
             is_valid(AdoptionChildFailureStageV1::DescriptorValidation) && !is_valid(static_cast<AdoptionChildFailureStageV1>(999)),
         "cleanup, exit, stream, containment, and adoption child-stage values are closed and validated");
  expect(to_string(ProcessRoleV1::Mcp) == "mcp" && to_string(TerminationReasonV1::DeadlineExpired) == "deadline_expired" &&
             to_string(static_cast<ProcessRoleV1>(999)) == "invalid",
         "closed process vocabulary has bounded canonical names and an invalid sentinel");
}

void test_generated_owner_hierarchy()
{
  auto application = make_application_owner();
  auto other_application = make_application_owner();
  auto session = application.session();
  auto run = session ? session->run() : ava::core::Result<ava::process::OwnerPathV1>(std::unexpected(session.error()));
  auto operation = run ? run->operation() : ava::core::Result<ava::process::OwnerPathV1>(std::unexpected(run.error()));
  auto application_operation = application.operation();
  auto invalid_run = application.run();
  auto below_operation =
      application_operation ? application_operation->session() : ava::core::Result<ava::process::OwnerPathV1>(std::unexpected(application_operation.error()));

  expect(application.is_valid_prefix() && !application.is_launch_owner() && application.depth() == 1 && application.schema_version() == 1,
         "a generated application owner is a valid non-launch prefix");
  expect(session && run && operation && operation->is_launch_owner() && operation->depth() == 4 && operation->encoded_size() <= 4 * 64 + 40,
         "session/run/operation derivation creates the bounded four-level owner hierarchy");
  expect(application_operation && application_operation->is_launch_owner() && application_operation->depth() == 2,
         "application-owned operations omit session and run");
  expect(!invalid_run && !below_operation, "run requires session and no owner can be derived below an operation");
  expect(operation && operation->matches_prefix(application) && operation->matches_prefix(*session) && operation->matches_prefix(*run) &&
             !operation->matches_prefix(other_application),
         "owner prefix matching selects descendants without conflating independently generated applications");
}

bool same_owner(ava::process::OwnerPathV1 const& left, ava::process::OwnerPathV1 const& right)
{
  return left.matches_prefix(right) && right.matches_prefix(left);
}

void test_process_scope_hierarchy_and_inert_copying()
{
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto const initial = supervisor->snapshot();
  auto null_scope = ava::process::ProcessScopeV1::application({});
  auto application = ava::process::ProcessScopeV1::application(supervisor);
  expect(!null_scope && application.has_value(), "application process scope rejects null authority and accepts one explicit supervisor");
  if (!application)
    return;

  auto first_session = application->session();
  auto second_session = application->session();
  auto invalid_application_run = application->run();
  auto application_operation = application->operation();
  expect(first_session && second_session && !invalid_application_run && application_operation,
         "process scope permits generated application children but requires a session before a run");
  if (!first_session || !second_session || !application_operation)
    return;

  auto run = first_session->run();
  auto invalid_second_session = first_session->session();
  auto session_operation = first_session->operation();
  expect(run && !invalid_second_session && session_operation, "session process scope derives one run or operation and rejects another session level");
  if (!run || !session_operation)
    return;

  auto operation = run->operation();
  auto invalid_second_run = run->run();
  auto invalid_below_operation = operation ? operation->operation() : run->operation();
  expect(operation && !invalid_second_run && !invalid_below_operation,
         "run process scope derives an operation while repeated and below-operation derivations fail explicitly");
  if (!operation)
    return;

  auto recovered_application = operation->application_scope();
  auto copied_session = *first_session;
  auto fresh_session = recovered_application.session();
  bool const distinct_siblings = fresh_session && !same_owner(first_session->owner_prefix(), second_session->owner_prefix()) &&
                                 !same_owner(first_session->owner_prefix(), fresh_session->owner_prefix());
  bool const same_authority = &application->supervisor() == supervisor.get() && &first_session->supervisor() == supervisor.get() &&
                              &run->supervisor() == supervisor.get() && &operation->supervisor() == supervisor.get() &&
                              &recovered_application.supervisor() == supervisor.get();
  bool const shared_host_capture =
      ava::process::testing::EnvironmentTestAccess::shares_capture(application->host_environment(), operation->host_environment()) &&
      ava::process::testing::EnvironmentTestAccess::shares_capture(application->host_environment(), recovered_application.host_environment());
  bool const recovered_root = same_owner(application->owner_prefix(), recovered_application.owner_prefix()) &&
                              operation->owner_prefix().matches_prefix(recovered_application.owner_prefix());
  expect(fresh_session && distinct_siblings, "generated sibling session scopes are distinct under one recovered application root");
  expect(same_authority && shared_host_capture && recovered_root && same_owner(copied_session.owner_prefix(), first_session->owner_prefix()),
         "derived and copied process scopes retain one supervisor and one immutable host capture while recovering the application owner");
#ifdef CWDEBUG
  std::ostringstream debug_output;
  debug_output << *application;
  expect(debug_output.str() == "$process_scope$", "process scope debug output is a fixed content-free token");
#endif

  auto const final = supervisor->snapshot();
  expect(!initial.monitor_started && initial.live_records == 0 && initial.records.empty() && !final.monitor_started && final.live_records == 0 &&
             final.records.empty(),
         "process scope construction, derivation, root recovery, and copying create no monitor or live process record");
}

void test_startup_timeout_policy_validation()
{
  ava::process::Supervisor supervisor;
  auto application = make_application_owner();
  auto zero =
      supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Plugin, {.startup_timeout = 0ms, .execution_deadline = std::nullopt});
  auto negative =
      supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Plugin, {.startup_timeout = -1ms, .execution_deadline = std::nullopt});
  auto excessive =
      supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Plugin, {.startup_timeout = 1h, .execution_deadline = std::nullopt});
  bool accepted_bounded = false;
  {
    auto bounded =
        supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Plugin, {.startup_timeout = 1ms, .execution_deadline = std::nullopt});
    accepted_bounded = bounded.has_value();
  }
  auto snapshot = supervisor.snapshot();
  expect(ava::process::LifecyclePolicyV1{}.startup_timeout == 2s && !zero && !negative && !excessive && accepted_bounded && snapshot.live_records == 0 &&
             !snapshot.monitor_started,
         "startup timeout defaults to two seconds and rejects zero, negative, and excessive policies before fork");
}

void test_lazy_monitor_and_live_capacity()
{
  ava::process::Supervisor supervisor;
  auto application = make_application_owner();
  auto initial = supervisor.snapshot();
  expect(!initial.monitor_started && initial.live_records == 0 && initial.records.empty(), "an idle supervisor creates neither a monitor nor process records");

  std::vector<ava::process::Reservation> reservations;
  reservations.reserve(ava::process::kMaxLiveProcessRecordsV1);
  bool filled = true;
  for (std::size_t index = 0; index < ava::process::kMaxLiveProcessRecordsV1; ++index)
  {
    auto reservation = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Plugin);
    filled = filled && reservation.has_value();
    if (reservation)
      reservations.push_back(std::move(*reservation));
  }
  auto rejected = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Plugin);
  auto full = supervisor.snapshot();
  expect(filled && reservations.size() == ava::process::kMaxLiveProcessRecordsV1 && !rejected && full.live_records == 256 && !full.monitor_started,
         "reservation consumes the complete 256-record live bound before any fork or monitor startup");

  reservations.clear();
  auto released = supervisor.snapshot();
  expect(released.live_records == 0 && released.records.empty() && !released.monitor_started,
         "abandoned pre-fork reservations release capacity without retaining terminal history");
}

void test_terminal_pruning_and_content_free_snapshot()
{
  ava::process::Supervisor supervisor;
  auto application = make_application_owner();
  constexpr std::string_view canary = "AVA_ARGV_ENV_CWD_CONTENT_CANARY_7db91";
  bool all_failed_before_fork = true;
  for (std::size_t index = 0; index < ava::process::kMaxRetainedProcessRecordsV1 + 44; ++index)
  {
    auto reservation = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Mcp);
    if (!reservation)
    {
      all_failed_before_fork = false;
      break;
    }
    auto result = supervisor.spawn(std::move(*reservation), invalid_spawn_spec(std::string(canary)));
    all_failed_before_fork = all_failed_before_fork && !result;
  }
  auto snapshot = supervisor.snapshot();
  bool settled_once = true;
  for (auto const& record : snapshot.records)
    settled_once = settled_once && record.state == ava::process::ProcessStateV1::Finished && record.settlement_count == 1 &&
                   record.reason == ava::process::TerminationReasonV1::LaunchFailed;
  auto const text = snapshot_text(snapshot);
  expect(all_failed_before_fork && !snapshot.monitor_started && snapshot.live_records == 0 &&
             snapshot.retained_terminal_records == ava::process::kMaxRetainedProcessRecordsV1 &&
             snapshot.records.size() == ava::process::kMaxRetainedProcessRecordsV1,
         "pre-fork terminal records are pruned FIFO to the 256-record retention bound without starting the monitor");
  expect(settled_once, "every retained launch failure settles exactly once");
  expect(text.find(canary) == std::string::npos && text.find("/bin/true") == std::string::npos && text.find("DUPLICATE") == std::string::npos,
         "content canaries from argv, executable, cwd, and environment are absent from process snapshots");
}

void test_pre_fork_nul_and_environment_capability_validation()
{
  ava::process::Supervisor supervisor;
  auto application = make_application_owner();
  auto reservation = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Plugin);
  auto environment = ava::process::make_plugin_environment_v1("/");
  ava::process::SpawnSpecV1 specification{.executable = "/bin/true", .argv = {"/bin/true", std::string("bad\0argument", 12)}, .environment = {}, .cwd = "/"};
  if (environment)
    specification.environment = std::move(*environment);
  auto result = reservation ? supervisor.spawn(std::move(*reservation), std::move(specification))
                            : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(reservation.error()));
  auto environment_reservation = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Mcp);
  auto environment_result = environment_reservation ? supervisor.spawn(std::move(*environment_reservation),
                                                                       {.executable = "/bin/true", .argv = {"/bin/true"}, .environment = {}, .cwd = "/"})
                                                    : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(environment_reservation.error()));

  constexpr std::string_view name_canary = "AVA_EXACT_ENV_NAME_CANARY_47f9";
  constexpr std::string_view value_canary = "exact-env-value-canary-91c2";
  auto canary_environment = ava::process::make_mcp_environment_v1("/", {{std::string(name_canary), std::string(value_canary)}});
  auto canary_reservation = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Mcp);
  ava::process::SpawnSpecV1 canary_specification{
      .executable = "/bin/true", .argv = {"/bin/true", std::string("bad\0argument", 12)}, .environment = {}, .cwd = "/"};
  if (canary_environment)
    canary_specification.environment = std::move(*canary_environment);
  auto canary_result = canary_reservation ? supervisor.spawn(std::move(*canary_reservation), std::move(canary_specification))
                                          : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(canary_reservation.error()));

  auto snapshot = supervisor.snapshot();
  auto const snapshot_output = snapshot_text(snapshot);
  bool settled_once = snapshot.records.size() == 3;
  for (auto const& record : snapshot.records)
    settled_once = settled_once && record.reason == ava::process::TerminationReasonV1::LaunchFailed &&
                   record.exit_kind == ava::process::ExitKindV1::LaunchError && record.settlement_count == 1;
  expect(environment && canary_environment && !result && !environment_result && !canary_result && settled_once && !snapshot.monitor_started &&
             snapshot.live_records == 0,
         "argv NUL and omitted exact-environment authority reject before fork with one launch-error settlement each");
  expect(snapshot_output.find(name_canary) == std::string::npos && snapshot_output.find(value_canary) == std::string::npos,
         "exact-environment name and value canaries are absent from content-free process snapshots");
}

void test_stop_accepting_shutdown_and_idempotence()
{
  ava::process::Supervisor supervisor;
  auto application = make_application_owner();
  auto first = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Curl);
  expect(first.has_value(), "supervisor accepts a reservation before stop_accepting");
  supervisor.stop_accepting();
  supervisor.stop_accepting();
  auto rejected = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::Curl);
  auto shutdown = supervisor.shutdown(ava::tests::now_plus_seconds(10));
  auto repeated = supervisor.shutdown(ava::tests::now_plus_seconds(10));
  auto snapshot = supervisor.snapshot();
  expect(!rejected && shutdown.complete && repeated.complete && shutdown.incomplete_count == repeated.incomplete_count && !snapshot.accepting &&
             snapshot.live_records == 0,
         "stop_accepting and bounded shutdown are idempotent and settle reserved work without a child");
}

void test_concurrent_reservation_stop_race()
{
  ava::process::Supervisor supervisor;
  auto application = make_application_owner();
  std::vector<ava::process::OwnerPathV1> owners;
  owners.reserve(320);
  for (int index = 0; index < 320; ++index)
    owners.push_back(make_operation(application));

  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> accepted{0};
  std::vector<std::vector<ava::process::Reservation>> reservations(8);
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 8; ++worker)
  {
    workers.emplace_back([&, worker] {
      while (true)
      {
        auto const index = next.fetch_add(1);
        if (index >= owners.size())
          return;
        auto reservation = supervisor.reserve(owners[index], ava::process::ProcessRoleV1::ClipboardHelper);
        if (reservation)
        {
          reservations[worker].push_back(std::move(*reservation));
          ++accepted;
          std::this_thread::yield();
        }
      }
    });
  }
  supervisor.stop_accepting();
  for (auto& worker : workers)
    worker.join();
  auto snapshot = supervisor.snapshot();
  expect(
      accepted.load() <= ava::process::kMaxLiveProcessRecordsV1 && snapshot.live_records == accepted.load() && !snapshot.accepting && !snapshot.monitor_started,
      "concurrent reservation versus stop_accepting never exceeds live capacity");
  for (auto& worker_reservations : reservations)
    worker_reservations.clear();
  snapshot = supervisor.snapshot();
  expect(snapshot.live_records == 0 && !snapshot.accepting && !snapshot.monitor_started,
         "concurrent reservation versus stop_accepting abandoned winners release cleanly");
}

void test_reservation_shutdown_race()
{
  ava::process::Supervisor supervisor;
  auto application = make_application_owner();
  auto reservation = supervisor.reserve(make_operation(application), ava::process::ProcessRoleV1::ExternalEditor);
  expect(reservation.has_value(), "reservation/shutdown race obtains its pre-fork capacity ticket");
  if (!reservation)
    return;

  std::atomic<bool> start{false};
  std::thread launcher([&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    static_cast<void>(supervisor.spawn(std::move(*reservation), invalid_spawn_spec()));
  });
  start.store(true, std::memory_order_release);
  auto result = supervisor.shutdown(ava::tests::now_plus_seconds(500ms));
  launcher.join();
  auto snapshot = supervisor.snapshot();
  bool settled_once = true;
  for (auto const& record : snapshot.records)
    settled_once = settled_once && record.settlement_count == 1;
  expect(result.complete && snapshot.live_records == 0 && settled_once,
         "reservation versus shutdown races settle once without a fork, capacity leak, or per-record grace multiplication");
}

}  // namespace

void run_process_supervisor_tests()
{
  test_closed_vocabulary();
  test_generated_owner_hierarchy();
  test_process_scope_hierarchy_and_inert_copying();
  test_startup_timeout_policy_validation();
  test_lazy_monitor_and_live_capacity();
  test_terminal_pruning_and_content_free_snapshot();
  test_pre_fork_nul_and_environment_capability_validation();
  test_stop_accepting_shutdown_and_idempotence();
  test_concurrent_reservation_stop_race();
  test_reservation_shutdown_race();
}
