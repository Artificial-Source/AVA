#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/observability/run_observer.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/session_run_controller.h"
#include "ava/session/compaction.h"
#include "ava/session/session_store.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace ava::app {

// A waiter count cannot keep the original waiter paused across historical
// eviction. Keep scheduling control local to the test instead of controller state.
struct SessionRunControllerTestAccess
{
  static ava::core::Result<RunOutcome> wait_outcome(SessionRunController& controller, std::string_view correlation_id, std::function<void()> const& before_wait)
  {
    return controller.wait_outcome(correlation_id, &before_wait);
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::app

namespace {

struct PausedWaiter
{
  explicit PausedWaiter(ava::app::SessionRunController& controller, std::string_view correlation_id)
      : thread_([this, &controller, correlation_id = std::string(correlation_id)] {
          std::function<void()> const before_wait = [this] {
            std::unique_lock lock(mutex_);
            registered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
          };
          auto outcome = ava::app::SessionRunControllerTestAccess::wait_outcome(controller, correlation_id, before_wait);
          std::lock_guard lock(mutex_);
          joined.emplace(std::move(outcome));
          returned_ = true;
          changed_.notify_all();
        })
  {
  }

  ~PausedWaiter() { release(); }

  [[nodiscard]] bool wait_until_registered()
  {
    std::unique_lock lock(mutex_);
    auto const observed = changed_.wait_until(lock, ava::tests::now_plus_seconds(3), [this] { return registered_ || returned_; });
    return observed && registered_ && !returned_;
  }

  void release()
  {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

  [[nodiscard]] bool wait_until_returned()
  {
    std::unique_lock lock(mutex_);
    return changed_.wait_until(lock, ava::tests::now_plus_seconds(3), [this] { return returned_; });
  }

  void join() { thread_.join(); }

  std::optional<ava::core::Result<ava::app::RunOutcome>> joined;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool registered_ = false;
  bool released_ = false;
  bool returned_ = false;
  std::jthread thread_;
};

class CallbackRunObserver final : public ava::observability::RunObserver
{
 public:
  explicit CallbackRunObserver(std::function<void(ava::observability::TraceEvent const&)> callback) : callback_(std::move(callback)) { }

  void on_event(ava::observability::TraceEvent const& event) override { callback_(event); }

 private:
  std::function<void(ava::observability::TraceEvent const&)> callback_;
};

std::shared_ptr<ava::session::SessionAppendTarget> ephemeral_controller_target()
{
  auto const root = create_empty_root("ephemeral_controller_target");
  auto store = ava::session::SessionStore::create_ephemeral(root / "controller-ephemeral-workspace");
  if (!store)
    return {};
  auto target = ava::session::SessionAppendTarget::create_ephemeral(*store);
  return target ? std::move(*target) : std::shared_ptr<ava::session::SessionAppendTarget>{};
}

ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>> persistent_controller_target(ava::session::SessionStore const& store,
                                                                                                   ava::session::SessionLease const& lease)
{
  return ava::session::SessionAppendTarget::create_persistent(store, lease, ava::session::SessionReadLimits{});
}

void test_session_run_controller_transitions_and_guard_release()
{
  ava::app::SessionRunController controller(ephemeral_controller_target());
  auto admitted = controller.admit({.request_id = "run-1"});
  expect(admitted.has_value(), "controller admits one run");
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  expect(guard.transition(ava::app::RunPhase::BuildingContext).has_value(), "controller permits admitted to building transition");
  expect(!guard.transition(ava::app::RunPhase::ExecutingTools).has_value(), "controller rejects skipped phase transition");
  expect(guard.transition(ava::app::RunPhase::AwaitingProvider).has_value(), "controller permits building to provider transition");
  expect(guard.transition(ava::app::RunPhase::Completing).has_value(), "controller permits provider completion transition");
  expect(guard.complete({.run_id = {}, .reason = ava::app::StopReason::Completed}).has_value(), "controller records one terminal outcome");
  expect(!guard.complete({.run_id = {}, .reason = ava::app::StopReason::Completed}).has_value(), "controller rejects a second terminal outcome");
  expect(!controller.snapshot().active, "completed guard releases active state");

  auto error_guard = controller.admit({.request_id = "run-2"});
  expect(error_guard.has_value(), "controller can admit after completed run");
  if (error_guard)
  {
    auto moved = std::move(*error_guard);
    auto moved_again = std::move(moved);
    expect(moved_again.active(), "move-only guard retains active ownership after move");
  }
  auto snapshot = controller.snapshot();
  expect(!snapshot.active && snapshot.outcome && snapshot.outcome->reason == ava::app::StopReason::ProviderError,
         "guard destructor releases an errored run exactly once");
}

void test_session_run_controller_concurrent_admit_wake_stop()
{
  ava::app::SessionRunController controller(ephemeral_controller_target());
  auto admitted = controller.admit({.request_id = "run"});
  expect(admitted.has_value(), "controller admits concurrency fixture run");
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  std::atomic<int> accepted = 0;
  std::vector<ava::core::JoinThread> threads;
  for (int index = 0; index < 8; ++index)
  {
    threads.emplace_back(ava::core::JoinThread::create("wake", [&] {
      if (controller.wake({.kind = ava::app::RunCommand::Kind::Steering, .correlation_id = "run", .message = "safe point"}))
        ++accepted;
    }));
  }
  for (auto& thread : threads)
    thread.join();
  expect(accepted == 8 && controller.snapshot().queued_commands == 8, "concurrent wake calls retain ordered bounded commands");
  auto stopped = controller.request_stop();
  expect(stopped && *stopped && guard.stop_requested(), "stop interrupts the active guard and reports accepted arbitration");
  expect(guard.complete({.run_id = {}, .reason = ava::app::StopReason::UserCanceled}).has_value(), "stopped run has one canceled outcome");
  expect(!controller.wake({.kind = ava::app::RunCommand::Kind::Wake, .correlation_id = {}, .message = "late"}).has_value(),
         "wake rejects after terminal release");
}

void test_session_run_controller_generation_and_cancel_precedence()
{
  ava::app::SessionRunController controller(ephemeral_controller_target());
  auto first = controller.admit({.request_id = "A"});
  expect(first.has_value(), "controller admits first generation");
  if (!first)
    return;
  auto stale = std::move(*first);
  expect(stale.transition(ava::app::RunPhase::BuildingContext).has_value(), "first generation enters context");
  expect(stale.transition(ava::app::RunPhase::AwaitingProvider).has_value(), "first generation enters provider");
  expect(stale.transition(ava::app::RunPhase::Completing).has_value(), "first generation completes work");
  auto late_stop = controller.request_stop();
  expect(late_stop && !*late_stop && !stale.stop_requested(), "late cancellation is rejected after Completing");
  expect(stale.complete({.run_id = {}, .reason = ava::app::StopReason::Completed}).has_value(), "completion wins over cancellation after Completing");
  auto second = controller.admit({.request_id = "B"});
  expect(second.has_value(), "second generation admits after A completion");
  if (!second)
    return;
  auto current = std::move(*second);
  expect(!stale.transition(ava::app::RunPhase::BuildingContext).has_value(), "stale A guard cannot transition B");
  expect(!stale.complete({.run_id = {}, .reason = ava::app::StopReason::Completed}).has_value(), "stale A guard cannot complete B");
  expect(current.active(), "B remains owned after stale A operations");

  auto before_completing_stop = controller.request_stop();
  expect(before_completing_stop && *before_completing_stop, "cancellation before Completing wins controller arbitration");
  auto canceled = current.complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError});
  expect(canceled && canceled->reason == ava::app::StopReason::UserCanceled && !canceled->error,
         "an accepted stop remains authoritative over a racing provider terminal error");
}

void test_session_run_controller_committed_append_generation_rejects_stale_publication()
{
  ava::app::SessionRunController controller(ephemeral_controller_target());
  bool published = false;
  auto const initial_identity = controller.statistics_mutation_identity();
  expect(initial_identity.committed_append_generation == 0 && initial_identity.append_order_generation == 0 &&
             controller.publish_if_statistics_mutation_current(initial_identity, [&] { published = true; }) && published,
         "controller accepts publication at its exact initial append generation");

  auto appended = controller.append(ava::session::SessionEntry{.id = "statistics-generation-entry",
                                                               .parent_id = "",
                                                               .type = ava::session::EntryType::UserMessage,
                                                               .timestamp = "2026-10-22T00:00:00Z",
                                                               .data_json = "{\"text\":\"committed\"}"});
  bool stale_published = false;
  bool current_published = false;
  auto const current_identity = controller.statistics_mutation_identity();
  auto const stale_accepted = controller.publish_if_statistics_mutation_current(initial_identity, [&] { stale_published = true; });
  auto const current_accepted = controller.publish_if_statistics_mutation_current(current_identity, [&] { current_published = true; });
  expect(appended && current_identity.committed_append_generation == 1 && current_identity.append_order_generation == 2 && !stale_accepted &&
             !stale_published && current_accepted && current_published,
         "actual append advances mutation identity and serialized validation rejects stale statistics work");
}

ava::session::SessionEntry append_entry(std::string id)
{
  return ava::session::SessionEntry{
      .id = std::move(id), .parent_id = "", .type = ava::session::EntryType::Error, .timestamp = ava::session::now_timestamp(), .data_json = "{\"test\":true}"};
}

ava::session::SessionEntry compaction_entry(std::string id)
{
  auto entry = ava::session::make_manual_compaction_entry(ava::session::ManualCompactionRequest{.summary = "summary",
                                                                                                .instructions = "",
                                                                                                .config = ava::session::default_compaction_config(),
                                                                                                .estimated_tokens = 0,
                                                                                                .threshold_tokens = 0,
                                                                                                .retained_tokens = 0,
                                                                                                .trigger = "manual",
                                                                                                .recent_context = "",
                                                                                                .recent_context_omitted = false});
  if (!entry)
    return {};
  entry->id = std::move(id);
  return std::move(*entry);
}

// Mirrors the controller's AppendItem payload byte accounting exactly.
std::size_t controller_entry_bytes(ava::session::SessionEntry const& entry)
{
  return entry.id.size() + entry.parent_id.size() + entry.timestamp.size() + entry.data_json.size() + 32;
}

std::size_t controller_entries_bytes(std::vector<ava::session::SessionEntry> const& entries)
{
  std::size_t bytes = 0;
  for (auto const& entry : entries)
    bytes += controller_entry_bytes(entry);
  return bytes;
}

ava::session::SessionEntry branch_summary_entry(ava::session::SessionStore const& store, std::string id, std::string parent_id, std::string root_id,
                                                std::string tip_id)
{
  return ava::session::SessionEntry{.id = std::move(id),
                                    .parent_id = std::move(parent_id),
                                    .type = ava::session::EntryType::BranchSummary,
                                    .timestamp = ava::session::now_timestamp(),
                                    .data_json = "{\"schema_version\":1,\"summary\":\"summary\",\"source_session_id\":\"" + store.options_r()->session_id +
                                                 "\",\"branch_root_entry_id\":\"" + root_id + "\",\"branch_tip_entry_id\":\"" + tip_id +
                                                 "\",\"provider\":\"test\",\"model\":\"test\",\"reason\":\"test\"}"};
}

std::optional<std::string> append_error_context(ava::core::Error const& error, std::string_view key)
{
  auto const found = std::ranges::find_if(error.context(), [&](auto const& item) { return item.key == key; });
  return found == error.context().end() ? std::nullopt : std::optional<std::string>(found->value);
}

std::string read_binary_file(std::filesystem::path const& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_session_run_controller_owner_and_generation_routes()
{
  auto const root = create_empty_root("session-run-controller");

  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  expect(store.has_value(), "controller append fixture store creates");
  if (!store)
    return;

  auto lease = ava::session::SessionLease::create_and_acquire(store->session_path());
  expect(lease.has_value(), "controller route fixture acquires a lease");
  if (!lease)
    return;
  auto target = persistent_controller_target(*store, *lease);
  expect(target.has_value(), "controller route fixture creates append target");
  if (!target)
    return;
  ava::app::SessionRunController controller(std::move(*target));
  auto owner = controller.owner_append_route();
  expect(owner(append_entry("inactive-owner")).has_value(), "owner route persists while no run is active");
  auto first = controller.admit({.request_id = "A"});
  expect(first.has_value(), "controller admits append generation A");
  if (!first)
    return;
  auto guard = std::move(*first);
  auto active = guard.append_route();
  expect(active(append_entry("A-active")).has_value(), "generation route persists active A entry");
  expect(owner(append_entry("A-owner")).has_value(), "owner route serializes active parent notice");
  expect(guard.transition(ava::app::RunPhase::BuildingContext).has_value(), "A enters context before completion");
  expect(guard.transition(ava::app::RunPhase::AwaitingProvider).has_value(), "A enters provider before completion");
  expect(guard.transition(ava::app::RunPhase::Completing).has_value(), "A enters completion before completion");
  expect(guard.complete({.run_id = {}, .reason = ava::app::StopReason::Completed}).has_value(), "A completes before inactive owner append");
  expect(owner(append_entry("after-A-owner")).has_value(), "owner route remains valid after A completion");
  auto second = controller.admit({.request_id = "B"});
  expect(second.has_value(), "controller admits B after A");
  if (!second)
    return;
  auto b_guard = std::move(*second);
  expect(!active(append_entry("stale-A")).has_value(), "completed A route cannot append into B");
  expect(owner(append_entry("during-B-owner")).has_value(), "owner route persists notice during B");
  auto entries = store->load();
  expect(entries && entries->size() == 5, "owner and active routes preserve all non-stale entries in order");
  if (entries && entries->size() == 5)
    expect(entries->at(0).id == "inactive-owner" && entries->at(4).id == "during-B-owner", "owner append FIFO order is durable");
}

void test_runtime_session_destruction_does_not_own_background_coordinator()
{
  auto const root = create_empty_root("session-run-controller-teardown");
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "application coordinator creates for runtime teardown fixture");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  std::mutex mutex;
  std::condition_variable changed;
  bool worker_started = false;
  bool worker_canceled = false;
  {
    auto visible_session_coordinator = coordinator;
    auto started = visible_session_coordinator->start_background(
        "parent_teardown", {.title = "blocked", .description = "blocked", .child_session_id = "child_teardown"},
        [&](ava::agent::BackgroundJobContext const& context) {
          std::stop_callback wake_on_stop(context.stop_token, [&] { changed.notify_all(); });
          std::unique_lock lock(mutex);
          worker_started = true;
          changed.notify_all();
          changed.wait(lock, [&] { return context.stop_token.stop_requested(); });
          worker_canceled = true;
          changed.notify_all();
          return ava::agent::BackgroundJobCompletion{
              .state = ava::agent::BackgroundJobState::Canceled, .final_text = {}, .stop_reason = "canceled", .error = std::nullopt};
        });
    expect(started.has_value(), "application coordinator starts blocked worker");
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(1), [&] { return worker_started; }), "blocked worker starts before visible session destruction");
    visible_session_coordinator.reset();
  }
  {
    std::lock_guard lock(mutex);
    expect(!worker_canceled, "visible runtime session destruction does not cancel application-owned worker");
  }
  coordinator->shutdown();
  std::lock_guard lock(mutex);
  expect(worker_canceled, "application coordinator shutdown cancels and joins worker");
}

void test_session_run_controller_admission_inspection_and_join()
{
  ava::app::SessionRunController controller(ephemeral_controller_target());
  expect(controller.inspect_admission({.request_id = "same"}) == ava::app::AdmissionDisposition::Admit, "inactive request is admissible");
  auto admitted = controller.admit({.request_id = "same"});
  expect(admitted.has_value(), "same-correlation fixture admits");
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  expect(controller.inspect_admission({.request_id = "same"}) == ava::app::AdmissionDisposition::JoinExistingOutcome,
         "same correlation has explicit join disposition");
  expect(controller.inspect_admission({.request_id = "other"}) == ava::app::AdmissionDisposition::RejectDifferentRequest,
         "different correlation has bounded reject disposition");
  std::optional<ava::app::RunOutcome> joined;
  ava::core::JoinThread waiter = ava::core::JoinThread::create("waiter", [&] {
    auto outcome = controller.wait_outcome("same");
    if (outcome)
      joined = *outcome;
  });
  expect(controller.wake({.kind = ava::app::RunCommand::Kind::FollowUp, .correlation_id = "same", .message = "next"}).has_value(),
         "controller accepts a correlated follow-up command");
  auto commands = controller.take_commands("same", ava::app::RunCommand::Kind::FollowUp);
  expect(commands && commands->size() == 1 && commands->front().kind == ava::app::RunCommand::Kind::FollowUp,
         "follow-up has a non-dropping controller adapter");
  expect(guard.transition(ava::app::RunPhase::BuildingContext).has_value(), "same request enters context");
  expect(guard.transition(ava::app::RunPhase::AwaitingProvider).has_value(), "same request enters provider");
  expect(guard.transition(ava::app::RunPhase::Completing).has_value(), "same request enters completion");
  auto completed = guard.complete({.run_id = {}, .reason = ava::app::StopReason::Completed});
  expect(completed.has_value(), "same request completes for join waiter");
  if (!completed)
    guard = {};
  waiter.join();
  expect(joined && joined->reason == ava::app::StopReason::Completed, "same request waiter receives correlated terminal outcome");
}

void test_session_run_controller_stable_join_and_command_kinds()
{
  ava::app::SessionRunController controller(ephemeral_controller_target());
  auto a = controller.admit({.request_id = "A"});
  expect(a.has_value(), "stable join fixture admits A");
  if (!a)
    return;
  auto guard_a = std::move(*a);
  std::optional<ava::app::RunOutcome> joined;
  ava::core::JoinThread waiter = ava::core::JoinThread::create("waiter", [&] {
    auto outcome = controller.wait_outcome("A");
    if (outcome)
      joined = *outcome;
  });
  expect(guard_a.transition(ava::app::RunPhase::BuildingContext).has_value(), "A enters context");
  expect(guard_a.transition(ava::app::RunPhase::AwaitingProvider).has_value(), "A enters provider");
  expect(guard_a.transition(ava::app::RunPhase::Completing).has_value(), "A enters completion");
  expect(guard_a.complete({.run_id = {}, .reason = ava::app::StopReason::Completed}).has_value(), "A completes");
  auto b = controller.admit({.request_id = "B"});
  expect(b.has_value(), "B can admit before A waiter is scheduled");
  waiter.join();
  expect(joined && joined->run_id == "A" && joined->reason == ava::app::StopReason::Completed, "A waiter retains A terminal outcome across B admission");

  if (!b)
    return;
  auto guard_b = std::move(*b);
  expect(controller.wake({.kind = ava::app::RunCommand::Kind::Steering, .correlation_id = "B", .message = "steer"}).has_value(), "steering queues");
  expect(controller.wake({.kind = ava::app::RunCommand::Kind::FollowUp, .correlation_id = "B", .message = "follow"}).has_value(), "follow-up queues");
  expect(controller.wake({.kind = ava::app::RunCommand::Kind::Wake, .correlation_id = "B", .message = "wake"}).has_value(), "wake queues");
  auto steering = controller.take_commands("B");
  expect(steering && steering->size() == 1 && steering->front().kind == ava::app::RunCommand::Kind::Steering, "steering adapter consumes only steering");
  auto follow_up = controller.take_commands("B", ava::app::RunCommand::Kind::FollowUp);
  auto wake = controller.take_commands("B", ava::app::RunCommand::Kind::Wake);
  expect(follow_up && follow_up->size() == 1 && wake && wake->size() == 1, "follow-up and wake survive mixed steering consumption");
}

void test_session_run_controller_join_survives_outcome_eviction()
{
  for (bool const release_guard : {false, true})
  {
    ava::app::SessionRunController controller(ephemeral_controller_target());
    auto admitted = controller.admit({.request_id = "evicted"});
    expect(admitted.has_value(), "outcome eviction fixture admits the original generation");
    if (!admitted)
      return;
    auto guard = std::move(*admitted);
    PausedWaiter waiter(controller, "evicted");
    expect(waiter.wait_until_registered(), "original waiter captures its generation before completion");

    if (release_guard)
      guard = {};
    else
      expect(guard.complete({.run_id = {}, .reason = ava::app::StopReason::NoProgress}).has_value(), "original generation explicitly completes");
    for (std::size_t index = 0; index <= ava::app::kMaxRetainedRunOutcomes; ++index)
    {
      auto newer = controller.admit({.request_id = "newer-" + std::to_string(index)});
      expect(newer.has_value(), "newer generation admits while original waiter is paused");
      if (newer)
        expect(newer->complete({.run_id = {}, .reason = ava::app::StopReason::MaxTurns}).has_value(), "newer generation publishes a retained outcome");
    }
    expect(!controller.wait_outcome("evicted"), "historical lookup evicts the original outcome even while its waiter retains completion");
    auto reused = controller.admit({.request_id = "evicted"});
    expect(reused.has_value(), "evicted request id admits a distinct active generation");
    waiter.release();
    bool const returned_before_reused_completion = waiter.wait_until_returned();
    // Completing the reused generation also releases a regressed map-based
    // waiter, so a failed assertion cannot strand the test's joining thread.
    if (reused)
      expect(reused->complete({.run_id = {}, .reason = ava::app::StopReason::MaxToolCalls}).has_value(), "reused generation completes independently");
    waiter.join();
    auto const& joined = waiter.joined;
    auto const expected_reason = release_guard ? ava::app::StopReason::ProviderError : ava::app::StopReason::NoProgress;
    expect(returned_before_reused_completion && joined && *joined && (*joined)->run_id == "evicted" && (*joined)->reason == expected_reason &&
               (*joined)->error.has_value() == release_guard,
           "explicit and guard-release completion survive historical eviction and never join a reused request id's generation");
  }
}

void test_session_run_controller_reused_id_joins_active_generation()
{
  for (bool const evict_previous : {false, true})
  {
    ava::app::SessionRunController controller(ephemeral_controller_target());
    auto previous = controller.admit({.request_id = "reused"});
    expect(previous.has_value(), "reused-id fixture admits the previous generation");
    if (!previous)
      return;
    expect(previous->complete({.run_id = {}, .reason = ava::app::StopReason::NoProgress}).has_value(), "previous generation publishes its outcome");
    if (evict_previous)
    {
      for (std::size_t index = 0; index <= ava::app::kMaxRetainedRunOutcomes; ++index)
      {
        auto newer = controller.admit({.request_id = "newer-" + std::to_string(index)});
        expect(newer.has_value(), "reused-id eviction fixture admits newer generation");
        if (newer)
          expect(newer->complete({.run_id = {}, .reason = ava::app::StopReason::MaxTurns}).has_value(), "reused-id fixture evicts previous outcome");
      }
    }
    auto active = controller.admit({.request_id = "reused"});
    expect(active.has_value(), "same request id admits a new active generation");
    if (!active)
      return;
    PausedWaiter waiter(controller, "reused");
    expect(waiter.wait_until_registered(), "active generation is joined before any retained previous-generation outcome is considered");
    expect(active->complete({.run_id = {}, .reason = ava::app::StopReason::MaxToolCalls}).has_value(), "active reused-id generation publishes its own outcome");
    waiter.release();
    waiter.join();
    auto const& joined = waiter.joined;
    expect(joined && *joined && (*joined)->run_id == "reused" && (*joined)->reason == ava::app::StopReason::MaxToolCalls,
           "reused-id waiter receives the active generation's result with or without previous-outcome eviction");
  }
}

void test_session_run_controller_effective_failure_and_stop_reentry()
{
  auto const root = create_empty_root("session-run-controller-latch");

  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  expect(store.has_value(), "latch fixture store creates");
  if (!store)
    return;
  auto lease = ava::session::SessionLease::create_and_acquire(store->session_path());
  expect(lease.has_value(), "latch fixture acquires a lease");
  if (!lease)
    return;
  auto target = persistent_controller_target(*store, *lease);
  expect(target.has_value(), "latch fixture creates append target");
  if (!target)
    return;
  ava::app::SessionRunController controller(std::move(*target));
  auto admitted = controller.admit({.request_id = "failed"});
  expect(admitted.has_value(), "latch fixture admits");
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  std::atomic<bool> callback_reentered = false;
  std::stop_callback callback(guard.stop_token(), [&] { callback_reentered.store(controller.snapshot().stop_requested, std::memory_order_release); });
  auto invalid = append_entry("invalid");
  invalid.data_json.clear();
  expect(!guard.append_route()(std::move(invalid)).has_value(), "append failure latches");
  expect(callback_reentered.load(std::memory_order_acquire), "append failure stop callback reenters without deadlock");
  auto effective = guard.complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError});
  expect(effective && effective->reason == ava::app::StopReason::PersistenceError && effective->error,
         "complete reports persistence as effective terminal outcome");
  expect(!controller.admit({.request_id = "blocked"}).has_value(), "persistence latch blocks admission");
  expect(controller.reset_persistence_failure().has_value(), "explicit terminal/drained recovery clears latch");
  auto recovered = controller.admit({.request_id = "recovered"});
  expect(recovered.has_value(), "admission resumes only after explicit recovery");
}

struct PartialAppendControl
{
  std::atomic_bool fail = true;
  std::atomic_int calls = 0;

  ssize_t write(int fd, std::string_view bytes)
  {
    if (!fail.load(std::memory_order_acquire))
      return ::write(fd, bytes.data(), bytes.size());
    if (calls.fetch_add(1, std::memory_order_acq_rel) == 0)
      return ::write(fd, bytes.data(), std::min<std::size_t>(1, bytes.size()));
    errno = EIO;
    return -1;
  }
};

void test_session_run_controller_recovers_persistent_and_ephemeral_targets()
{
  auto const root = create_empty_root("session-run-controller-recovery");

  auto const workspace = root / "workspace";
  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  expect(store && lease, "controller recovery fixture creates a persistent owned store");
  if (store && lease)
  {
    expect(store->append(*lease, append_entry("recovery-prefix")).has_value(), "controller recovery fixture seeds a framed record");
    auto control = std::make_shared<PartialAppendControl>();
    store->set_append_write_for_test([control](int fd, std::string_view bytes) { return control->write(fd, bytes); });
    auto target = persistent_controller_target(*store, *lease);
    if (target)
    {
      ava::app::SessionRunController controller(std::move(*target));
      auto owner = controller.owner_append_route();
      auto torn = owner(append_entry("recovery-torn"));
      expect(!torn, "partial persistent owner append latches a controller failure");
      auto recovered = controller.reset_persistence_failure();
      control->fail.store(false, std::memory_order_release);
      auto next = owner(append_entry("recovery-next"));
      auto entries = store->load();
      expect(recovered && next && entries && entries->size() == 2 && entries->back().id == "recovery-next",
             "controller recovery repairs a torn tail before clearing the latch so the next owner append succeeds");
    }
  }

  auto ephemeral = ava::session::SessionStore::create_ephemeral(root / "ephemeral-workspace");
  auto ephemeral_target = ephemeral ? ava::session::SessionAppendTarget::create_ephemeral(*ephemeral)
                                    : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(ephemeral.error()));
  if (ephemeral_target)
  {
    ava::app::SessionRunController controller(std::move(*ephemeral_target));
    auto owner = controller.owner_append_route();
    auto invalid = append_entry("ephemeral-invalid");
    invalid.data_json.clear();
    auto failed = owner(std::move(invalid));
    auto recovered = controller.reset_persistence_failure();
    auto next = owner(append_entry("ephemeral-next"));
    expect(!failed && recovered && next, "ephemeral target recovery is an explicit no-op that clears only its controller latch");
  }
}

void test_session_run_controller_failed_and_inflight_recovery()
{
  auto const root = create_empty_root("session-run-controller-recovery-races");

  {
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root / "failed");
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      expect(store->append(*lease, append_entry("failed-prefix")).has_value(), "failed recovery fixture seeds a framed record");
      auto control = std::make_shared<PartialAppendControl>();
      store->set_append_write_for_test([control](int fd, std::string_view bytes) { return control->write(fd, bytes); });
      auto target = persistent_controller_target(*store, *lease);
      if (target)
      {
        ava::app::SessionRunController controller(std::move(*target));
        auto owner = controller.owner_append_route();
        auto torn = owner(append_entry("failed-torn"));
        auto const parked = store->session_path().string() + ".parked";
        std::filesystem::rename(store->session_path(), parked);
        std::ofstream replacement(store->session_path());
        replacement << "replacement\n";
        replacement.close();
        auto recovered = controller.reset_persistence_failure();
        auto blocked = owner(append_entry("must-remain-blocked"));
        expect(!torn && !recovered && !blocked &&
                   controller.inspect_admission({.request_id = "blocked"}) == ava::app::AdmissionDisposition::RejectPersistenceFailure,
               "failed persistent recovery leaves the original latch authoritative and blocks later owner and run appends");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root / "inflight");
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      expect(store->append(*lease, append_entry("inflight-prefix")).has_value(), "in-flight recovery fixture seeds a framed record");
      auto control = std::make_shared<PartialAppendControl>();
      std::mutex hook_mutex;
      std::condition_variable hook_cv;
      bool entered = false;
      bool release = false;
      store->set_before_append_identity_check_for_test([&] {
        std::unique_lock lock(hook_mutex);
        entered = true;
        hook_cv.notify_all();
        static_cast<void>(hook_cv.wait_for(lock, std::chrono::seconds(3), [&] { return release; }));
      });
      store->set_append_write_for_test([control](int fd, std::string_view bytes) { return control->write(fd, bytes); });
      auto target = persistent_controller_target(*store, *lease);
      if (target)
      {
        ava::app::SessionRunController controller(std::move(*target));
        auto owner = controller.owner_append_route();
        std::optional<ava::core::VoidResult> append_result;
        std::optional<ava::core::VoidResult> reset_result;
        ava::core::JoinThread writer = ava::core::JoinThread::create("writer", [&] { append_result.emplace(owner(append_entry("inflight-torn"))); });
        {
          std::unique_lock lock(hook_mutex);
          expect(hook_cv.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }), "in-flight append reaches deterministic write gate");
        }
        ava::core::JoinThread resetter = ava::core::JoinThread::create("resetter", [&] { reset_result.emplace(controller.reset_persistence_failure()); });
        {
          std::lock_guard lock(hook_mutex);
          release = true;
        }
        hook_cv.notify_all();
        writer.join();
        resetter.join();
        control->fail.store(false, std::memory_order_release);
        auto next = owner(append_entry("inflight-next"));
        expect(append_result && !*append_result && reset_result && *reset_result && next,
               "in-flight reset serializes behind the append driver, repairs its partial failure, and permits the next append");
      }
    }
  }
}

void test_session_run_controller_shutdown_during_recovery_and_queued_append()
{
  auto const root = create_empty_root("session-run-controller-shutdown-races");

  {
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root / "reset");
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      expect(store->append(*lease, append_entry("reset-prefix")).has_value(), "shutdown/reset fixture seeds a framed record");
      auto control = std::make_shared<PartialAppendControl>();
      std::mutex hook_mutex;
      std::condition_variable hook_cv;
      bool recovery_entered = false;
      bool release_recovery = false;
      store->set_append_write_for_test([control](int fd, std::string_view bytes) { return control->write(fd, bytes); });
      store->set_after_recovery_quarantine_publication_for_test([&] {
        std::unique_lock lock(hook_mutex);
        recovery_entered = true;
        hook_cv.notify_all();
        static_cast<void>(hook_cv.wait_for(lock, std::chrono::seconds(3), [&] { return release_recovery; }));
      });
      auto target = persistent_controller_target(*store, *lease);
      if (target)
      {
        ava::app::SessionRunController controller(std::move(*target));
        auto owner = controller.owner_append_route();
        expect(!owner(append_entry("reset-torn")), "shutdown/reset fixture creates a partial persistence latch");
        std::optional<ava::core::VoidResult> reset_result;
        ava::core::JoinThread resetter = ava::core::JoinThread::create("resetter", [&] { reset_result.emplace(controller.reset_persistence_failure()); });
        {
          std::unique_lock lock(hook_mutex);
          expect(hook_cv.wait_for(lock, std::chrono::seconds(3), [&] { return recovery_entered; }), "recovery reaches deterministic publication gate");
        }
        ava::core::JoinThread shutdown = ava::core::JoinThread::create("shutdown", [&] { controller.shutdown(); });
        auto const deadline = ava::tests::now_plus_seconds(3);
        while (controller.inspect_admission({.request_id = "closing"}) != ava::app::AdmissionDisposition::RejectClosing &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::yield();
        {
          std::lock_guard lock(hook_mutex);
          release_recovery = true;
        }
        hook_cv.notify_all();
        resetter.join();
        shutdown.join();
        expect(reset_result && !*reset_result && !owner(append_entry("stale-after-reset-shutdown")),
               "shutdown racing recovery revokes authority, prevents latch clearing, and completes without lock inversion");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root / "queued");
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      std::mutex hook_mutex;
      std::condition_variable hook_cv;
      bool active_entered = false;
      bool release_active = false;
      store->set_before_append_identity_check_for_test([&] {
        std::unique_lock lock(hook_mutex);
        if (active_entered)
          return;
        active_entered = true;
        hook_cv.notify_all();
        static_cast<void>(hook_cv.wait_for(lock, std::chrono::seconds(3), [&] { return release_active; }));
      });
      auto target = persistent_controller_target(*store, *lease);
      if (target)
      {
        ava::app::SessionRunController controller(std::move(*target));
        auto admitted = controller.admit({.request_id = "shutdown-queued"});
        if (admitted)
        {
          auto guard = std::move(*admitted);
          auto active = guard.append_route();
          auto owner = controller.owner_append_route();
          std::optional<ava::core::VoidResult> active_result;
          std::optional<ava::core::VoidResult> queued_result;
          ava::core::JoinThread first = ava::core::JoinThread::create("first", [&] { active_result.emplace(active(append_entry("shutdown-active"))); });
          {
            std::unique_lock lock(hook_mutex);
            expect(hook_cv.wait_for(lock, std::chrono::seconds(3), [&] { return active_entered; }), "active append reaches shutdown gate");
          }
          ava::core::JoinThread second = ava::core::JoinThread::create("second", [&] { queued_result.emplace(owner(append_entry("shutdown-queued"))); });
          auto const queue_deadline = ava::tests::now_plus_seconds(3);
          while (controller.snapshot().queued_appends < 2 && std::chrono::steady_clock::now() < queue_deadline)
            std::this_thread::yield();
          ava::core::JoinThread shutdown = ava::core::JoinThread::create("shutdown", [&] { controller.shutdown(); });
          auto const close_deadline = ava::tests::now_plus_seconds(3);
          while (controller.inspect_admission({.request_id = "closing"}) != ava::app::AdmissionDisposition::RejectClosing &&
                 std::chrono::steady_clock::now() < close_deadline)
            std::this_thread::yield();
          {
            std::lock_guard lock(hook_mutex);
            release_active = true;
          }
          hook_cv.notify_all();
          first.join();
          second.join();
          shutdown.join();
          auto outcome = guard.complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError});
          auto entries = store->load();
          expect(active_result && *active_result && queued_result && entries && entries->size() >= 1 && entries->size() <= 2 && outcome &&
                     outcome->reason != ava::app::StopReason::PersistenceError && !owner(append_entry("shutdown-stale")),
                 "shutdown preserves the accepted active append, completes a queued caller, releases authority, and never synthesizes a persistence latch");
        }
      }
    }
  }
}

void test_session_run_controller_cross_thread_observer_shutdown_is_nonblocking()
{
  struct ShutdownState
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool append_at_hook = false;
    bool release_append = false;
    bool callback_entered = false;
    bool call_shutdown = false;
    bool shutdown_calling = false;
    bool shutdown_returned = false;
    bool first_done = false;
    bool second_done = false;
    bool callback_context = false;
    std::optional<ava::core::VoidResult> first_result;
    std::optional<ava::core::VoidResult> second_result;
  };

  auto const root = create_empty_root("session-run-controller-observer-shutdown");

  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  expect(store && lease, "cross-thread observer shutdown fixture creates a persistent owned session");
  if (!store || !lease)
    return;

  auto state = std::make_shared<ShutdownState>();
  store->set_after_append_write_for_test([state] {
    std::unique_lock lock(state->mutex);
    if (state->append_at_hook)
      return;
    state->append_at_hook = true;
    state->changed.notify_all();
    static_cast<void>(state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->release_append; }));
  });

  auto controller_slot = std::make_shared<std::shared_ptr<ava::app::SessionRunController>>();
  auto observer = std::make_shared<CallbackRunObserver>([state, controller_slot](ava::observability::TraceEvent const& event) {
    if (event.type != ava::observability::TraceEventType::AgentRunStart)
      return;
    {
      std::unique_lock lock(state->mutex);
      state->callback_context = ava::observability::in_run_observer_callback();
      state->callback_entered = true;
      state->changed.notify_all();
      static_cast<void>(state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->call_shutdown; }));
      state->shutdown_calling = true;
    }
    state->changed.notify_all();
    if (auto controller = *controller_slot)
      controller->shutdown();
    {
      std::lock_guard lock(state->mutex);
      state->shutdown_returned = true;
    }
    state->changed.notify_all();
  });
  auto observation = std::make_shared<ava::observability::RunObservation>(observer);
  static_cast<void>(store->set_run_observation(observation, {}));

  auto target = persistent_controller_target(*store, *lease);
  expect(target.has_value(), "cross-thread observer shutdown fixture creates append target");
  if (!target)
    return;
  auto controller = std::make_shared<ava::app::SessionRunController>(std::move(*target));
  *controller_slot = controller;
  auto owner = controller->owner_append_route();
  auto first_entry = append_entry("observer-shutdown-active");
  auto second_entry = append_entry("observer-shutdown-queued");
  auto const expected_bytes = first_entry.id.size() + first_entry.parent_id.size() + first_entry.timestamp.size() + first_entry.data_json.size() + 32 +
                              second_entry.id.size() + second_entry.parent_id.size() + second_entry.timestamp.size() + second_entry.data_json.size() + 32;

  std::thread first([state, owner, entry = std::move(first_entry)]() mutable {
    auto result = owner(std::move(entry));
    {
      std::lock_guard lock(state->mutex);
      state->first_result.emplace(std::move(result));
      state->first_done = true;
    }
    state->changed.notify_all();
  });
  bool append_reached_hook = false;
  {
    std::unique_lock lock(state->mutex);
    append_reached_hook = state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->append_at_hook; });
  }
  expect(append_reached_hook, "append driver reaches the post-write gate while owning append serialization");
  if (!append_reached_hook)
  {
    {
      std::lock_guard lock(state->mutex);
      state->release_append = true;
    }
    state->changed.notify_all();
    first.join();
    return;
  }

  std::thread second([state, owner, entry = std::move(second_entry)]() mutable {
    auto result = owner(std::move(entry));
    {
      std::lock_guard lock(state->mutex);
      state->second_result.emplace(std::move(result));
      state->second_done = true;
    }
    state->changed.notify_all();
  });
  auto const queue_deadline = ava::tests::now_plus_seconds(3);
  auto queued = controller->snapshot();
  while (queued.queued_appends < 2 && std::chrono::steady_clock::now() < queue_deadline)
  {
    std::this_thread::yield();
    queued = controller->snapshot();
  }
  expect(queued.queued_appends == 2 && queued.queued_append_bytes == expected_bytes,
         "a second accepted ticket waits behind the append driver with exact byte accounting");

  std::thread emitter([observation] { observation->emit(ava::observability::TraceEventType::AgentRunStart, {}); });
  bool callback_entered = false;
  {
    std::unique_lock lock(state->mutex);
    callback_entered = state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->callback_entered; });
    state->call_shutdown = true;
  }
  state->changed.notify_all();
  expect(callback_entered, "unrelated producer enters the user observer while the append driver is gated");

  bool shutdown_calling = false;
  {
    std::unique_lock lock(state->mutex);
    shutdown_calling = state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->shutdown_calling; });
    state->release_append = true;
  }
  state->changed.notify_all();
  expect(shutdown_calling, "observer callback starts shutdown before the append may emit its result");

  bool completed = false;
  {
    std::unique_lock lock(state->mutex);
    completed = state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->shutdown_returned && state->first_done && state->second_done; });
  }
  if (!completed)
  {
    first.detach();
    second.detach();
    emitter.detach();
    expect(false, "observer shutdown and every accepted append ticket terminate without emit_mutex/append_mutex deadlock");
    return;
  }
  first.join();
  second.join();
  emitter.join();

  bool terminal_results = false;
  bool callback_context = false;
  {
    std::lock_guard lock(state->mutex);
    terminal_results = state->first_result && *state->first_result && state->second_result && !*state->second_result;
    callback_context = state->callback_context;
  }
  auto terminal = controller->snapshot();
  *lease = ava::session::SessionLease{};
  auto reacquired = ava::session::SessionLease::acquire(store->session_path());
  expect(terminal_results && callback_context && terminal.queued_appends == 0 && terminal.queued_append_bytes == 0 && reacquired &&
             !owner(append_entry("observer-shutdown-stale")),
         "observer shutdown returns nonblocking, commits only the in-flight append, drains queued work, and releases target lease authority");
}

void test_session_run_controller_observer_reset_is_immediate_for_unrelated_observer()
{
  struct ResetState
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool append_at_hook = false;
    bool release_append = false;
    bool callback_returned = false;
    bool writer_done = false;
    bool callback_context = false;
    std::optional<ava::core::VoidResult> reset_result;
    std::optional<ava::core::VoidResult> append_result;
  };

  auto const root = create_empty_root("session-run-controller-observer-reset");

  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  expect(store && lease, "observer reset fixture creates a persistent owned session");
  if (!store || !lease)
    return;

  auto state = std::make_shared<ResetState>();
  store->set_after_append_write_for_test([state] {
    std::unique_lock lock(state->mutex);
    state->append_at_hook = true;
    state->changed.notify_all();
    static_cast<void>(state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->release_append; }));
  });
  auto target = persistent_controller_target(*store, *lease);
  if (!target)
    return;
  auto controller = std::make_shared<ava::app::SessionRunController>(std::move(*target));
  auto owner = controller->owner_append_route();

  std::thread writer([state, owner] {
    auto result = owner(append_entry("observer-reset-active"));
    {
      std::lock_guard lock(state->mutex);
      state->append_result.emplace(std::move(result));
      state->writer_done = true;
    }
    state->changed.notify_all();
  });
  bool append_reached_hook = false;
  {
    std::unique_lock lock(state->mutex);
    append_reached_hook = state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->append_at_hook; });
  }

  auto unrelated =
      std::make_shared<ava::observability::RunObservation>(std::make_shared<CallbackRunObserver>([state, controller](ava::observability::TraceEvent const&) {
        auto reset = controller->reset_persistence_failure();
        {
          std::lock_guard lock(state->mutex);
          state->callback_context = ava::observability::in_run_observer_callback();
          state->reset_result.emplace(std::move(reset));
          state->callback_returned = true;
        }
        state->changed.notify_all();
      }));
  std::thread emitter([unrelated] { unrelated->emit(ava::observability::TraceEventType::AgentRunStart, {}); });

  bool callback_returned_while_driver_gated = false;
  {
    std::unique_lock lock(state->mutex);
    callback_returned_while_driver_gated = state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->callback_returned; });
    state->release_append = true;
  }
  state->changed.notify_all();
  writer.join();
  emitter.join();

  bool stable_rejection = false;
  bool append_succeeded = false;
  bool callback_context = false;
  {
    std::lock_guard lock(state->mutex);
    stable_rejection =
        state->reset_result && !*state->reset_result && state->reset_result->error().message() == "cannot reset persistence from a session append observer";
    append_succeeded = state->append_result && *state->append_result;
    callback_context = state->callback_context;
  }
  expect(append_reached_hook && callback_returned_while_driver_gated && callback_context && stable_rejection && append_succeeded,
         "reset from any user observer rejects with its stable error before waiting on append serialization");
  controller->shutdown();
}

void test_session_run_controller_nested_controller_membership_is_stack_aware()
{
  struct NestedState
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::function<ava::core::VoidResult(ava::session::SessionEntry)> route_a;
    std::function<ava::core::VoidResult(ava::session::SessionEntry)> route_b;
    std::optional<ava::core::VoidResult> recursive_a_result;
    std::optional<ava::core::VoidResult> nested_b_result;
    std::optional<ava::core::VoidResult> outer_a_result;
    bool entered_a = false;
    bool entered_b = false;
    bool outer_done = false;
  };

  auto state = std::make_shared<NestedState>();
  auto const root = create_empty_root("test_session_run_controller_nested_controller_membership_is_stack_aware");

  auto store_a = ava::session::SessionStore::create_ephemeral(root / "controller-nested-a");
  auto store_b = ava::session::SessionStore::create_ephemeral(root / "controller-nested-b");
  expect(store_a && store_b, "nested controller fixture creates two ephemeral sessions");
  if (!store_a || !store_b)
    return;

  auto observer_a = std::make_shared<CallbackRunObserver>([state](ava::observability::TraceEvent const& event) {
    if (event.type != ava::observability::TraceEventType::SessionAppendAttempt)
      return;
    {
      std::lock_guard lock(state->mutex);
      if (state->entered_a)
        return;
      state->entered_a = true;
    }
    auto result = state->route_b(append_entry("nested-b"));
    std::lock_guard lock(state->mutex);
    state->nested_b_result.emplace(std::move(result));
  });
  auto observer_b = std::make_shared<CallbackRunObserver>([state](ava::observability::TraceEvent const& event) {
    if (event.type != ava::observability::TraceEventType::SessionAppendAttempt)
      return;
    {
      std::lock_guard lock(state->mutex);
      if (state->entered_b)
        return;
      state->entered_b = true;
    }
    auto result = state->route_a(append_entry("recursive-a"));
    std::lock_guard lock(state->mutex);
    state->recursive_a_result.emplace(std::move(result));
  });
  static_cast<void>(store_a->set_run_observation(std::make_shared<ava::observability::RunObservation>(observer_a), {}));
  static_cast<void>(store_b->set_run_observation(std::make_shared<ava::observability::RunObservation>(observer_b), {}));
  auto target_a = ava::session::SessionAppendTarget::create_ephemeral(*store_a);
  auto target_b = ava::session::SessionAppendTarget::create_ephemeral(*store_b);
  expect(target_a && target_b, "nested controller fixture creates two append targets");
  if (!target_a || !target_b)
    return;
  auto controller_a = std::make_shared<ava::app::SessionRunController>(std::move(*target_a));
  auto controller_b = std::make_shared<ava::app::SessionRunController>(std::move(*target_b));
  state->route_a = controller_a->owner_append_route();
  state->route_b = controller_b->owner_append_route();

  std::thread writer([state, controller_a, controller_b] {
    auto result = state->route_a(append_entry("outer-a"));
    {
      std::lock_guard lock(state->mutex);
      state->outer_a_result.emplace(std::move(result));
      state->outer_done = true;
    }
    state->changed.notify_all();
  });
  bool completed = false;
  {
    std::unique_lock lock(state->mutex);
    completed = state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->outer_done; });
  }
  if (!completed)
  {
    writer.detach();
    expect(false, "nested A to B to A callback rejects instead of recursively waiting on A append serialization");
    return;
  }
  writer.join();

  bool recursive_rejected = false;
  bool nested_succeeded = false;
  bool outer_succeeded = false;
  {
    std::lock_guard lock(state->mutex);
    recursive_rejected =
        state->recursive_a_result && !*state->recursive_a_result && state->recursive_a_result->error().message() == "reentrant session append is not allowed";
    nested_succeeded = state->nested_b_result && *state->nested_b_result;
    outer_succeeded = state->outer_a_result && *state->outer_a_result;
  }
  auto entries_a = store_a->load();
  auto entries_b = store_b->load();
  expect(recursive_rejected && nested_succeeded && outer_succeeded && entries_a && entries_a->size() == 1 && entries_b && entries_b->size() == 1,
         "active-controller membership retains outer A across nested B and preserves both nonrecursive FIFO appends");
  controller_a->shutdown();
  controller_b->shutdown();
  state->route_a = {};
  state->route_b = {};
}

void test_session_run_controller_unrelated_observer_shutdown_without_active_append()
{
  struct ShutdownState
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool returned = false;
    bool callback_context = false;
  };

  auto const root = create_empty_root("session-run-controller-unrelated-observer-shutdown");

  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  expect(store && lease, "unrelated observer shutdown fixture creates a persistent owned session");
  if (!store || !lease)
    return;
  auto target = persistent_controller_target(*store, *lease);
  if (!target)
    return;
  auto controller = std::make_shared<ava::app::SessionRunController>(std::move(*target));
  auto owner = controller->owner_append_route();
  auto state = std::make_shared<ShutdownState>();
  auto observation =
      std::make_shared<ava::observability::RunObservation>(std::make_shared<CallbackRunObserver>([state, controller](ava::observability::TraceEvent const&) {
        controller->shutdown();
        {
          std::lock_guard lock(state->mutex);
          state->callback_context = ava::observability::in_run_observer_callback();
          state->returned = true;
        }
        state->changed.notify_all();
      }));

  std::thread emitter([observation] { observation->emit(ava::observability::TraceEventType::AgentRunStart, {}); });
  bool returned = false;
  {
    std::unique_lock lock(state->mutex);
    returned = state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->returned; });
  }
  emitter.join();
  *lease = ava::session::SessionLease{};
  auto reacquired = ava::session::SessionLease::acquire(store->session_path());
  expect(returned && state->callback_context && reacquired && !owner(append_entry("unrelated-observer-stale")),
         "an unrelated observer finalizes an idle controller immediately and copied routes retain no target lease");
}

void test_session_run_controller_conditional_rejections_do_not_latch()
{
  {
    auto const root = create_empty_root("session-run-controller-conditional-final-read-cancel");
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease)
      return;

    std::string parent;
    for (int index = 0; index < 64; ++index)
    {
      auto entry = append_entry("cancel-read-" + std::to_string(index));
      entry.parent_id = parent;
      entry.data_json = "{\"padding\":\"" + std::string(192, 'x') + "\"}";
      if (!store->append(*lease, entry))
        return;
      parent = entry.id;
    }
    int read_hook_calls = 0;
    std::atomic<bool> cancel = false;
    store->set_after_lease_bound_read_for_test([&] {
      if (++read_hook_calls == 2)
        cancel.store(true, std::memory_order_release);
    });
    auto target = persistent_controller_target(*store, *lease);
    if (!target)
      return;
    ava::app::SessionRunController controller(std::move(*target));
    auto admitted = controller.admit({.request_id = "conditional-final-read-cancel"});
    if (!admitted)
      return;
    auto guard = std::move(*admitted);
    int cancel_calls = 0;
    auto const before = read_binary_file(store->session_path());
    auto summary = branch_summary_entry(*store, "cancel-read-summary", parent, "cancel-read-0", parent);
    auto canceled = controller.append_branch_summary_if_absent(std::move(summary), [&] {
      ++cancel_calls;
      return cancel.load(std::memory_order_acquire);
    });
    auto const after_canceled = read_binary_file(store->session_path());
    auto later = controller.append(append_entry("cancel-read-later"));
    auto entries = store->load();
    expect(!canceled && !append_error_context(canceled.error(), "append_commit_state") && read_hook_calls == 2 && cancel_calls >= 2 &&
               after_canceled == before && read_binary_file(store->session_path()).starts_with(before) && later && entries && entries->size() == 65 &&
               !guard.stop_requested() && controller.inspect_admission({.request_id = "other"}) != ava::app::AdmissionDisposition::RejectPersistenceFailure,
           "conditional cancellation during the final bounded read is nonmutating, does not latch, and permits a later ordinary append");
  }

  {
    auto const root = create_empty_root("session-run-controller-conditional-linearization-cancel");
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease)
      return;
    auto root_entry = append_entry("linearization-root");
    auto tip_entry = append_entry("linearization-tip");
    tip_entry.parent_id = root_entry.id;
    if (!store->append(*lease, root_entry) || !store->append(*lease, tip_entry))
      return;
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    int cancel_calls = 0;
    bool at_linearization = false;
    bool release = false;
    bool ordinary_at_write = false;
    bool release_ordinary = false;
    store->set_before_append_identity_check_for_test([&] {
      std::unique_lock lock(gate_mutex);
      ordinary_at_write = true;
      gate_changed.notify_all();
      static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release_ordinary; }));
    });
    auto target = persistent_controller_target(*store, *lease);
    if (!target)
      return;
    ava::app::SessionRunController controller(std::move(*target));
    auto admitted = controller.admit({.request_id = "conditional-linearization-cancel"});
    if (!admitted)
      return;
    auto guard = std::move(*admitted);
    expect(controller.wake({.kind = ava::app::RunCommand::Kind::Wake, .correlation_id = {}, .message = "unrelated wake"}).has_value(),
           "conditional rejection fixture queues an unrelated wake");

    std::optional<ava::core::Result<ava::session::SessionConditionalAppendResult>> conditional_result;
    std::optional<ava::core::VoidResult> ordinary_result;
    auto summary = branch_summary_entry(*store, "linearization-summary", tip_entry.id, root_entry.id, tip_entry.id);
    std::jthread conditional_writer([&] {
      conditional_result.emplace(controller.append_branch_summary_if_absent(std::move(summary), [&] {
        std::unique_lock lock(gate_mutex);
        ++cancel_calls;
        if (cancel_calls != 2)
          return false;
        at_linearization = true;
        gate_changed.notify_all();
        static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release; }));
        return true;
      }));
    });
    {
      std::unique_lock lock(gate_mutex);
      expect(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return at_linearization; }),
             "conditional append reaches its post-read cancellation linearization point");
    }
    auto const before = read_binary_file(store->session_path());
    std::jthread ordinary_writer([&] { ordinary_result.emplace(controller.append(append_entry("linearization-later"))); });
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto queued = controller.snapshot();
    while (queued.queued_appends < 2 && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
      queued = controller.snapshot();
    }
    {
      std::lock_guard lock(gate_mutex);
      release = true;
    }
    gate_changed.notify_all();
    bool reached_ordinary_write = false;
    {
      std::unique_lock lock(gate_mutex);
      reached_ordinary_write = gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return ordinary_at_write; });
    }
    auto const after_rejection = read_binary_file(store->session_path());
    {
      std::lock_guard lock(gate_mutex);
      release_ordinary = true;
    }
    gate_changed.notify_all();
    conditional_writer.join();
    ordinary_writer.join();

    auto entries = store->load();
    auto wake = controller.take_commands("conditional-linearization-cancel", ava::app::RunCommand::Kind::Wake);
    auto drained = controller.snapshot();
    expect(queued.queued_appends == 2 && conditional_result && !*conditional_result &&
               !append_error_context(conditional_result->error(), "append_commit_state") && reached_ordinary_write && after_rejection == before &&
               ordinary_result && *ordinary_result && entries && entries->size() == 3 && entries->back().id == "linearization-later" &&
               read_binary_file(store->session_path()).starts_with(before) && cancel_calls == 2 && wake && wake->size() == 1 && drained.queued_appends == 0 &&
               drained.queued_append_bytes == 0 && !guard.stop_requested() &&
               controller.inspect_admission({.request_id = "other"}) != ava::app::AdmissionDisposition::RejectPersistenceFailure,
           "post-read cancellation rejects only its ticket, preserves wake and queue accounting, and permits a queued ordinary append");
  }

  {
    auto const root = create_empty_root("session-run-controller-conditional-stale-parent");
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease)
      return;
    auto root_entry = append_entry("stale-root");
    auto tip_entry = append_entry("stale-tip");
    tip_entry.parent_id = root_entry.id;
    if (!store->append(*lease, root_entry) || !store->append(*lease, tip_entry))
      return;

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool external_at_write = false;
    bool release_external = false;
    store->set_before_append_identity_check_for_test([&] {
      std::unique_lock lock(gate_mutex);
      if (external_at_write)
        return;
      external_at_write = true;
      gate_changed.notify_all();
      static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release_external; }));
    });
    auto controller_target = persistent_controller_target(*store, *lease);
    auto external_target = persistent_controller_target(*store, *lease);
    if (!controller_target || !external_target)
      return;
    ava::app::SessionRunController controller(std::move(*controller_target));
    auto admitted = controller.admit({.request_id = "conditional-stale-parent"});
    if (!admitted)
      return;
    auto guard = std::move(*admitted);

    auto growth = append_entry("stale-growth");
    growth.parent_id = tip_entry.id;
    std::optional<ava::core::VoidResult> growth_result;
    std::jthread external_writer([&] { growth_result.emplace((*external_target)->append(growth)); });
    {
      std::unique_lock lock(gate_mutex);
      expect(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return external_at_write; }),
             "stale-parent fixture holds a concurrent valid append at its write boundary");
    }
    auto summary = branch_summary_entry(*store, "stale-summary", tip_entry.id, root_entry.id, tip_entry.id);
    std::optional<ava::core::Result<ava::session::SessionConditionalAppendResult>> stale_result;
    std::jthread conditional_writer([&] { stale_result.emplace(controller.append_branch_summary_if_absent(std::move(summary))); });
    {
      std::lock_guard lock(gate_mutex);
      release_external = true;
    }
    gate_changed.notify_all();
    external_writer.join();
    conditional_writer.join();

    auto later = controller.append(append_entry("stale-later"));
    auto entries = store->load();
    bool const has_summary = entries && std::ranges::any_of(*entries, [](auto const& entry) { return entry.type == ava::session::EntryType::BranchSummary; });
    expect(growth_result && *growth_result && stale_result && !*stale_result && !append_error_context(stale_result->error(), "append_commit_state") && later &&
               entries && entries->size() == 4 && !has_summary && !guard.stop_requested() &&
               controller.inspect_admission({.request_id = "other"}) != ava::app::AdmissionDisposition::RejectPersistenceFailure,
           "a concurrent valid append makes the prepared parent stale without latching, appending a summary, or blocking later persistence");
  }
}

void test_session_run_controller_conditional_append_failures_latch_by_commit_state()
{
  auto run_case = [](std::string const& name, std::string_view expected_state) {
    auto const root = create_empty_root("session-run-controller-conditional-" + name);
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease)
      return false;
    auto root_entry = append_entry(name + "-root");
    auto tip_entry = append_entry(name + "-tip");
    tip_entry.parent_id = root_entry.id;
    if (!store->append(*lease, root_entry) || !store->append(*lease, tip_entry))
      return false;
    auto const before = read_binary_file(store->session_path());

    int write_calls = 0;
    if (expected_state == "not_started")
    {
      store->set_append_write_for_test([&](int, std::string_view) -> ssize_t {
        ++write_calls;
        errno = EIO;
        return -1;
      });
    }
    else if (expected_state == "partial_or_unknown")
    {
      store->set_append_write_for_test([&](int fd, std::string_view bytes) -> ssize_t {
        ++write_calls;
        if (write_calls == 1)
          return ::write(fd, bytes.data(), std::max<std::size_t>(1, bytes.size() / 2));
        errno = EIO;
        return -1;
      });
    }
    else
    {
      store->set_append_write_for_test([&](int fd, std::string_view bytes) -> ssize_t {
        ++write_calls;
        return ::write(fd, bytes.data(), bytes.size());
      });
      store->set_after_append_write_for_test([] { throw 1; });
    }

    auto target = persistent_controller_target(*store, *lease);
    if (!target)
      return false;
    ava::app::SessionRunController controller(std::move(*target));
    auto admitted = controller.admit({.request_id = "conditional-" + name});
    if (!admitted)
      return false;
    auto guard = std::move(*admitted);
    auto summary = branch_summary_entry(*store, name + "-summary", tip_entry.id, root_entry.id, tip_entry.id);
    auto failed = controller.append_branch_summary_if_absent(std::move(summary));
    auto blocked = controller.append(append_entry(name + "-blocked"));
    bool const stop_requested = guard.stop_requested();
    auto outcome = guard.complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError});
    auto const after = read_binary_file(store->session_path());
    bool const bytes_match_state = expected_state == "not_started" ? after == before : after != before;
    int const expected_write_calls = expected_state == "partial_or_unknown" ? 2 : 1;
    return !failed && append_error_context(failed.error(), "append_commit_state") == std::optional<std::string>(expected_state) && !blocked &&
           append_error_context(blocked.error(), "append_commit_state") == std::optional<std::string>(expected_state) && write_calls == expected_write_calls &&
           stop_requested && outcome && outcome->reason == ava::app::StopReason::PersistenceError && bytes_match_state &&
           controller.inspect_admission({.request_id = "blocked"}) == ava::app::AdmissionDisposition::RejectPersistenceFailure;
  };

  expect(run_case("not-started", "not_started"), "conditional append_impl not-started failures latch once and are never retried");
  expect(run_case("partial", "partial_or_unknown"), "conditional append_impl partial failures latch once and are never retried");
  expect(run_case("committed", "committed_to_leased_inode"), "conditional append_impl committed failures latch once and are never retried");

  {
    auto const root = create_empty_root("session-run-controller-conditional-throwing-cancel-public");
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease)
      return;
    auto root_entry = append_entry("throwing-public-root");
    auto tip_entry = append_entry("throwing-public-tip");
    tip_entry.parent_id = root_entry.id;
    if (!store->append(*lease, root_entry) || !store->append(*lease, tip_entry))
      return;
    auto target = persistent_controller_target(*store, *lease);
    if (!target)
      return;
    auto summary = branch_summary_entry(*store, "throwing-public-summary", tip_entry.id, root_entry.id, tip_entry.id);
    bool threw = false;
    std::optional<ava::core::Result<ava::session::SessionConditionalAppendResult>> result;
    try
    {
      result.emplace((*target)->append_branch_summary_if_absent(summary, []() -> bool { throw std::runtime_error("cancel callback failure"); }));
    }
    catch (...)
    {
      threw = true;
    }
    expect(!threw && result && !*result && append_error_context(result->error(), "append_commit_state") == std::optional<std::string>("partial_or_unknown") &&
               append_error_context(result->error(), "recovery").has_value(),
           "the public conditional append API returns a stable unknown-state error when cancellation throws before append");
  }

  {
    auto const root = create_empty_root("session-run-controller-conditional-throwing-cancel");
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease)
      return;
    auto root_entry = append_entry("throwing-cancel-root");
    auto tip_entry = append_entry("throwing-cancel-tip");
    tip_entry.parent_id = root_entry.id;
    if (!store->append(*lease, root_entry) || !store->append(*lease, tip_entry))
      return;
    auto const before = read_binary_file(store->session_path());
    auto target = persistent_controller_target(*store, *lease);
    if (!target)
      return;
    ava::app::SessionRunController controller(std::move(*target));
    auto admitted = controller.admit({.request_id = "conditional-throwing-cancel"});
    if (!admitted)
      return;
    auto guard = std::move(*admitted);
    auto route = guard.append_route();

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool callback_entered = false;
    bool release_callback = false;
    std::optional<ava::core::Result<ava::session::SessionConditionalAppendResult>> conditional_result;
    std::optional<ava::core::VoidResult> queued_result;
    auto summary = branch_summary_entry(*store, "throwing-cancel-summary", tip_entry.id, root_entry.id, tip_entry.id);
    std::jthread conditional_writer([&] {
      conditional_result.emplace(controller.append_branch_summary_if_absent(std::move(summary), [&]() -> bool {
        std::unique_lock lock(gate_mutex);
        callback_entered = true;
        gate_changed.notify_all();
        static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release_callback; }));
        throw std::runtime_error("cancel callback failure");
      }));
    });
    {
      std::unique_lock lock(gate_mutex);
      expect(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return callback_entered; }),
             "throwing cancellation callback reaches the pre-append read boundary");
    }
    std::jthread queued_writer([&] { queued_result.emplace(route(append_entry("throwing-cancel-queued"))); });
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto queued = controller.snapshot();
    while (queued.queued_appends < 2 && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
      queued = controller.snapshot();
    }
    {
      std::lock_guard lock(gate_mutex);
      release_callback = true;
    }
    gate_changed.notify_all();
    conditional_writer.join();
    queued_writer.join();

    auto blocked = controller.append(append_entry("throwing-cancel-blocked"));
    bool const stop_requested = guard.stop_requested();
    auto outcome = guard.complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError});
    auto drained = controller.snapshot();
    auto const expected_state = std::optional<std::string>("partial_or_unknown");
    expect(queued.queued_appends == 2 && conditional_result && !*conditional_result &&
               append_error_context(conditional_result->error(), "append_commit_state") == expected_state && queued_result && !*queued_result &&
               append_error_context(queued_result->error(), "append_commit_state") == expected_state && !blocked &&
               append_error_context(blocked.error(), "append_commit_state") == expected_state && stop_requested && outcome &&
               outcome->reason == ava::app::StopReason::PersistenceError && drained.queued_appends == 0 && drained.queued_append_bytes == 0 &&
               read_binary_file(store->session_path()) == before &&
               controller.inspect_admission({.request_id = "blocked"}) == ava::app::AdmissionDisposition::RejectPersistenceFailure,
           "a throwing pre-append cancellation callback latches, stops, drains queued appends, and blocks subsequent persistence");
  }
}

void test_session_run_controller_failure_drains_tickets_with_exact_accounting()
{
  auto const root = create_empty_root("session-run-controller-failure-tickets");

  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  if (!store || !lease)
    return;
  expect(store->append(*lease, append_entry("ticket-prefix")).has_value(), "failure ticket fixture seeds a framed record");

  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool entered = false;
  bool release = false;
  store->set_before_append_identity_check_for_test([&] {
    std::unique_lock lock(gate_mutex);
    if (entered)
      return;
    entered = true;
    gate_changed.notify_all();
    static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release; }));
  });
  auto control = std::make_shared<PartialAppendControl>();
  store->set_append_write_for_test([control](int fd, std::string_view bytes) { return control->write(fd, bytes); });
  auto target = persistent_controller_target(*store, *lease);
  if (!target)
    return;
  ava::app::SessionRunController controller(std::move(*target));
  auto admitted = controller.admit({.request_id = "failure-tickets"});
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  auto route = guard.append_route();

  auto first_entry = append_entry("ticket-first");
  auto second_entry = append_entry("ticket-second");
  auto third_entry = append_entry("ticket-third");
  auto bytes = [](ava::session::SessionEntry const& entry) {
    return entry.id.size() + entry.parent_id.size() + entry.timestamp.size() + entry.data_json.size() + 32;
  };
  auto const expected_bytes = bytes(first_entry) + bytes(second_entry) + bytes(third_entry);
  std::optional<ava::core::VoidResult> first_result;
  std::optional<ava::core::VoidResult> second_result;
  std::optional<ava::core::VoidResult> third_result;
  ava::core::JoinThread first = ava::core::JoinThread::create("first", [&] { first_result.emplace(route(std::move(first_entry))); });
  {
    std::unique_lock lock(gate_mutex);
    expect(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }), "failure ticket first append reaches its write gate");
  }
  ava::core::JoinThread second = ava::core::JoinThread::create("second", [&] { second_result.emplace(route(std::move(second_entry))); });
  ava::core::JoinThread third = ava::core::JoinThread::create("third", [&] { third_result.emplace(route(std::move(third_entry))); });
  auto const deadline = ava::tests::now_plus_seconds(3);
  auto queued = controller.snapshot();
  while (queued.queued_appends < 3 && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::yield();
    queued = controller.snapshot();
  }
  expect(queued.queued_appends == 3 && queued.queued_append_bytes == expected_bytes,
         "accepted append tickets expose exact aggregate queue byte accounting before persistence");
  {
    std::lock_guard lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  first.join();
  second.join();
  third.join();

  auto same_failure = [&] {
    return first_result && second_result && third_result && !*first_result && !*second_result && !*third_result &&
           first_result->error().format() == second_result->error().format() && first_result->error().format() == third_result->error().format() &&
           first_result->error().format().find("append_commit_state: partial_or_unknown") != std::string::npos;
  };
  auto drained = controller.snapshot();
  auto outcome = guard.complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError});
  control->fail.store(false, std::memory_order_release);
  auto recovered = controller.reset_persistence_failure();
  auto next = controller.append(append_entry("ticket-after-recovery"));
  auto entries = store->load();
  expect(same_failure() && control->calls.load(std::memory_order_acquire) == 2 && drained.queued_appends == 0 && drained.queued_append_bytes == 0 && outcome &&
             outcome->reason == ava::app::StopReason::PersistenceError && recovered && next && entries && entries->size() == 2,
         "one immutable partial failure terminally drains every accepted ticket, prevents queued writes, and recovers without byte-account drift");
}

void test_session_run_controller_batch_is_one_ticket_and_latches_failure()
{
  auto const root = create_empty_root("session-run-controller-batch");
  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  if (!store || !lease)
    return;

  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool entered = false;
  bool release = false;
  store->set_before_append_identity_check_for_test([&] {
    std::unique_lock lock(gate_mutex);
    entered = true;
    gate_changed.notify_all();
    static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release; }));
  });
  store->set_append_write_for_test([](int, std::string_view) -> ssize_t {
    errno = EIO;
    return -1;
  });

  auto target = persistent_controller_target(*store, *lease);
  if (!target)
    return;
  ava::app::SessionRunController controller(std::move(*target));
  auto admitted = controller.admit({.request_id = "batch"});
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  auto route = guard.append_batch_route();
  auto output_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "controller-batch-turn",
      .sequence = 0,
      .kind = ava::session::AssistantOutputItemKind::Text,
      .provider_item_id = "controller-batch-item",
      .provider_output_index = 0,
      .payload = ava::session::AssistantOutputText{.text = "staged", .assistant_phase = ava::session::AssistantOutputTextPhase::Commentary}});
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "controller-batch-turn",
                                                                                                               .item_count = 1,
                                                                                                               .provider = "openai",
                                                                                                               .model = "gpt-5.5",
                                                                                                               .finish_reason = "completed",
                                                                                                               .usage_json = std::nullopt});
  if (!output_data || !commit_data)
    return;
  std::vector<ava::session::SessionEntry> entries;
  auto output = append_entry("batch-output");
  output.type = ava::session::EntryType::AssistantOutputItem;
  output.data_json = std::move(*output_data);
  entries.push_back(std::move(output));
  auto commit = append_entry("batch-commit");
  commit.type = ava::session::EntryType::AssistantTurnCommit;
  commit.data_json = std::move(*commit_data);
  entries.push_back(std::move(commit));
  auto bytes = [](ava::session::SessionEntry const& entry) {
    return entry.id.size() + entry.parent_id.size() + entry.timestamp.size() + entry.data_json.size() + 32;
  };
  auto const expected_bytes = bytes(entries[0]) + bytes(entries[1]);
  std::optional<ava::core::VoidResult> result;
  ava::core::JoinThread writer = ava::core::JoinThread::create("writer", [&] { result.emplace(route(std::move(entries))); });
  bool reached_gate = false;
  {
    std::unique_lock lock(gate_mutex);
    reached_gate = gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; });
  }
  auto queued = controller.snapshot();
  {
    std::lock_guard lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  writer.join();

  auto outcome = guard.complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError});
  auto drained = controller.snapshot();
  expect(reached_gate && queued.queued_appends == 1 && queued.queued_append_bytes == expected_bytes && result && !*result && outcome &&
             outcome->reason == ava::app::StopReason::PersistenceError && drained.queued_appends == 0 && drained.queued_append_bytes == 0 &&
             controller.inspect_admission({.request_id = "blocked"}) == ava::app::AdmissionDisposition::RejectPersistenceFailure,
         "one session append batch reserves one bounded controller ticket and latches a persistence failure exactly like a single append");
}

void test_session_run_controller_concurrent_fifo_appends()
{
  auto const root = create_empty_root("session-run-controller-fifo");

  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  expect(store.has_value(), "fifo fixture store creates");
  if (!store)
    return;
  auto lease = ava::session::SessionLease::create_and_acquire(store->session_path());
  expect(lease.has_value(), "fifo fixture acquires a lease");
  if (!lease)
    return;
  auto target = persistent_controller_target(*store, *lease);
  expect(target.has_value(), "fifo fixture creates append target");
  if (!target)
    return;
  ava::app::SessionRunController controller(std::move(*target));
  auto admitted = controller.admit({.request_id = "fifo"});
  expect(admitted.has_value(), "fifo fixture admits");
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  auto active = guard.append_route();
  auto owner = controller.owner_append_route();
  std::atomic<int> accepted = 0;
  std::vector<ava::core::JoinThread> workers;
  for (int i = 0; i < 16; ++i)
    workers.emplace_back(ava::core::JoinThread::create("append", [&, i] {
      if ((i % 2 ? active : owner)(append_entry("fifo-" + std::to_string(i))))
        ++accepted;
    }));
  for (auto& worker : workers)
    worker.join();
  auto entries = store->load();
  expect(accepted == 16 && entries && entries->size() == 16, "concurrent owner and generation appends retain every accepted record");
  if (entries)
  {
    std::vector<std::string> ids;
    for (auto const& entry : *entries)
      ids.push_back(entry.id);
    std::sort(ids.begin(), ids.end());
    expect(std::ranges::adjacent_find(ids) == ids.end(), "concurrent accepted entries retain unique ids");
  }
}

void test_session_run_controller_routes_release_target_on_shutdown()
{
  auto const root = create_empty_root("test_session_run_controller_routes_release_target_on_shutdown");
  auto store = ava::session::SessionStore::create_ephemeral(root / "controller-route-teardown");
  expect(store.has_value(), "sessionless route teardown fixture creates an ephemeral store");
  if (!store)
    return;
  auto target = ava::session::SessionAppendTarget::create_ephemeral(*store);
  expect(target.has_value(), "sessionless route teardown fixture creates an append target");
  if (!target)
    return;
  ava::app::SessionRunController controller(std::move(*target));
  auto admitted = controller.admit({.request_id = "shutdown"});
  expect(admitted.has_value(), "sessionless controller admits an active generation");
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  auto owner = controller.owner_append_route();
  auto active = guard.append_route();
  expect(owner(append_entry("sessionless-owner")).has_value() && active(append_entry("sessionless-active")).has_value(),
         "sessionless owner and active routes append through the immutable target");

  std::mutex mutex;
  std::condition_variable ready;
  bool writer_ready = false;
  bool release_writer = false;
  std::optional<ava::core::VoidResult> racing_result;
  ava::core::JoinThread writer = ava::core::JoinThread::create("writer", [&] {
    std::unique_lock lock(mutex);
    writer_ready = true;
    ready.notify_all();
    ready.wait(lock, [&] { return release_writer; });
    lock.unlock();
    racing_result.emplace(owner(append_entry("racing-owner")));
  });
  {
    std::unique_lock lock(mutex);
    ready.wait(lock, [&] { return writer_ready; });
    release_writer = true;
  }
  ready.notify_all();
  controller.shutdown();
  writer.join();
  expect(racing_result.has_value(), "route-vs-teardown racing append completes or fails without retaining a runtime reference");
  expect(!owner(append_entry("stale-owner")).has_value() && !active(append_entry("stale-active")).has_value(),
         "owner and active route copies reject after teardown clears the append target");
  auto entries = store->load();
  expect(entries && entries->size() >= 2 && entries->size() <= 3, "teardown either drains or fails the one concurrent accepted append without data loss");
}

void test_session_run_controller_exclusive_maintenance_reservation()
{
  {
    ava::app::SessionRunController controller(ephemeral_controller_target());
    auto reserved = controller.reserve_maintenance();
    expect(reserved.has_value() && reserved->active() && controller.snapshot().maintenance_reserved && !controller.snapshot().active,
           "move-only maintenance reservation is visible without impersonating an active run");
    auto const inspected = controller.inspect_admission({.request_id = "maintenance-inspection"});
    auto second = controller.reserve_maintenance();
    auto admitted = controller.admit({.request_id = "maintenance-conflict"});
    auto owner_append = controller.append(append_entry("maintenance-owner-append"));
    auto reset = controller.reset_persistence_failure();
    expect(inspected == ava::app::AdmissionDisposition::RejectMaintenanceReservation && ava::app::to_string(inspected) == "rejected_maintenance_reservation" &&
               !second && !admitted && !owner_append && !reset,
           "reservation is serialized distinctly and atomically rejects duplicate reservations, admissions, owner appends, and persistence reset");

    auto moved = std::move(*reserved);
    expect(moved.active(), "maintenance reservation preserves ownership across move");
    moved = {};
    expect(!controller.snapshot().maintenance_reserved && controller.admit({.request_id = "after-maintenance"}).has_value(),
           "explicit release reopens ordinary admission");
  }
  {
    ava::app::SessionRunController controller(ephemeral_controller_target());
    auto active = controller.admit({.request_id = "active-blocks-maintenance"});
    expect(active.has_value() && !controller.reserve_maintenance().has_value(), "an active normal run prevents maintenance reservation acquisition");
  }
  {
    ava::app::SessionRunController controller(ephemeral_controller_target());
    controller.shutdown();
    expect(!controller.reserve_maintenance().has_value(), "shutdown controller rejects maintenance reservation acquisition");
  }

  for (int iteration = 0; iteration < 32; ++iteration)
  {
    ava::app::SessionRunController controller(ephemeral_controller_target());
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    int ready = 0;
    bool release = false;
    std::optional<ava::core::Result<ava::app::SessionMaintenanceReservation>> reservation_result;
    std::optional<ava::core::Result<ava::app::ActiveRunGuard>> admission_result;
    auto wait_for_start = [&] {
      std::unique_lock lock(gate_mutex);
      ++ready;
      gate_changed.notify_all();
      gate_changed.wait(lock, [&] { return release; });
    };
    std::jthread reserver([&] {
      wait_for_start();
      reservation_result.emplace(controller.reserve_maintenance());
    });
    std::jthread admitter([&] {
      wait_for_start();
      admission_result.emplace(controller.admit({.request_id = "reservation-race"}));
    });
    {
      std::unique_lock lock(gate_mutex);
      bool const both_ready = gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return ready == 2; });
      expect(both_ready, "reservation/admission race reaches its deterministic start barrier");
      release = true;
    }
    gate_changed.notify_all();
    reserver.join();
    admitter.join();
    bool const reservation_won = reservation_result && reservation_result->has_value();
    bool const admission_won = admission_result && admission_result->has_value();
    expect(reservation_won != admission_won, "reservation and normal admission have one final linearization winner");
  }

  {
    auto const root = create_empty_root("session-run-controller-maintenance-inflight");
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      std::mutex gate_mutex;
      std::condition_variable gate_changed;
      bool entered = false;
      bool release = false;
      store->set_before_append_identity_check_for_test([&] {
        std::unique_lock lock(gate_mutex);
        entered = true;
        gate_changed.notify_all();
        gate_changed.wait(lock, [&] { return release; });
      });
      auto target = persistent_controller_target(*store, *lease);
      if (target)
      {
        ava::app::SessionRunController controller(std::move(*target));
        auto owner = controller.owner_append_route();
        std::optional<ava::core::VoidResult> append_result;
        std::optional<ava::core::VoidResult> queued_result;
        std::jthread writer([&] { append_result.emplace(owner(append_entry("maintenance-inflight"))); });
        {
          std::unique_lock lock(gate_mutex);
          expect(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }), "owner append reaches in-flight persistence gate");
        }
        std::jthread queued_writer([&] { queued_result.emplace(owner(append_entry("maintenance-queued"))); });
        auto const queue_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (controller.snapshot().queued_appends < 2 && std::chrono::steady_clock::now() < queue_deadline)
          std::this_thread::yield();
        expect(controller.snapshot().queued_appends == 2 && !controller.reserve_maintenance().has_value(),
               "queued and in-flight owner appends jointly prevent maintenance reservation acquisition");
        {
          std::lock_guard lock(gate_mutex);
          release = true;
        }
        gate_changed.notify_all();
        writer.join();
        queued_writer.join();
        expect(append_result && append_result->has_value() && queued_result && queued_result->has_value() && controller.reserve_maintenance().has_value(),
               "maintenance can reserve only after every accepted append fully linearizes");
      }
    }
  }

  {
    auto const root = create_empty_root("session-run-controller-maintenance-failed");
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      store->set_append_write_for_test([](int, std::string_view) -> ssize_t {
        errno = EIO;
        return -1;
      });
      auto target = persistent_controller_target(*store, *lease);
      if (target)
      {
        ava::app::SessionRunController controller(std::move(*target));
        expect(!controller.append(append_entry("maintenance-failure")).has_value() && !controller.reserve_maintenance().has_value(),
               "latched persistence failure prevents maintenance reservation acquisition");
      }
    }
  }
}

void test_session_run_controller_authority_retirement_is_permanent()
{
  ava::app::SessionRunController controller(ephemeral_controller_target());
  auto pre_retirement = controller.admit({.request_id = "capture-stale-routes"});
  expect(pre_retirement.has_value(), "retirement fixture admits one generation before maintenance");
  if (!pre_retirement)
    return;
  auto stale_append = pre_retirement->append_route();
  auto stale_batch = pre_retirement->append_batch_route();
  auto stale_compaction = pre_retirement->compaction_append_route();
  auto owner_append = controller.owner_append_route();
  auto owner_batch = controller.owner_append_batch_route();
  pre_retirement = {};

  auto reservation = controller.reserve_maintenance();
  expect(reservation && controller.retire_authority(*reservation), "exclusive maintenance permanently retires controller authority");
  auto during = controller.admit({.request_id = "during-retirement"});
  auto during_owner = owner_append(append_entry("retired-owner-during"));
  expect(!during && !during_owner && during.error().message() == "runtime session authority was retired" &&
             during.error().format().find("reopen the session") != std::string::npos &&
             during.error().format().find("capture-stale-routes") == std::string::npos,
         "retired admission and copied owner routes return bounded content-free reopen guidance before reservation release");

  reservation = {};
  auto after = controller.admit({.request_id = "after-retirement"});
  auto after_owner_batch = owner_batch({append_entry("retired-owner-batch")});
  auto after_stale = stale_append(append_entry("retired-generation"));
  auto after_stale_batch = stale_batch({append_entry("retired-generation-batch")});
  auto after_stale_compaction = stale_compaction(compaction_entry("retired-compaction"), {}, nullptr);
  auto snapshot = controller.snapshot();
  expect(controller.authority_retired() && snapshot.authority_retired && !snapshot.maintenance_reserved &&
             controller.inspect_admission({.request_id = "inspection"}) == ava::app::AdmissionDisposition::RejectRetiredAuthority &&
             ava::app::to_string(ava::app::AdmissionDisposition::RejectRetiredAuthority) == "rejected_retired_authority" && !after && !after_owner_batch &&
             !after_stale && !after_stale_batch && !after_stale_compaction,
         "releasing maintenance cannot reactivate retired run, owner, batch, generation, or compaction authority");
}

void test_session_run_controller_compaction_compare_and_append()
{
  {
    auto const root = create_empty_root("session-run-controller-compaction-persistent");
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease)
      return;
    expect(store->append(*lease, append_entry("persistent-prefix")).has_value(), "persistent compaction CAS fixture seeds history");
    auto expected = store->load(*lease);
    auto target = persistent_controller_target(*store, *lease);
    if (!expected || !target)
      return;
    ava::app::SessionRunController controller(std::move(*target));
    auto exact = controller.append_compaction_if_snapshot_matches(compaction_entry("persistent-compaction"), *expected);
    auto after_exact = store->load(*lease);
    expect(exact && *exact == ava::session::SessionCompactionAppendResult::Appended && after_exact && after_exact->size() == 2,
           "persistent compaction CAS appends on an exact authoritative snapshot");

    if (!after_exact)
      return;
    auto stale = *after_exact;
    expect(controller.append(append_entry("persistent-growth")).has_value(), "persistent compaction CAS fixture advances history ordinarily");
    auto mismatch = controller.append_compaction_if_snapshot_matches(compaction_entry("must-not-append"), std::move(stale));
    auto after_mismatch = store->load(*lease);
    auto const compactions =
        after_mismatch ? std::ranges::count_if(*after_mismatch, [](auto const& entry) { return entry.type == ava::session::EntryType::Compaction; }) : 0;
    expect(mismatch && *mismatch == ava::session::SessionCompactionAppendResult::SnapshotMismatch && after_mismatch && compactions == 1 &&
               controller.append(append_entry("persistent-after-mismatch")),
           "persistent snapshot mismatch is successful, nonmutating, and non-latching");

    auto title_snapshot = store->load(*lease);
    if (!title_snapshot)
      return;
    auto automatic_title = ava::session::SessionEntry{.id = "automatic-title",
                                                      .parent_id = title_snapshot->back().id,
                                                      .type = ava::session::EntryType::SessionMetadata,
                                                      .timestamp = ava::session::now_timestamp(),
                                                      .data_json = "{\"actor\":\"auto-title\",\"generated_title\":\"Generated\"}"};
    expect(controller.append(std::move(automatic_title)).has_value(), "compaction CAS fixture appends context-neutral automatic title metadata");
    auto after_title = controller.append_compaction_if_snapshot_matches(compaction_entry("after-automatic-title"), std::move(*title_snapshot));
    expect(after_title && *after_title == ava::session::SessionCompactionAppendResult::Appended,
           "persistent compaction CAS accepts the validated trailing automatic-title suffix");

    auto manual_snapshot = store->load(*lease);
    if (!manual_snapshot)
      return;
    auto manual_title = ava::session::SessionEntry{.id = "manual-title",
                                                   .parent_id = manual_snapshot->back().id,
                                                   .type = ava::session::EntryType::SessionMetadata,
                                                   .timestamp = ava::session::now_timestamp(),
                                                   .data_json = "{\"actor\":\"manual\",\"name\":\"Manual\"}"};
    expect(controller.append(std::move(manual_title)).has_value(), "compaction CAS fixture appends context-affecting manual metadata");
    auto after_manual = controller.append_compaction_if_snapshot_matches(compaction_entry("after-manual-title"), std::move(*manual_snapshot));
    expect(after_manual && *after_manual == ava::session::SessionCompactionAppendResult::SnapshotMismatch,
           "persistent compaction CAS rejects a trailing manual metadata mismatch");

    auto cancel_snapshot = store->load(*lease);
    if (!cancel_snapshot)
      return;
    auto canceled = controller.append_compaction_if_snapshot_matches(compaction_entry("canceled-compaction"), *cancel_snapshot, [] { return true; });
    expect(!canceled && !append_error_context(canceled.error(), "append_commit_state") && controller.append(append_entry("persistent-after-cancel")),
           "conditional compaction cancellation before append does not latch persistence");

    auto admitted = controller.admit({.request_id = "compaction-generation"});
    if (admitted)
    {
      auto guard = std::move(*admitted);
      auto stale_route = guard.compaction_append_route();
      auto stale_snapshot = store->load(*lease);
      guard = ava::app::ActiveRunGuard{};
      auto stale_result = stale_snapshot ? stale_route(compaction_entry("stale-route-compaction"), std::move(*stale_snapshot), nullptr)
                                         : ava::core::Result<ava::session::SessionCompactionAppendResult>(std::unexpected(stale_snapshot.error()));
      expect(!stale_result && stale_result.error().message() == "stale run route",
             "conditional compaction active route rejects its stale immutable generation");
    }
  }

  {
    auto const root = create_empty_root("session-run-controller-compaction-ephemeral");
    auto store = ava::session::SessionStore::create_ephemeral(root / "workspace");
    if (!store)
      return;
    expect(store->append_ephemeral(append_entry("ephemeral-prefix")).has_value(), "ephemeral compaction CAS fixture seeds history");
    auto expected = store->load();
    auto target = ava::session::SessionAppendTarget::create_ephemeral(*store);
    if (!expected || !target)
      return;
    ava::app::SessionRunController controller(std::move(*target));
    auto exact = controller.append_compaction_if_snapshot_matches(compaction_entry("ephemeral-compaction"), *expected);
    auto stale = store->load();
    if (!stale)
      return;
    expect(controller.append(append_entry("ephemeral-growth")).has_value(), "ephemeral compaction CAS fixture advances history ordinarily");
    auto mismatch = controller.append_compaction_if_snapshot_matches(compaction_entry("ephemeral-must-not-append"), std::move(*stale));
    auto final_entries = store->load();
    auto const compactions =
        final_entries ? std::ranges::count_if(*final_entries, [](auto const& entry) { return entry.type == ava::session::EntryType::Compaction; }) : 0;
    expect(exact && *exact == ava::session::SessionCompactionAppendResult::Appended && mismatch &&
               *mismatch == ava::session::SessionCompactionAppendResult::SnapshotMismatch && final_entries && compactions == 1,
           "ephemeral compaction CAS has exact-match and mismatch parity with persistent sessions");
  }

  auto run_failure = [](std::string const& name, std::string_view expected_state) {
    auto const root = create_empty_root("session-run-controller-compaction-failure-" + name);
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease || !store->append(*lease, append_entry(name + "-prefix")))
      return false;
    auto expected = store->load(*lease);
    if (!expected)
      return false;
    int calls = 0;
    if (expected_state == "not_started")
    {
      store->set_append_write_for_test([&](int, std::string_view) -> ssize_t {
        ++calls;
        errno = EIO;
        return -1;
      });
    }
    else if (expected_state == "partial_or_unknown")
    {
      store->set_append_write_for_test([&](int fd, std::string_view bytes) -> ssize_t {
        ++calls;
        if (calls == 1)
          return ::write(fd, bytes.data(), std::max<std::size_t>(1, bytes.size() / 2));
        errno = EIO;
        return -1;
      });
    }
    else
    {
      store->set_append_write_for_test([&](int fd, std::string_view bytes) -> ssize_t {
        ++calls;
        return ::write(fd, bytes.data(), bytes.size());
      });
      store->set_after_append_write_for_test([] { throw 1; });
    }
    auto target = persistent_controller_target(*store, *lease);
    if (!target)
      return false;
    ava::app::SessionRunController controller(std::move(*target));
    auto failed = controller.append_compaction_if_snapshot_matches(compaction_entry(name + "-compaction"), std::move(*expected));
    auto blocked = controller.append(append_entry(name + "-blocked"));
    return !failed && append_error_context(failed.error(), "append_commit_state") == std::optional<std::string>(expected_state) && !blocked &&
           append_error_context(blocked.error(), "append_commit_state") == std::optional<std::string>(expected_state) &&
           controller.inspect_admission({.request_id = "blocked"}) == ava::app::AdmissionDisposition::RejectPersistenceFailure;
  };
  expect(run_failure("not-started", "not_started"), "compaction CAS not-started append failures latch with stable classification");
  expect(run_failure("partial", "partial_or_unknown"), "compaction CAS partial append failures latch and cannot be bypassed");
  expect(run_failure("committed", "committed_to_leased_inode"), "compaction CAS committed append failures latch with stable classification");
}

void test_session_run_controller_compaction_snapshot_lane()
{
  // Six ~768 KiB records byte-account above the 4 MiB ordinary append queue
  // budget while staying inside the default 8 MiB file / 1 MiB line read
  // limits, so the CAS comparison itself remains authoritative.
  std::size_t constexpr kOversizedEntries = 6;
  std::size_t constexpr kOversizedPadding = 768 * 1024;
  auto seed_history = [](ava::session::SessionStore& store, ava::session::SessionLease const& lease, std::string_view prefix, std::size_t count,
                         std::size_t padding_bytes) {
    std::string parent;
    for (std::size_t index = 0; index < count; ++index)
    {
      std::string entry_id(prefix);
      entry_id.push_back('-');
      entry_id += std::to_string(index);
      auto entry = append_entry(std::move(entry_id));
      entry.parent_id = parent;
      if (padding_bytes > 0)
        entry.data_json = "{\"padding\":\"" + std::string(padding_bytes, 'x') + "\"}";
      if (!store.append(lease, entry))
        return false;
      parent = entry.id;
    }
    return true;
  };
  auto create_owned_store = [](std::filesystem::path const& root) {
    auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    return std::pair<std::optional<ava::session::SessionStore>, std::optional<ava::session::SessionLease>>(
        store ? std::optional<ava::session::SessionStore>(std::move(*store)) : std::nullopt,
        lease ? std::optional<ava::session::SessionLease>(std::move(*lease)) : std::nullopt);
  };

  {
    auto const root = create_empty_root("session-run-controller-compaction-oversized-snapshot");
    auto [store, lease] = create_owned_store(root);
    expect(store && lease, "oversized snapshot fixture creates a persistent owned session");
    if (!store || !lease)
      return;
    expect(seed_history(*store, *lease, "oversized", kOversizedEntries, kOversizedPadding), "oversized snapshot fixture seeds an over-budget history");
    auto expected = store->load(*lease);
    auto target = persistent_controller_target(*store, *lease);
    if (!expected || !target)
      return;
    expect(controller_entries_bytes(*expected) > ava::app::kMaxSessionAppendQueueBytes,
           "oversized snapshot fixture byte-accounting exceeds the ordinary append queue budget");
    ava::app::SessionRunController controller(std::move(*target));
    auto exact = controller.append_compaction_if_snapshot_matches(compaction_entry("oversized-compaction"), *expected);
    auto after = store->load(*lease);
    auto drained = controller.snapshot();
    expect(exact && *exact == ava::session::SessionCompactionAppendResult::Appended && after && after->size() == kOversizedEntries + 1 &&
               after->back().type == ava::session::EntryType::Compaction && drained.queued_appends == 0 && drained.queued_append_bytes == 0,
           "compaction CAS carrying an over-budget expected snapshot reaches authoritative comparison and appends");
  }

  {
    auto const root = create_empty_root("session-run-controller-compaction-lane");
    auto [store, lease] = create_owned_store(root);
    expect(store && lease, "compaction lane fixture creates a persistent owned session");
    if (!store || !lease)
      return;
    expect(seed_history(*store, *lease, "lane", kOversizedEntries, kOversizedPadding), "compaction lane fixture seeds an over-budget history");
    auto expected = store->load(*lease);
    auto target = persistent_controller_target(*store, *lease);
    if (!expected || !target)
      return;
    expect(controller_entries_bytes(*expected) > ava::app::kMaxSessionAppendQueueBytes,
           "compaction lane fixture snapshot byte-accounting exceeds the ordinary append queue budget");
    ava::app::SessionRunController controller(std::move(*target));
    auto admitted = controller.admit({.request_id = "compaction-lane"});
    expect(admitted.has_value(), "compaction lane fixture admits an active run");
    if (!admitted)
      return;
    auto guard = std::move(*admitted);

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool in_flight = false;
    bool release_first = false;
    auto first_entry = compaction_entry("lane-first");
    auto const first_payload_bytes = controller_entry_bytes(first_entry);
    std::optional<ava::core::Result<ava::session::SessionCompactionAppendResult>> first_result;
    std::jthread first_writer([&] {
      first_result.emplace(controller.append_compaction_if_snapshot_matches(std::move(first_entry), *expected, [&] {
        std::unique_lock lock(gate_mutex);
        if (in_flight)
          return false;
        in_flight = true;
        gate_changed.notify_all();
        static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release_first; }));
        return false;
      }));
    });
    {
      std::unique_lock lock(gate_mutex);
      expect(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return in_flight; }),
             "first over-budget compaction is admitted and reaches its in-flight cancellation gate");
    }

    auto const gated = controller.snapshot();
    auto second = controller.append_compaction_if_snapshot_matches(compaction_entry("lane-second"), *expected);
    auto ordinary_entry = append_entry("lane-ordinary");
    auto const ordinary_payload_bytes = controller_entry_bytes(ordinary_entry);
    std::optional<ava::core::VoidResult> ordinary_result;
    std::jthread ordinary_writer([&] { ordinary_result.emplace(controller.append(std::move(ordinary_entry))); });
    auto const queue_deadline = ava::tests::now_plus_seconds(3);
    auto queued = controller.snapshot();
    while (queued.queued_appends < 2 && std::chrono::steady_clock::now() < queue_deadline)
    {
      std::this_thread::yield();
      queued = controller.snapshot();
    }
    {
      std::lock_guard lock(gate_mutex);
      release_first = true;
    }
    gate_changed.notify_all();
    first_writer.join();
    ordinary_writer.join();

    auto entries = store->load(*lease);
    auto drained = controller.snapshot();
    auto const admission = controller.inspect_admission({.request_id = "other"});
    expect(gated.queued_appends == 1 && gated.queued_append_bytes == first_payload_bytes,
           "an in-flight compaction ticket byte-accounts only its append payload, never the expected snapshot");
    expect(!second && second.error().message() == "session already has a pending compaction append" &&
               !append_error_context(second.error(), "append_commit_state"),
           "a second concurrent compaction is rejected with an actionable non-latching admission error");
    expect(queued.queued_appends == 2 && queued.queued_append_bytes == first_payload_bytes + ordinary_payload_bytes,
           "an ordinary append queues behind the in-flight compaction with exact payload-only byte accounting");
    expect(first_result && *first_result && **first_result == ava::session::SessionCompactionAppendResult::Appended && ordinary_result && *ordinary_result &&
               entries && entries->size() == kOversizedEntries + 2 && entries->at(kOversizedEntries).type == ava::session::EntryType::Compaction &&
               entries->back().id == "lane-ordinary" && !guard.stop_requested() && admission != ava::app::AdmissionDisposition::RejectPersistenceFailure &&
               drained.queued_appends == 0 && drained.queued_append_bytes == 0,
           "the gated compaction and queued ordinary append both persist in FIFO order without latching or stopping the run");

    auto after_first = store->load(*lease);
    expect(after_first && controller_entries_bytes(*after_first) > ava::app::kMaxSessionAppendQueueBytes,
           "compaction lane fixture history remains over the ordinary append queue budget after growth");
    auto released_after_success = after_first ? controller.append_compaction_if_snapshot_matches(compaction_entry("lane-after-success"), *after_first)
                                              : ava::core::Result<ava::session::SessionCompactionAppendResult>(std::unexpected(after_first.error()));
    auto mismatch = controller.append_compaction_if_snapshot_matches(compaction_entry("lane-stale"), *expected);
    auto after_mismatch = store->load(*lease);
    auto released_after_mismatch = after_mismatch ? controller.append_compaction_if_snapshot_matches(compaction_entry("lane-after-mismatch"), *after_mismatch)
                                                  : ava::core::Result<ava::session::SessionCompactionAppendResult>(std::unexpected(after_mismatch.error()));
    auto final_entries = store->load(*lease);
    auto const compactions =
        final_entries ? std::ranges::count_if(*final_entries, [](auto const& entry) { return entry.type == ava::session::EntryType::Compaction; }) : 0;
    expect(released_after_success && *released_after_success == ava::session::SessionCompactionAppendResult::Appended && mismatch &&
               *mismatch == ava::session::SessionCompactionAppendResult::SnapshotMismatch && released_after_mismatch &&
               *released_after_mismatch == ava::session::SessionCompactionAppendResult::Appended && compactions == 3,
           "the single-compaction lane is released after success and after an over-budget snapshot mismatch");
    auto outcome = guard.complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError});
    expect(outcome && outcome->reason == ava::app::StopReason::ProviderError, "compaction lane fixture run completes without a persistence override");
  }

  {
    auto const root = create_empty_root("session-run-controller-compaction-lane-drain");
    auto [store, lease] = create_owned_store(root);
    expect(store && lease, "compaction lane drain fixture creates a persistent owned session");
    if (!store || !lease)
      return;
    expect(store->append(*lease, append_entry("drain-prefix")).has_value(), "compaction lane drain fixture seeds history");
    auto expected = store->load(*lease);
    if (!expected)
      return;

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool entered = false;
    bool release_head = false;
    store->set_before_append_identity_check_for_test([&] {
      std::unique_lock lock(gate_mutex);
      if (entered)
        return;
      entered = true;
      gate_changed.notify_all();
      static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release_head; }));
    });
    auto control = std::make_shared<PartialAppendControl>();
    store->set_append_write_for_test([control](int fd, std::string_view bytes) { return control->write(fd, bytes); });
    auto target = persistent_controller_target(*store, *lease);
    if (!target)
      return;
    ava::app::SessionRunController controller(std::move(*target));

    std::optional<ava::core::VoidResult> head_result;
    std::jthread head_writer([&] { head_result.emplace(controller.append(append_entry("drain-head"))); });
    {
      std::unique_lock lock(gate_mutex);
      expect(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }), "drain fixture head append reaches its write gate");
    }
    std::optional<ava::core::Result<ava::session::SessionCompactionAppendResult>> queued_compaction;
    std::jthread compaction_writer(
        [&] { queued_compaction.emplace(controller.append_compaction_if_snapshot_matches(compaction_entry("drain-queued"), *expected)); });
    auto const queue_deadline = ava::tests::now_plus_seconds(3);
    auto queued = controller.snapshot();
    while (queued.queued_appends < 2 && std::chrono::steady_clock::now() < queue_deadline)
    {
      std::this_thread::yield();
      queued = controller.snapshot();
    }
    auto second = controller.append_compaction_if_snapshot_matches(compaction_entry("drain-second"), *expected);
    {
      std::lock_guard lock(gate_mutex);
      release_head = true;
    }
    gate_changed.notify_all();
    head_writer.join();
    compaction_writer.join();

    auto drained = controller.snapshot();
    control->fail.store(false, std::memory_order_release);
    auto recovered = controller.reset_persistence_failure();
    auto released = controller.append_compaction_if_snapshot_matches(compaction_entry("drain-after-recovery"), *expected);
    auto entries = store->load(*lease);
    auto const expected_state = std::optional<std::string>("partial_or_unknown");
    expect(queued.queued_appends == 2 && !second && second.error().message() == "session already has a pending compaction append" &&
               !append_error_context(second.error(), "append_commit_state") && head_result && !*head_result &&
               append_error_context(head_result->error(), "append_commit_state") == expected_state && queued_compaction && !*queued_compaction &&
               append_error_context(queued_compaction->error(), "append_commit_state") == expected_state && drained.queued_appends == 0 &&
               drained.queued_append_bytes == 0 && recovered && released && *released == ava::session::SessionCompactionAppendResult::Appended && entries &&
               entries->size() == 2 && entries->back().id == "drain-after-recovery",
           "a failing head terminally drains a queued compaction with the shared failure, a concurrent second compaction stays non-latching, and "
           "recovery releases the lane");
  }

  {
    auto const root = create_empty_root("session-run-controller-compaction-lane-shutdown");
    auto [store, lease] = create_owned_store(root);
    expect(store && lease, "compaction lane shutdown fixture creates a persistent owned session");
    if (!store || !lease)
      return;
    expect(store->append(*lease, append_entry("shutdown-prefix")).has_value(), "compaction lane shutdown fixture seeds history");
    auto expected = store->load(*lease);
    if (!expected)
      return;

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool entered = false;
    bool release_head = false;
    store->set_before_append_identity_check_for_test([&] {
      std::unique_lock lock(gate_mutex);
      if (entered)
        return;
      entered = true;
      gate_changed.notify_all();
      static_cast<void>(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return release_head; }));
    });
    auto target = persistent_controller_target(*store, *lease);
    if (!target)
      return;
    ava::app::SessionRunController controller(std::move(*target));
    auto owner = controller.owner_append_route();

    std::optional<ava::core::VoidResult> head_result;
    std::jthread head_writer([&] { head_result.emplace(controller.append(append_entry("shutdown-head"))); });
    {
      std::unique_lock lock(gate_mutex);
      expect(gate_changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }), "shutdown fixture head append reaches its write gate");
    }
    std::optional<ava::core::Result<ava::session::SessionCompactionAppendResult>> queued_compaction;
    std::jthread compaction_writer(
        [&] { queued_compaction.emplace(controller.append_compaction_if_snapshot_matches(compaction_entry("shutdown-queued"), *expected)); });
    auto const queue_deadline = ava::tests::now_plus_seconds(3);
    while (controller.snapshot().queued_appends < 2 && std::chrono::steady_clock::now() < queue_deadline)
      std::this_thread::yield();
    std::jthread shutdown_thread([&] { controller.shutdown(); });
    auto const close_deadline = ava::tests::now_plus_seconds(3);
    while (controller.inspect_admission({.request_id = "closing"}) != ava::app::AdmissionDisposition::RejectClosing &&
           std::chrono::steady_clock::now() < close_deadline)
      std::this_thread::yield();
    {
      std::lock_guard lock(gate_mutex);
      release_head = true;
    }
    gate_changed.notify_all();
    head_writer.join();
    compaction_writer.join();
    shutdown_thread.join();

    auto drained = controller.snapshot();
    expect(head_result && *head_result && queued_compaction && !*queued_compaction && queued_compaction->error().message() == "session has no active run" &&
               drained.queued_appends == 0 && drained.queued_append_bytes == 0 && !owner(append_entry("shutdown-stale")),
           "shutdown preserves the in-flight append, drains a queued compaction ticket as closing, and releases authority");
  }
}

void test_session_run_controller_bounds_and_reentrant_snapshot()
{
  ava::app::SessionRunController controller(ephemeral_controller_target());
  auto admitted = controller.admit({.request_id = "run"});
  expect(admitted.has_value(), "controller admits bounded queue fixture");
  if (!admitted)
    return;
  auto guard = std::move(*admitted);
  for (std::size_t index = 0; index < ava::app::kMaxSessionRunCommands; ++index)
    expect(controller.wake({.kind = ava::app::RunCommand::Kind::Wake, .correlation_id = {}, .message = "x"}).has_value(),
           "controller accepts each bounded command");
  expect(!controller.wake({.kind = ava::app::RunCommand::Kind::Wake, .correlation_id = {}, .message = "overflow"}).has_value(),
         "controller rejects command queue overflow");
  auto snapshot = controller.snapshot();
  expect(snapshot.active && snapshot.queued_commands == ava::app::kMaxSessionRunCommands,
         "snapshot is reentrant and observes controller state without callback locking");
}

}  // namespace

void run_session_run_controller_tests()
{
  test_session_run_controller_transitions_and_guard_release();
  test_session_run_controller_concurrent_admit_wake_stop();
  test_session_run_controller_generation_and_cancel_precedence();
  test_session_run_controller_committed_append_generation_rejects_stale_publication();
  test_session_run_controller_owner_and_generation_routes();
  test_runtime_session_destruction_does_not_own_background_coordinator();
  test_session_run_controller_admission_inspection_and_join();
  test_session_run_controller_stable_join_and_command_kinds();
  test_session_run_controller_join_survives_outcome_eviction();
  test_session_run_controller_reused_id_joins_active_generation();
  test_session_run_controller_effective_failure_and_stop_reentry();
  test_session_run_controller_recovers_persistent_and_ephemeral_targets();
  test_session_run_controller_failed_and_inflight_recovery();
  test_session_run_controller_shutdown_during_recovery_and_queued_append();
  test_session_run_controller_cross_thread_observer_shutdown_is_nonblocking();
  test_session_run_controller_observer_reset_is_immediate_for_unrelated_observer();
  test_session_run_controller_nested_controller_membership_is_stack_aware();
  test_session_run_controller_unrelated_observer_shutdown_without_active_append();
  test_session_run_controller_conditional_rejections_do_not_latch();
  test_session_run_controller_conditional_append_failures_latch_by_commit_state();
  test_session_run_controller_failure_drains_tickets_with_exact_accounting();
  test_session_run_controller_batch_is_one_ticket_and_latches_failure();
  test_session_run_controller_concurrent_fifo_appends();
  test_session_run_controller_routes_release_target_on_shutdown();
  test_session_run_controller_exclusive_maintenance_reservation();
  test_session_run_controller_authority_retirement_is_permanent();
  test_session_run_controller_compaction_compare_and_append();
  test_session_run_controller_compaction_snapshot_lane();
  test_session_run_controller_bounds_and_reentrant_snapshot();
}
