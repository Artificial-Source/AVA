// Exercise Mermaid callback reentry and allocation failure through application-owned renderers.
#include "sys.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/app/mermaid_render_coordinator.h"
#include "ava/app/tui/mermaid_tui_bridge.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// GNU ld --wrap=_Znwm preserves ordinary allocation while exposing one scoped test fault.
extern "C" void* mermaid_real_allocate(std::size_t size) asm("__real__Znwm");
// This callthrough wrapper is test-only; it neither replaces nor directly calls operator new.
extern "C" void* mermaid_wrap_allocate(std::size_t size) asm("__wrap__Znwm");

// Observe the external bridge-to-coordinator shutdown boundary without replacing its join.
extern "C" void mermaid_real_shutdown(ava::app::MermaidRenderCoordinator*) noexcept asm("__real__ZN3ava3app24MermaidRenderCoordinator8shutdownEv");
// Only the explicitly registered shutdown thread signals the test gate.
extern "C" void mermaid_wrap_shutdown(ava::app::MermaidRenderCoordinator*) noexcept asm("__wrap__ZN3ava3app24MermaidRenderCoordinator8shutdownEv");

namespace {

// A lock regression cannot be unwound safely while another thread still borrows its owner.
[[noreturn]] void fail_notification_deadline()
{
  std::fputs("Mermaid notification test exceeded its callback or shutdown deadline\n", stderr);
  std::_Exit(EXIT_FAILURE);
}

// Keep direct synchronous reentry and destructor joins bounded even if production code deadlocks.
class NotificationWatchdog final
{
 public:
  // Start only after the synchronization borrowed by the watchdog has been initialized.
  NotificationWatchdog()
      : worker_(ava::core::JoinThread::create("mermaid_notification_watchdog", [this, deadline = ava::tests::now_plus_seconds(15)] {
          std::unique_lock lock(mutex_);
          if (!changed_.wait_until(lock, deadline, [this] { return finished_; }))
            fail_notification_deadline();
        }))
  {
  }

  // Wake and join the watchdog before its borrowed synchronization is destroyed.
  ~NotificationWatchdog()
  {
    {
      std::lock_guard lock(mutex_);
      finished_ = true;
    }
    changed_.notify_all();
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Protects the completion latch shared with the watchdog thread.
  std::mutex mutex_;
  // Owns the wait channel until the last-declared worker joins.
  std::condition_variable changed_;
  // Published by test-thread teardown while holding mutex_.
  bool finished_ = false;
  // Destruction joins before the state borrowed by its callback is destroyed.
  ava::core::JoinThread worker_;
};

// A one-shot predicate gate establishes callback/shutdown ordering without success sleeps.
class NotificationGate final
{
 public:
  // Publish the gate before waking every waiter.
  void open()
  {
    {
      std::lock_guard lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }

  // Bound fixture ordering failures before returning to borrowed-owner teardown.
  void wait()
  {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_until(lock, ava::tests::now_plus_seconds(10), [this] { return open_; }))
      fail_notification_deadline();
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Serializes publication and observation of the one-way gate.
  std::mutex mutex_;
  // Wait channel used only while mutex_ protects the predicate.
  std::condition_variable changed_;
  // Protected one-way latch; repeated open calls are harmless.
  bool open_ = false;
};

// Borrowed only on the external shutdown thread until its noexcept join returns.
// The fixture owns this gate until that thread has joined.
thread_local NotificationGate* shutdown_observation = nullptr;

// Collect owned callback payloads while allowing producer and test threads to proceed independently.
template <typename Completion>
class CompletionLog final
{
 public:
  // Move already-drained values into test-owned storage before notifying the waiting test.
  void append(std::vector<Completion> completions)
  {
    {
      std::lock_guard lock(mutex_);
      completions_.insert(completions_.end(), std::make_move_iterator(completions.begin()), std::make_move_iterator(completions.end()));
    }
    changed_.notify_all();
  }

  // Wait for explicit payload publication, then return an owned assertion snapshot.
  [[nodiscard]] std::vector<Completion> wait_for_size(std::size_t count)
  {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_until(lock, ava::tests::now_plus_seconds(10), [this, count] { return completions_.size() >= count; }))
      fail_notification_deadline();
    return completions_;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Owns all synchronization for the collected callback payloads.
  std::mutex mutex_;
  // Signals the size predicate after append publishes its owned values.
  std::condition_variable changed_;
  // Protected result copies; no coordinator or frontend storage is borrowed.
  std::vector<Completion> completions_;
};

// Echo bytes with the system cat program. These tests cover coordinator locking;
// they do not need the separate process-protocol helper or its debug libraries.
ava::app::MermaidRenderConfiguration notification_helper_configuration()
{
  return {.epoch = 1, .enabled = true, .argv = {"/bin/cat"}};
}

// Retain callback targets through explicit worker shutdown even when a test returns early.
class NotificationBridge final
{
 public:
  // Shared bridge ownership is required by the exported callbacks, which retain the same owner.
  NotificationBridge()
      : bridge_(ava::app::MermaidTuiBridge::create(
            {.enabled = true, .enabled_configured = true, .argv = {"/bin/cat"}, .argv_configured = true, .unknown_fields = {}})),
        callbacks_(bridge_->tui_bridge())
  {
  }

  // Join while callback-borrowed fixture members remain alive; worker callbacks never shut themselves down.
  ~NotificationBridge() { bridge_->shutdown(); }

  // Borrow the stable application owner while this fixture remains alive through shutdown.
  [[nodiscard]] ava::app::MermaidTuiBridge& bridge() const { return *bridge_; }
  // Borrow the callback bundle; notifier captures borrow this fixture rather than forming an ownership cycle.
  [[nodiscard]] ava::tui::TuiMermaidRenderBridge const& callbacks() const { return callbacks_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Shared ownership matches the bridge's callback API; no notifier captures another owning reference.
  std::shared_ptr<ava::app::MermaidTuiBridge> bridge_;
  // Callback owners and notifier borrows remain alive until the destructor joins bridge work.
  ava::tui::TuiMermaidRenderBridge callbacks_;
};

// Validate both coordinator queries and drains from immediate publication on the caller thread.
void test_coordinator_immediate_notification_reentry()
{
  NotificationWatchdog watchdog;
  CompletionLog<ava::app::MermaidRenderCompletion> completions;
  auto coordinator = ava::app::MermaidRenderCoordinator::create({.epoch = 1, .enabled = false, .argv = {}});
  if (!ava::tests::require(coordinator.has_value(), "notification fallback coordinator starts"))
    return;
  // Both borrowed targets precede coordinator destruction, which joins before they can retire.
  (*coordinator)->set_completion_notifier([owner = coordinator->get(), log = &completions] {
    auto const before = owner->stats();
    auto drained = owner->take_completions();
    expect(before.completion_count == drained.size() && !drained.empty(), "fallback notifier can query and drain the published coordinator completion");
    log->append(std::move(drained));
  });
  auto const disabled = (*coordinator)->enqueue({.identity = 1, .config_epoch = 1, .source = "disabled diagram"});
  auto const oversized = (*coordinator)->enqueue({.identity = 2, .config_epoch = 1, .source = std::string(ava::app::kMaxMermaidSourceBytes + 1, 'x')});
  auto const observed = completions.wait_for_size(2);
  expect(disabled == ava::app::MermaidEnqueueResult::CompletedFallback && oversized == ava::app::MermaidEnqueueResult::CompletedFallback &&
             observed.size() == 2 && observed[0].identity == 1 && observed[0].outcome == ava::app::MermaidRenderOutcome::Disabled &&
             observed[1].identity == 2 && observed[1].outcome == ava::app::MermaidRenderOutcome::SourceTooLarge &&
             (*coordinator)->stats().completion_count == 0,
         "synchronous fallback callbacks drain each published identity exactly once");
}

// Freeze the worker inside one callback to deterministically exercise queued cancellation and a later cache hit.
void test_coordinator_worker_cancel_and_cache_notification_reentry()
{
  NotificationWatchdog watchdog;
  NotificationGate entered;
  NotificationGate resume;
  NotificationGate returned;
  CompletionLog<ava::app::MermaidRenderCompletion> completions;
  auto coordinator = ava::app::MermaidRenderCoordinator::create(notification_helper_configuration());
  if (!ava::tests::require(coordinator.has_value(), "notification worker coordinator starts"))
    return;
  (*coordinator)
      ->set_completion_notifier([owner = coordinator->get(), log = &completions, entered_gate = &entered, resume_gate = &resume, returned_gate = &returned] {
        auto const before = owner->stats();
        auto drained = owner->take_completions();
        auto const first = std::ranges::any_of(drained, [](auto const& completion) { return completion.identity == 10; });
        expect(before.completion_count == drained.size() && !drained.empty(), "worker, cancel, and cache callbacks can query and drain their coordinator");
        log->append(std::move(drained));
        if (first)
        {
          entered_gate->open();
          resume_gate->wait();
          returned_gate->open();
        }
      });
  auto const queued = (*coordinator)->enqueue({.identity = 10, .config_epoch = 1, .source = "accepted worker diagram"});
  entered.wait();
  auto const second = (*coordinator)->enqueue({.identity = 11, .config_epoch = 1, .source = "queued cancellation diagram"});
  auto const canceled = (*coordinator)->cancel(11, 1);
  resume.open();
  returned.wait();
  auto const cached = (*coordinator)->enqueue({.identity = 12, .config_epoch = 1, .source = "accepted worker diagram"});
  auto const observed = completions.wait_for_size(3);
  expect(queued == ava::app::MermaidEnqueueResult::Queued && second == ava::app::MermaidEnqueueResult::Queued && canceled &&
             cached == ava::app::MermaidEnqueueResult::CompletedFromCache && observed.size() == 3 && observed[0].identity == 10 &&
             observed[0].outcome == ava::app::MermaidRenderOutcome::Accepted && observed[0].text == "accepted worker diagram" && observed[1].identity == 11 &&
             observed[1].outcome == ava::app::MermaidRenderOutcome::Canceled && observed[2].identity == 12 &&
             observed[2].outcome == ava::app::MermaidRenderOutcome::Accepted && observed[2].from_cache,
         "worker publication, queued cancellation, and cache publication permit ordinary coordinator reentry");
}

// Replacement can synchronously notify backlog while the original callback's retained snapshot stays alive.
void test_coordinator_notifier_replacement_backlog_and_shutdown()
{
  NotificationWatchdog watchdog;
  CompletionLog<ava::app::MermaidRenderCompletion> completions;
  auto coordinator = ava::app::MermaidRenderCoordinator::create({.epoch = 1, .enabled = false, .argv = {}});
  if (!ava::tests::require(coordinator.has_value(), "replacement notification coordinator starts"))
    return;
  auto const queued = (*coordinator)->enqueue({.identity = 20, .config_epoch = 1, .source = "backlog"});
  // The callback exclusively owns this sentinel; weak observation proves replacement retains only the active snapshot.
  auto sentinel = std::make_shared<int>(42);
  std::weak_ptr<int> retired = sentinel;
  (*coordinator)->set_completion_notifier([owner = coordinator->get(), log = &completions, sentinel = std::move(sentinel)] {
    expect(owner->stats().completion_count == 1, "late coordinator registration sees existing backlog");
    owner->set_completion_notifier([owner, log] {
      auto const before = owner->stats();
      auto drained = owner->take_completions();
      expect(before.completion_count == drained.size() && !drained.empty(), "replacement notifier drains backlog through ordinary reentry");
      log->append(std::move(drained));
    });
    expect(*sentinel == 42 && owner->stats().completion_count == 0, "original notifier capture survives reentrant replacement until invocation returns");
  });
  auto const next = (*coordinator)->enqueue({.identity = 21, .config_epoch = 1, .source = "next"});
  auto const observed = completions.wait_for_size(2);
  expect(queued == ava::app::MermaidEnqueueResult::CompletedFallback && next == ava::app::MermaidEnqueueResult::CompletedFallback && retired.expired() &&
             observed.size() == 2 && observed[0].identity == 20 && observed[1].identity == 21,
         "replacement notifier receives backlog and future publications without retaining the original callback");
  (*coordinator)->shutdown();
  // Shared capture lifetime makes retained registration observable without invoking any stopped worker.
  auto late = std::make_shared<int>(1);
  std::weak_ptr<int> rejected = late;
  (*coordinator)->set_completion_notifier([late = std::move(late)] { expect(!late, "a coordinator notifier registered after shutdown must never run"); });
  expect(rejected.expired(), "coordinator registration after shutdown does not retain the notifier");
}

// Synchronous oversized fallback also exercises the bridge mutex and reentrant notifier registration.
void test_bridge_immediate_replacement_and_backlog_notification_reentry()
{
  NotificationWatchdog watchdog;
  CompletionLog<ava::tui::TuiMermaidRenderCompletion> completions;
  NotificationBridge fixture;
  auto replacement = [owner = &fixture, log = &completions] {
    auto const configuration = owner->bridge().presentation_configuration();
    auto drained = owner->callbacks().drain();
    expect(configuration.enabled && drained && drained->size() == 1, "replacement bridge notifier queries configuration and drains published fallback");
    if (drained)
      log->append(std::move(*drained));
  };
  fixture.callbacks().set_completion_notifier([owner = &fixture, replacement] {
    auto const configuration = owner->bridge().presentation_configuration();
    expect(configuration.enabled, "immediate bridge callback can query presentation configuration");
    // Leave the completion pending so replacement immediately reenters registration and drains that backlog.
    owner->callbacks().set_completion_notifier(replacement);
  });
  auto const epoch = fixture.callbacks().config_epoch;
  auto const first = fixture.callbacks().enqueue({.identity = 30, .config_epoch = epoch, .source = std::string(ava::app::kMaxMermaidSourceBytes + 1, 'x')});
  fixture.callbacks().set_completion_notifier({});
  auto const second = fixture.callbacks().enqueue({.identity = 31, .config_epoch = epoch, .source = std::string(ava::app::kMaxMermaidSourceBytes + 1, 'x')});
  fixture.callbacks().set_completion_notifier(replacement);
  auto const observed = completions.wait_for_size(2);
  expect(first == ava::tui::TuiMermaidEnqueueResult::Accepted && second == ava::tui::TuiMermaidEnqueueResult::Accepted && observed.size() == 2 &&
             observed[0].identity == 30 && observed[1].identity == 31 && !observed[0].accepted && !observed[1].accepted && observed[0].text.empty() &&
             observed[1].text.empty(),
         "bridge fallback enqueue and registration release bridge serialization before callback reentry");
  fixture.bridge().shutdown();
  // Shared capture lifetime reveals registration retention independently of whether shutdown has any backlog.
  auto late = std::make_shared<int>(1);
  std::weak_ptr<int> rejected = late;
  fixture.callbacks().set_completion_notifier([late = std::move(late)] { expect(!late, "a bridge notifier registered after shutdown must never run"); });
  expect(rejected.expired(), "bridge registration after shutdown does not retain the notifier");
}

// Hold the first worker notification so cancellation has a deterministically queued identity to retire.
void test_bridge_worker_and_cancel_notification_reentry()
{
  NotificationWatchdog watchdog;
  NotificationGate entered;
  NotificationGate resume;
  CompletionLog<ava::tui::TuiMermaidRenderCompletion> completions;
  NotificationBridge fixture;
  fixture.callbacks().set_completion_notifier([owner = &fixture, log = &completions, entered_gate = &entered, resume_gate = &resume] {
    auto const configuration = owner->bridge().presentation_configuration();
    auto drained = owner->callbacks().drain();
    expect(configuration.enabled && drained && drained->size() == 1, "worker and cancel bridge callbacks can query presentation and drain completions");
    bool const first = drained && std::ranges::any_of(*drained, [](auto const& completion) { return completion.identity == 40; });
    if (drained)
      log->append(std::move(*drained));
    if (first)
    {
      entered_gate->open();
      resume_gate->wait();
    }
  });
  auto const epoch = fixture.callbacks().config_epoch;
  auto const first = fixture.callbacks().enqueue({.identity = 40, .config_epoch = epoch, .source = "accepted bridge diagram"});
  entered.wait();
  auto const second = fixture.callbacks().enqueue({.identity = 41, .config_epoch = epoch, .source = "cancel bridge diagram"});
  auto const canceled = fixture.callbacks().cancel(41, epoch);
  resume.open();
  auto const observed = completions.wait_for_size(2);
  expect(first == ava::tui::TuiMermaidEnqueueResult::Accepted && second == ava::tui::TuiMermaidEnqueueResult::Accepted && canceled && observed.size() == 2 &&
             observed[0].identity == 40 && observed[0].accepted && observed[0].text == "accepted bridge diagram" && observed[1].identity == 41 &&
             !observed[1].accepted && observed[1].text.empty(),
         "bridge worker completion and synchronous cancellation permit ordinary notifier reentry");
}

// External shutdown must release the bridge mutex before joining a worker whose callback reenters the bridge.
void test_bridge_shutdown_during_reentrant_worker_notification()
{
  NotificationWatchdog watchdog;
  NotificationGate entered;
  NotificationGate resume;
  NotificationGate returned;
  NotificationGate shutdown_started;
  NotificationGate shutdown_finished;
  NotificationBridge fixture;
  fixture.callbacks().set_completion_notifier([owner = &fixture, entered_gate = &entered, resume_gate = &resume, returned_gate = &returned] {
    entered_gate->open();
    resume_gate->wait();
    auto const configuration = owner->bridge().presentation_configuration();
    auto drained = owner->callbacks().drain();
    expect(!configuration.enabled && drained && drained->empty(), "worker callback can reenter bridge after the shutdown latch closes admission");
    returned_gate->open();
  });
  auto const result = fixture.callbacks().enqueue({.identity = 50, .config_epoch = fixture.callbacks().config_epoch, .source = "shutdown diagram"});
  entered.wait();
  // This external owning thread joins the renderer; its callback never performs shutdown or joins itself.
  auto shutdown_worker =
      ava::core::JoinThread::create("mermaid_external_shutdown", [owner = &fixture, started_gate = &shutdown_started, finished_gate = &shutdown_finished] {
        // The callthrough boundary opens the gate only after the bridge closes
        // admission and reaches coordinator shutdown; no polling races that step.
        shutdown_observation = started_gate;
        owner->bridge().shutdown();
        shutdown_observation = nullptr;
        finished_gate->open();
      });
  shutdown_started.wait();
  resume.open();
  returned.wait();
  shutdown_finished.wait();
  shutdown_worker.join();
  expect(result == ava::tui::TuiMermaidEnqueueResult::Accepted, "external bridge shutdown joins after the reentrant worker callback returns");
}

// Allocation failure belongs only to the test thread's explicitly armed drain operation.
struct AllocationFaultState
{
  // The first allocation disarms the fault before throwing, so exception handling can allocate normally.
  bool armed = true;
  // Counts actual intercepted failures rather than assuming the intended operation allocated.
  std::size_t failures = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Non-owning test-thread pointer; each scoped fault restores its predecessor before its state dies.
thread_local AllocationFaultState* active_allocation_fault = nullptr;

// Restrict allocator interception to one call and retire it even when the call unexpectedly throws.
class ScopedAllocationFault final
{
 public:
  // Install initialized stack-owned fault state without performing an allocation.
  ScopedAllocationFault() : previous_(std::exchange(active_allocation_fault, &state_)) { }
  // Restore the previous fault scope before this scope's state can retire.
  ~ScopedAllocationFault() { active_allocation_fault = previous_; }
  // Read only on this scope's owning test thread after the drain returns.
  [[nodiscard]] std::size_t failures() const { return state_.failures; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Stack-owned fault accounting borrowed by the linker wrapper only within this scope.
  AllocationFaultState state_;
  // Previous test-thread borrow, restored on every exit path.
  AllocationFaultState* previous_;
};

// Reserving projection capacity must fail before taking the coordinator's pending completions.
void test_bridge_projection_allocation_failure_preserves_completion()
{
  NotificationWatchdog watchdog;
  NotificationBridge fixture;
  auto const result = fixture.callbacks().enqueue(
      {.identity = 60, .config_epoch = fixture.callbacks().config_epoch, .source = std::string(ava::app::kMaxMermaidSourceBytes + 1, 'x')});
  std::optional<std::vector<ava::tui::TuiMermaidRenderCompletion>> failed;
  std::size_t injected = 0;
  {
    ScopedAllocationFault fault;
    failed = fixture.callbacks().drain();
    injected = fault.failures();
  }
  auto recovered = fixture.callbacks().drain();
  auto empty = fixture.callbacks().drain();
  expect(result == ava::tui::TuiMermaidEnqueueResult::Accepted && injected == 1 && !failed && recovered && recovered->size() == 1 &&
             recovered->front().identity == 60 && !recovered->front().accepted && empty && empty->empty(),
         "projection reserve failure returns retryable Busy and preserves the original completion for one successful drain");
}

}  // namespace

// Reentry is released at the actual bridge handoff, while the real coordinator
// still performs all cancellation and joining. Other shutdown calls just forward.
extern "C" void mermaid_wrap_shutdown(ava::app::MermaidRenderCoordinator* coordinator) noexcept
{
  if (shutdown_observation)
    shutdown_observation->open();
  mermaid_real_shutdown(coordinator);
}

// Only the armed test-thread scope injects a fault; helper and renderer thread allocations always call through.
extern "C" void* mermaid_wrap_allocate(std::size_t size)
{
  if (active_allocation_fault != nullptr && active_allocation_fault->armed)
  {
    active_allocation_fault->armed = false;
    ++active_allocation_fault->failures;
    throw std::bad_alloc{};
  }
  return mermaid_real_allocate(size);
}

// Keep notification regressions independently selectable from broader helper/process tests.
void run_mermaid_notification_tests()
{
  ava::tests::HermeticTestEnvironment environment("mermaid-notification-tests-");
  test_coordinator_immediate_notification_reentry();
  test_coordinator_worker_cancel_and_cache_notification_reentry();
  test_coordinator_notifier_replacement_backlog_and_shutdown();
  test_bridge_immediate_replacement_and_backlog_notification_reentry();
  test_bridge_worker_and_cancel_notification_reentry();
  test_bridge_shutdown_during_reentrant_worker_notification();
  test_bridge_projection_allocation_failure_preserves_completion();
}
