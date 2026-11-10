// Exercise prompt callback construction through public coordinator APIs.
#include "sys.h"
#include "tests/support/TuiEventLoopFixture.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/runtime_prompts_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/session_grants.h"
#include "ava/core/thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Coordinator = ava::tui::RuntimePromptCoordinator;
using Outcome = ava::tui::PromptServiceOutcome;
using CancelReason = ava::tui::PromptCancelReason;
// These are deadlock guards, not latency expectations; correct handshakes finish immediately.
constexpr auto kGuard = std::chrono::seconds(10);

// A broken cleanup must fail the isolated case instead of blocking in future destruction.
[[noreturn]] void abort_blocked_worker(char const* message)
{
  std::fprintf(stderr, "prompt callback locking: %s\n", message);
  std::abort();
}

// Own one named resolver worker; tests cancel its request before consuming its result.
class ResolverWorker final
{
 public:
  // The task owns its prompt/resolver values; its coordinator borrow outlives this owner.
  explicit ResolverWorker(std::future<bool> worker) : worker_(std::move(worker)) { }
  // An exceptional test exit remains bounded even if normal request cancellation was skipped.
  ~ResolverWorker()
  {
    if (worker_.valid() && !ready())
      abort_blocked_worker("resolver did not retire before its owner");
  }
  // Test-thread observation; readiness also proves request_question's local share is gone.
  [[nodiscard]] bool ready() const { return worker_.wait_for(kGuard) == std::future_status::ready; }
  // Consume only after a bounded readiness check, never through an unbounded get.
  [[nodiscard]] bool result()
  {
    if (!ready())
      abort_blocked_worker("resolver remained blocked after cancellation");
    return worker_.get();
  }
  // Internal-linkage future ownership is deliberately excluded from generated printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Owns the async task until its resolver has released all coordinator borrows.
  std::future<bool> worker_;
};

// Identify the synchronized API used to detect a lock retained by a callback operation.
enum class ReentryOperation
{
  ClearAudit, // Reenter the audit mutex through its supported either-thread setter.
  ReadQueue, // Reenter the request mutex through its supported either-thread observer.
};

// Lives before the fixture and retains the async future beyond the tested callback.
// A blocked copy/destructor returns after its guard, allowing the original lock to
// release. Only then does the test join the retained reentry task.
class ReentryProbe final
{
 public:
  // Obtain completion observation before any worker can enter the callable.
  ReentryProbe() : callback_finished_(callback_finished_promise_.get_future()) { }
  // Tests explicitly finish while the coordinator exists; this is a bounded fallback.
  ~ReentryProbe() { finish(); }
  // Setup runs before handing callback ownership to another thread.
  void arm(Coordinator& coordinator, ReentryOperation operation)
  {
    coordinator_ = &coordinator;
    operation_ = operation;
    state_.store(State::Armed);
  }
  // One copy or destructor wins the probe. No future is destroyed while it may be blocked.
  [[nodiscard]] bool run()
  {
    auto expected = State::Armed;
    if (!state_.compare_exchange_strong(expected, State::Running))
      return false;
    entered_.store(true);
    // The test joins this value-captured borrow before destroying the coordinator.
    worker_ = ava::core::make_async("prompt_lock_reentry", [coordinator = coordinator_, operation = operation_] {
      if (operation == ReentryOperation::ClearAudit)
        coordinator->set_audit_sink({});
      else
        static_cast<void>(coordinator->has_pending_requests());
    });
    completed_inside_.store(worker_.wait_for(kGuard) == std::future_status::ready);
    callback_finished_promise_.set_value();
    return completed_inside_.load();
  }
  // Allow the callback's entire guard, then inspect its future only after synchronization.
  void wait_for_callback()
  {
    if (callback_finished_.wait_for(2 * kGuard) != std::future_status::ready)
      abort_blocked_worker("expected callback copy did not finish");
  }
  // Retire the one-shot trigger before its coordinator can die. A callback that
  // already won the trigger must finish publishing worker_ before this thread reads it.
  void finish()
  {
    if (state_.exchange(State::Idle) == State::Running)
      wait_for_callback();
    if (worker_.valid())
    {
      if (worker_.wait_for(kGuard) != std::future_status::ready)
        abort_blocked_worker("reentry stayed blocked after the outer call returned");
      worker_.get();
    }
  }
  // Read after callback completion; both observations remain valid after future consumption.
  [[nodiscard]] bool entered() const { return entered_.load(); }
  // True means the synchronized reentry finished while the callback operation was on stack.
  [[nodiscard]] bool completed_inside() const { return completed_inside_.load(); }
  // Internal-linkage probe owns synchronization state, not printable application data.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Atomic ownership distinguishes an unused trigger from a callback still writing its future.
  enum class State
  {
    Idle, // No callback may start; teardown can inspect the retained future.
    Armed, // One callback may take the trigger after setup has published its borrows.
    Running, // Callback owns future publication until callback_finished_ becomes ready.
  };
  // Borrowed only between arm and finish; the fixture remains alive throughout that interval.
  Coordinator* coordinator_ = nullptr;
  // Immutable between arm and callback completion.
  ReentryOperation operation_ = ReentryOperation::ReadQueue;
  // arm/run/finish transfer the one-shot trigger; retirement cannot miss a callback that just started.
  std::atomic<State> state_{State::Idle};
  // Atomic observations cover callbacks running on resolver workers.
  std::atomic<bool> entered_ = false;
  // Records the behavioral assertion independently of eventual worker completion.
  std::atomic<bool> completed_inside_ = false;
  // The callback writes this owner; the test waits for callback_finished_ before reading it.
  std::future<void> worker_;
  // Publishes that the callback has finished using worker_ and released its test-side wait.
  std::promise<void> callback_finished_promise_;
  // Constructed once on the TUI thread, then observed without mutating the promise.
  std::future<void> callback_finished_;
};

// A test declares this after its fixture, while the probe itself precedes the fixture.
// Retirement therefore disarms and joins reentry before the coordinator dies, while
// callback targets destroyed by the fixture can still safely inspect the idle probe.
class ReentryScope final
{
 public:
  // Borrow a longer-lived probe; the coordinator fixture must already exist.
  explicit ReentryScope(ReentryProbe& probe) : probe_(probe) { }
  // Test thread, before fixture teardown: stop new callbacks and join any started task.
  ~ReentryScope() { probe_.finish(); }
  // Exactly one scope owns this retirement obligation.
  ReentryScope(ReentryScope const&) = delete;
  ReentryScope& operator=(ReentryScope const&) = delete;
  // This internal test lifetime guard has no generated printer.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Test-owned probe outlives both this scope and the coordinator fixture.
  ReentryProbe& probe_;
};

// A callback's state can change on invocation, but emission must mutate only its copy.
class AuditTarget final
{
 public:
  // The probe and observations are declared before the fixture and outlive every target.
  AuditTarget(ReentryProbe* copy_probe, ReentryProbe* destroy_probe, std::vector<unsigned>& calls)
      : copy_probe_(copy_probe), destroy_probe_(destroy_probe), calls_(&calls)
  {
  }
  // Copying a std::function target is unknown code even before operator() executes.
  AuditTarget(AuditTarget const& other)
      : copy_probe_(other.copy_probe_), destroy_probe_(other.destroy_probe_), calls_(other.calls_), calls_on_this_target_(other.calls_on_this_target_)
  {
    if (copy_probe_)
      static_cast<void>(copy_probe_->run());
  }
  // Moved-from temporaries cannot accidentally consume the destructor probe.
  AuditTarget(AuditTarget&& other) noexcept
      : copy_probe_(std::exchange(other.copy_probe_, nullptr)),
        destroy_probe_(std::exchange(other.destroy_probe_, nullptr)),
        calls_(other.calls_),
        calls_on_this_target_(other.calls_on_this_target_)
  {
  }
  // Replacement must retire arbitrary captured state outside the audit mutex.
  ~AuditTarget()
  {
    if (destroy_probe_)
      static_cast<void>(destroy_probe_->run());
  }
  // The real public audit path delivers the event; no synthetic emission is used.
  ava::core::VoidResult operator()(ava::event::RuntimeEvent const&)
  {
    calls_->push_back(++calls_on_this_target_);
    return {};
  }
  // Internal-linkage callback targets deliberately opt out of generated printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Optional earlier-lived probe; only the explicitly armed production copy consumes it.
  ReentryProbe* copy_probe_;
  // Optional earlier-lived probe; ordinary construction temporaries see it unarmed.
  ReentryProbe* destroy_probe_;
  // TUI-only observations outlive the fixture and all in-flight sink snapshots.
  std::vector<unsigned>* calls_;
  // Per-target mutable state: every fresh emission copy should observe its first call.
  unsigned calls_on_this_target_ = 0;
};

// Pause exactly the request's callback copy while the TUI closes admission.
class ConstructionGate final
{
 public:
  // Both futures exist before the resolver starts copying the prompt.
  ConstructionGate() : entered_(entered_promise_.get_future()), release_(release_promise_.get_future().share()) { }
  // Arm only after constructing/moving the worker's original callback target.
  void arm() { armed_.store(true); }
  // A regression holding the queue mutex must eventually release this wait itself.
  void pause_copy()
  {
    if (!armed_.exchange(false))
      return;
    entered_promise_.set_value();
    released_.store(release_.wait_for(kGuard) == std::future_status::ready);
  }
  // Test-thread rendezvous proves cancellation overlaps request construction.
  void wait_until_entered()
  {
    if (entered_.wait_for(kGuard) != std::future_status::ready)
      abort_blocked_worker("question construction never entered its copy gate");
  }
  // Open only after fail_pending_requests returned, while its closure remains authoritative.
  void release() { release_promise_.set_value(); }
  // Read after joining the resolver; false means cancellation was blocked by the copy.
  [[nodiscard]] bool released_without_timeout() const { return released_.load(); }
  // Internal-linkage rendezvous state has no generated print representation.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Cross-thread one-shot flag prevents unrelated copies from entering the gate.
  std::atomic<bool> armed_ = false;
  // Records whether cancellation could finish before the guard expired.
  std::atomic<bool> released_ = false;
  // The request-copy thread announces that it is paused before enqueueing.
  std::promise<void> entered_promise_;
  // Only the test thread consumes this observation.
  std::future<void> entered_;
  // The test thread owns admission closure and explicitly releases construction afterward.
  std::promise<void> release_promise_;
  // Shared future allows immutable waiting from the copied callable without borrowing a future.
  std::shared_future<void> release_;
};

class QuestionTarget final
{
 public:
  QuestionTarget(ReentryProbe* copy_probe = nullptr, ConstructionGate* gate = nullptr) : copy_probe_(copy_probe), gate_(gate) { }
  // This is the unknown-code operation performed while constructing PendingQuestionRequest.
  QuestionTarget(QuestionTarget const& other) : copy_probe_(other.copy_probe_), gate_(other.gate_)
  {
    if (copy_probe_)
      static_cast<void>(copy_probe_->run());
    if (gate_)
      gate_->pause_copy();
  }
  // Preserve original-versus-copy identity through moves and deactivate moved-from targets.
  QuestionTarget(QuestionTarget&& other) noexcept : copy_probe_(std::exchange(other.copy_probe_, nullptr)), gate_(std::exchange(other.gate_, nullptr)) { }
  // These questions have no automatic answer and remain queued until cancellation.
  [[nodiscard]] bool operator()() const { return false; }
  // Internal-linkage callable probes deliberately opt out of generated printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Earlier-lived optional observer of request construction's callback copy.
  ReentryProbe* copy_probe_;
  // Earlier-lived optional handshake controlling admission-versus-construction ordering.
  ConstructionGate* gate_;
};

// Stack-owned deadline target; its timer always dies before the target.
class EnqueueDeadline final
{
 public:
  // Timer dispatch records timeout without running any coordinator operation.
  void expire(ava::tui::TuiTimeoutHandler::Clock::time_point) { expired_ = true; }
  // TUI-only observation after actual loop waiting and timer service.
  [[nodiscard]] bool expired() const { return expired_; }
  // Internal-linkage timer targets have no generated printer.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Written and read only by the test's TUI thread.
  bool expired_ = false;
};

// Real coordinator composition; declaration order keeps the loop alive past every timer owner.
class PromptFixture final
{
 public:
  // Bind real UI owners before enabling the worker-facing resolver APIs.
  PromptFixture()
      : presentation_(frontend_.frontend(), {}),
        renderer_(event_loop_.tui_timeout_handler(), frontend_.frontend(), presentation_, composer_),
        coordinator_(*frontend_.frontend(), presentation_, grants_, renderer_, event_loop_.tui_timeout_handler(), event_loop_.wakeup())
  {
    coordinator_.open_admission();
  }
  // Tests finish every resolver before destruction; coordinator teardown retires its timers first.
  [[nodiscard]] Coordinator& coordinator() { return coordinator_; }
  // Each call follows one fresh worker launch and consumes its enqueue notification.
  // In the two-worker case the first notification is consumed before the second launch;
  // no rendering or other producer exists, so that next wake proves the second enqueue.
  [[nodiscard]] bool wait_for_enqueue()
  {
    auto& tui_timeout_handler = event_loop_.tui_timeout_handler();
    EnqueueDeadline deadline;
    auto timeout = tui_timeout_handler.create_timer(deadline, &EnqueueDeadline::expire);
    timeout.arm_after(kGuard);
    event_loop_.wait();
    tui_timeout_handler.expire(std::chrono::steady_clock::now());
    return !deadline.expired() && !event_loop_.terminal_hangup() && !event_loop_.terminal_error() && coordinator_.has_pending_requests();
  }
  // Internal-linkage composition owns opaque fixtures and deliberately opts out of printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Must precede every owner registering a member timer.
  tui_test_support::TuiEventLoopFixture event_loop_;
  // Backend/frontend session ownership outlives its display projection and renderer.
  tui_test_support::TuiFrontEndFixture frontend_;
  // TUI-only projection borrowed by both renderer and coordinator.
  ava::tui::RuntimePresentationState presentation_;
  // TUI-only editor borrowed until renderer destruction.
  ava::tui::Composer composer_;
  // Retires after the coordinator, before the editor, projection and loop.
  ava::tui::RuntimeRenderer renderer_;
  // TUI-only grant authority remains alive through coordinator teardown.
  ava::tui::TuiSessionGrantRegistry grants_;
  // All resolver callbacks borrow this stable address and finish before destruction.
  Coordinator coordinator_;
};

// Construct the original callback without copying it, then move the prompt into the worker.
ava::agent::QuestionPrompt question_prompt(QuestionTarget target)
{
  return {
      .header = "Callback lock test", .question = "Wait for cancellation", .options = {{.value = "done", .label = "Done"}}, .auto_resolve = std::move(target)};
}

// The worker owns its prompt; the resolver's coordinator borrow is joined inside its fixture.
ResolverWorker start_question(Coordinator& coordinator, ava::agent::QuestionPrompt prompt)
{
  return ResolverWorker(ava::core::make_async("prompt_lock_question", [resolver = coordinator.question_resolver(), prompt = std::move(prompt)] {
    auto const answer = resolver(prompt);
    return !answer && answer.error().category() == ava::core::ErrorCategory::Tool && answer.error().message() == "question prompt interrupted";
  }));
}

// Ordinary permissions reach the real prompt-audit path without callback-bearing payloads.
ResolverWorker start_permission(Coordinator& coordinator)
{
  return ResolverWorker(ava::core::make_async("prompt_lock_permission", [resolver = coordinator.permission_resolver()] {
    ava::permissions::PermissionPrompt prompt;
    prompt.operation = ava::permissions::Operation::RunCommand;
    prompt.mode = ava::core::Mode::Build;
    prompt.command = "echo callback-lock-test";
    prompt.tool_name = "bash";
    auto const answer = resolver(prompt);
    return answer && answer->resolution == ava::permissions::PermissionResolution::Deny;
  }));
}

// Bug: replacing an audit endpoint destroyed its previous std::function target under the audit mutex.
void test_audit_replacement_destructor()
{
  ReentryProbe probe;
  std::vector<unsigned> calls;
  PromptFixture fixture;
  ReentryScope probe_scope(probe);
  fixture.coordinator().set_audit_sink(AuditTarget(nullptr, &probe, calls));
  probe.arm(fixture.coordinator(), ReentryOperation::ClearAudit);
  fixture.coordinator().set_audit_sink({});
  probe.finish();
  expect(probe.entered() && probe.completed_inside(), "replaced audit target can reenter set_audit_sink during destruction");
}

// Bug: emission copied arbitrary audit-target state under the audit mutex before invoking it unlocked.
void test_audit_emission_copy()
{
  ReentryProbe probe;
  std::vector<unsigned> calls;
  {
    PromptFixture fixture;
    ReentryScope probe_scope(probe);
    auto worker = start_permission(fixture.coordinator());
    expect(fixture.wait_for_enqueue(), "permission is queued before auditing cancellation");
    expect(fixture.coordinator().start_pending() == Outcome::Active, "permission is active before installing the copy probe");
    fixture.coordinator().set_audit_sink(AuditTarget(&probe, nullptr, calls));
    probe.arm(fixture.coordinator(), ReentryOperation::ClearAudit);
    expect(fixture.coordinator().cancel_active(CancelReason::TerminalSignal) == Outcome::Completed, "public cancellation emits the audit event");
    probe.finish();
    fixture.coordinator().shutdown();
    expect(worker.result(), "audit reentry preserves permission denial");
  }
  expect(probe.entered() && probe.completed_inside(), "audit target copy can reenter set_audit_sink before returning");
  expect(calls == std::vector<unsigned>{1}, "in-flight copied sink still receives the event after replacing its stored endpoint");

  // Calling a shared stored target directly would incorrectly preserve mutable state across emissions.
  calls.clear();
  PromptFixture fixture;
  fixture.coordinator().set_audit_sink(AuditTarget(nullptr, nullptr, calls));
  auto second = start_permission(fixture.coordinator());
  expect(fixture.wait_for_enqueue(), "second permission reaches the public audit path");
  expect(fixture.coordinator().start_pending() == Outcome::Active, "permission startup emits the first copied-target event");
  expect(fixture.coordinator().cancel_active(CancelReason::TerminalSignal) == Outcome::Completed,
         "permission cancellation emits the second copied-target event");
  fixture.coordinator().shutdown();
  expect(second.result(), "mutable audit-target check retains denial");
  expect(calls == std::vector<unsigned>({1, 1}), "each audit emission invokes a fresh copy of the stored mutable target");
}

// Bug: question admission copied callback-bearing QuestionPrompt while holding the request mutex.
void test_question_copy_reentry()
{
  ReentryProbe probe;
  PromptFixture fixture;
  ReentryScope probe_scope(probe);
  auto prompt = question_prompt(QuestionTarget(&probe));
  probe.arm(fixture.coordinator(), ReentryOperation::ReadQueue);
  auto worker = start_question(fixture.coordinator(), std::move(prompt));
  probe.wait_for_callback();
  expect(fixture.wait_for_enqueue(), "question is enqueued after the callback copy returns");
  fixture.coordinator().fail_pending_requests();
  expect(worker.result(), "question copy probe remains cancelable");
  probe.finish();
  expect(probe.entered() && probe.completed_inside(), "question callback copy can reenter has_pending_requests before returning");
}

// Bug: moving question construction outside the lock requires admission to be checked again before enqueueing.
void test_question_construction_closes_admission()
{
  ConstructionGate gate;
  PromptFixture fixture;
  auto prompt = question_prompt(QuestionTarget(nullptr, &gate));
  gate.arm();
  auto worker = start_question(fixture.coordinator(), std::move(prompt));
  gate.wait_until_entered();
  fixture.coordinator().fail_pending_requests();
  gate.release();
  bool const rejected_before_cleanup = worker.ready();
  // A missing second admission check would enqueue after closure; release that waiter before joining.
  fixture.coordinator().fail_pending_requests();
  expect(worker.result(), "admission closing during construction returns the prepared question interruption");
  expect(gate.released_without_timeout(), "closing admission completes while callback construction is still paused");
  expect(rejected_before_cleanup, "question constructed across admission closure is rejected without a second cancellation");
  expect(!fixture.coordinator().has_pending_requests(), "closed admission leaves no question queued");
}

} // namespace

void run_tui_prompt_callback_lock_test_case(std::string_view test_case)
{
  if (test_case == "prompt_audit_replacement")
    test_audit_replacement_destructor();
  else if (test_case == "prompt_audit_copy")
    test_audit_emission_copy();
  else if (test_case == "prompt_question_copy")
    test_question_copy_reentry();
  else if (test_case == "prompt_question_admission")
    test_question_construction_closes_admission();
  else
    expect(false, "unknown prompt callback locking case: " + std::string(test_case));
}
