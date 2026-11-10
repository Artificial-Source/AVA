// Queues worker prompts and holds their workers until TUI callbacks retire.
// See docs/engineering/tui-architecture.md for the surrounding session lifetime.
#pragma once

#include "PendingPermissionRequest.h"
#include "PendingQuestionRequest.h"
#include "TuiTimerHandle.h"
#include "TuiWakeup.h"
#include "runtime_render_internal.h"
#include "ava/debug/print_members_on.h"
#include "ava/event/RuntimeEvent.h"
#include "ava/agent/question.h"
#include "ava/tui/terminal.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace ava::tui {

class RuntimePresentationState;

namespace detail {
// TUI-thread pure check: ignore only wheel events within the post-navigation quiet period.
[[nodiscard]] bool prompt_wheel_input_suppressed(terminal::Key key, std::optional<std::chrono::steady_clock::time_point> const& deadline,
                                                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
} // namespace detail

struct ComposerSnapshot;
class TuiFrontEnd;
class SelectorHost;
class RuntimeRenderer;
class TuiSessionGrantRegistry;

class TuiTimerRegistry;

namespace runtime_input {
struct RuntimeInput;
}

// TUI control result; only permission input can keep its request held while asking for run stop.
enum class PromptServiceOutcome
{
  None, // No active or newly started prompt owns this service turn.
  Active, // The TUI holds the worker while callbacks may borrow its stack.
  Completed, // Callbacks retired and the worker may consume its result.
  // Keep the request held: request run stop, then cancel_active(RunStop).
  RequestRunStop,
};

// TUI cancellation policy distinguishes interactive auditing from teardown release.
enum class PromptCancelReason
{
  RunStop, // Deny/cancel interactively after checking a ready automatic question answer.
  TerminalSignal, // Report interruption while preserving a ready automatic answer.
  Shutdown, // Retire callbacks and use prepared cancellation without automatic resolution.
};

// TUI caller learns whether an existing/queued prompt or search consumed the retained key.
enum class SearchInputPromptDispatchResult
{
  PromptServiced, // A prompt was serviced; never replay this key into a newly opened view.
  InputHandled, // Search ran after unlocking the queue; later arrivals await a later prompt check.
  InputFailed, // Search ran but its input/render path reported failure.
};

// TuiSession owns this coordinator. Only the TUI thread accesses prompt views and
// the active request; resolver workers enqueue under a mutex and wait for a result.
// A claim is the TUI's hold on that request: its worker remains blocked while TUI
// callbacks can borrow the worker's stack, even if a result is already stored.
// Close the queues, remove those callbacks, release the hold and join workers before
// destruction. Session collaborators and this stable address outlive every resolver call.
class RuntimePromptCoordinator final
{
 public:
  // TUI thread: bind session collaborators and member timers before accepting requests.
  // The optional selector and timer registry must outlive this stable callback target.
  RuntimePromptCoordinator(TuiFrontEnd const& frontend, RuntimePresentationState& presentation, TuiSessionGrantRegistry& session_grants,
                           RuntimeRenderer& renderer, TuiTimerRegistry& timer_registry, SelectorHost* selectors = nullptr);
  // Not copyable: one owner releases active requests, and resolver/timer callbacks retain its address.
  RuntimePromptCoordinator(RuntimePromptCoordinator const&) = delete;
  RuntimePromptCoordinator& operator=(RuntimePromptCoordinator const&) = delete;

  // TUI thread after resolver workers join: shut down residual UI state and timers.
  // The destructor alone cannot make outstanding raw-this resolver calls safe.
  ~RuntimePromptCoordinator();

  // The session and borrowed UI collaborators outlive every resolver invocation;
  // stop accepting requests and join workers before destroying this coordinator.
  // Reopen only after prior submission workers and active TUI prompts are gone.
  void open_admission();
  // TUI teardown stops new requests, releases the active request with prepared
  // cancellation, clears audit/screen data and cancels timers before destruction.
  void shutdown();
  // TUI setup: return a raw-this callback for workers. Its callers must finish before
  // coordinator destruction; a closed queue returns Deny without waiting.
  [[nodiscard]] ava::permissions::PermissionResolver permission_resolver();
  // TUI setup: return a raw-this worker callback with the same join requirement.
  // Accepted callers wait until an answer exists and the TUI releases its hold.
  [[nodiscard]] ava::agent::QuestionResolver question_resolver();
  // TUI thread: close the queue and interrupt every queued waiter before dropping
  // queue ownership. Coordinator cleanup allocates no queue storage; arbitrary
  // callback destruction runs unlocked. Active claims retire separately in service/cancel.
  void fail_pending_requests();
  // TUI thread: claim the next queued request before presenting it; an existing
  // claim is serviced first. Retained stop callbacks must bind stable owners that
  // outlive the claim; the before hook runs synchronously after the queue unlocks.
  [[nodiscard]] PromptServiceOutcome start_pending(std::function<bool()> stop_requested = {}, std::function<void()> before_prompt = {});
  // TUI thread: check automatic question completion before stop/signal cancellation.
  // Timers mark work due; this method invokes callbacks while the claim is held.
  [[nodiscard]] PromptServiceOutcome service();
  // TUI thread: recheck automatic completion before a retained key, then route only
  // to the current prompt. Permission Escape returns with its claim still held.
  [[nodiscard]] PromptServiceOutcome handle_input(runtime_input::RuntimeInput const& input);
  // TUI input stage, only after the real decoder reports Empty: flush remaining
  // native/replay input before allowing later keys to answer the painted prompt.
  // Empty also covers decoding errors. False means flushing failed and the claim
  // completed with Io; the caller must stop the session. Earlier batches discard
  // typeahead under the usual input budget, without blocking other loop stages.
  [[nodiscard]] bool input_drained();
  // TUI thread: normal cancellation checks a ready automatic answer first. Shutdown
  // uses the prepared result and retires callbacks before releasing the worker.
  [[nodiscard]] PromptServiceOutcome cancel_active(PromptCancelReason reason);
  // TUI thread only, after a failed batch paint and before shutdown. Retire any
  // borrowed prompt callbacks before waking its worker with the original Io error.
  // A failure must not run automatic question resolution and turn into success.
  void fail_active_render();
  // TUI thread: test whether a request is still held, even after its view is cleared.
  [[nodiscard]] bool has_active() const noexcept;
  // Either thread: synchronized queue readiness, separate from TUI-only active claim state.
  [[nodiscard]] bool has_pending_requests() const;
  // TUI thread: choose a queued prompt under the mutex, then invoke either prompt
  // startup or search after unlocking. A request queued during search waits for
  // a later prompt check; this retained key can never answer that new prompt.
  [[nodiscard]] SearchInputPromptDispatchResult dispatch_search_input_with_prompt_precedence(std::function<bool()> const& dispatch_search_input,
                                                                                             std::function<bool()> stop_requested = {},
                                                                                             std::function<void()> before_prompt = {});
  // Either thread: prepare a shared endpoint, swap ownership under its mutex,
  // and destroy the replaced target unlocked. Empty replacement does not allocate.
  // In-flight copies may still run; captured owners must survive those invocations.
  void set_audit_sink(ava::event::RuntimeEventSink sink);

  // Queued prompts, decisions and audit callbacks contain arbitrary user text.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Shared ownership keeps a request alive independently for its waiting worker,
  // queue and active TUI prompt. Only the request owns result synchronization.
  using ClaimedPromptRequest = std::variant<std::monostate, std::shared_ptr<PendingPermissionRequest>, std::shared_ptr<PendingQuestionRequest>>;
  // Owned permission outcome moved to a request only after its callbacks retire.
  using PermissionResult = ava::core::Result<ava::permissions::PermissionResolutionDecision>;
  // Owned answer/error moved to the sole waiting question resolver after retirement.
  using QuestionResult = ava::core::Result<ava::agent::QuestionAnswer>;

  // Worker thread: construct and enqueue under the queue lock, notify after unlock,
  // then wait without retaining the queue mutex. The coordinator must outlive the call.
  [[nodiscard]] ava::core::Result<ava::permissions::PermissionResolutionDecision> request_permission(ava::permissions::PermissionPrompt const& prompt);
  // Worker thread: copy the callback-bearing prompt outside the queue mutex, then
  // recheck acceptance before enqueueing. Closure during copying returns interruption.
  // Retain a worker share until resolution and retirement of borrowed TUI callbacks.
  [[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> request_question(ava::agent::QuestionPrompt const& prompt);
  // TUI thread with prompt_request_mutex_: decline closed queues, otherwise prefer
  // permission over question FIFO. Claim before unlocking so cancellation cannot
  // release the worker early; cleanup reentry must not claim interrupted requests.
  [[nodiscard]] ClaimedPromptRequest claim_pending_request_locked();
  // TUI thread after unlocking the queue: install the held request before invoking hooks
  // or presenting the prompt. Exceptions abandon only after retiring callbacks.
  [[nodiscard]] PromptServiceOutcome start_claimed_request(ClaimedPromptRequest request, std::function<bool()> stop_requested,
                                                           std::function<void()> before_prompt);
  // TUI thread: retain only shared endpoint ownership under its mutex, then copy,
  // invoke and destroy the callable unlocked. Each event gets a fresh callable
  // copy; replacement cannot destroy the retained source during that copy.
  void emit_prompt_audit(std::string status, std::string text, std::string permission_request_id = {}, std::string tool_name = {}, std::string reason = {},
                         std::string resolution_reason = {});
  // TUI thread with a permission claim: audit and reuse an eligible session grant,
  // otherwise prepare a default-deny view. A returned result completes immediately.
  [[nodiscard]] std::optional<PermissionResult> start_permission_prompt(ava::permissions::PermissionPrompt const& prompt);
  // TUI thread with a question claim: paint the view before arming automatic polling.
  // A render error is returned for completion; absence leaves the claim active.
  [[nodiscard]] std::optional<QuestionResult> start_question_prompt(ava::agent::QuestionPrompt const& prompt);
  // TUI thread with a permission claim: validate/store remembered or session choices
  // before returning an Allow decision. Storage/eligibility failure denies.
  [[nodiscard]] PermissionResult resolve_permission_choice(PermissionPromptChoice selected, std::string user_guidance = {});
  // TUI thread with a permission claim: sample queued cancellation under its
  // mutex, then call the borrowed stop predicate unlocked and observe signals.
  // Check before saving an Allow choice and again before releasing its worker.
  [[nodiscard]] bool permission_interrupted() const;
  // TUI thread with a question claim: audit dismissal and clear its view; caller
  // still owns callback retirement and final delivery of the cancellation error.
  [[nodiscard]] QuestionResult cancel_question();
  // TUI thread while claim protects the worker: invoke the optional borrowed callback.
  // No answer keeps the claim; successful discovery returns an answer or render error.
  [[nodiscard]] std::optional<QuestionResult> auto_resolve_question();
  // TUI thread: Escape keeps the request held until ActiveRun requests worker stop. Other choices
  // resolve through permission policy before releasing the request.
  [[nodiscard]] PromptServiceOutcome handle_permission_input(runtime_input::RuntimeInput const& input);
  // TUI thread: update selection/text before deriving an answer, audit/copy as needed,
  // then retire callbacks before delivering the owned result.
  [[nodiscard]] PromptServiceOutcome handle_question_input(runtime_input::RuntimeInput const& input);
  // TUI thread with permission claim: clear view and retire timers/callbacks before
  // request completion wakes the worker whose stack those callbacks may borrow.
  // Then wake the loop so deferred features can resume without a worker event.
  [[nodiscard]] PromptServiceOutcome complete_permission(PermissionResult result);
  // TUI thread with question claim: retire callbacks before releasing the worker,
  // then wake the loop to resume features held while the question was open.
  [[nodiscard]] PromptServiceOutcome complete_question(QuestionResult result);
  // TUI thread with claim: apply cancellation policy after the caller checked auto
  // resolution. Shutdown abandons directly and avoids interactive audit allocation.
  [[nodiscard]] PromptServiceOutcome cancel_active_after_auto(PromptCancelReason reason);
  // TUI thread: cancel timers and clear retained stop callback before transferring
  // the claim to a local value. The caller must then complete or abandon that request.
  [[nodiscard]] ClaimedPromptRequest retire_active_callbacks();
  // TUI thread: remove both prompt surfaces without deciding or releasing a request.
  void clear_prompt_projection();
  // TUI thread on teardown/unwind: retire callbacks, release with prepared cancellation,
  // then clear screen data so UI failure cannot strand the waiter.
  void abandon_active_request();
  // TUI timer callback: request a later service check; never invoke borrowed prompt callbacks
  // from timer dispatch itself.
  void mark_auto_resolution_due(TuiTimerHandle::Clock::time_point now);
  // TUI timer callback: restore wheel eligibility and reset burst state after the quiet period.
  void expire_wheel_suppression(TuiTimerHandle::Clock::time_point now);
  // TUI thread after accepted prompt wheel navigation: arm a quiet period to keep a
  // burst from changing multiple choices across redraws.
  void suppress_wheel_input();
  // TUI thread on prompt acquisition: close overview/stash presentation so it cannot
  // hide under the prompt and later reclaim input unexpectedly.
  void close_competing_selector();

  // Session borrow used on the TUI thread for persistent permission-rule decisions.
  TuiFrontEnd const& frontend_;
  // Session-owned screen data; workers never access prompt views.
  RuntimePresentationState& presentation_;
  // Session-owned grant registry; the TUI checks it before showing eligible permission prompts.
  TuiSessionGrantRegistry& session_grants_;
  // Session-owned TUI renderer; rendering never occurs under the audit mutex.
  RuntimeRenderer& renderer_;
  // Optional session borrow used only on the TUI thread. Prompt acquisition closes
  // overview/stash lists that would otherwise hide beneath its new surface.
  SelectorHost* selectors_ = nullptr;
  // Serializes worker enqueueing, queue removal and pending cancellation. All
  // callbacks run after unlock so reentry cannot deadlock this queue.
  mutable std::mutex prompt_request_mutex_;
  // Under queue mutex: shared requests stay alive for blocked permission workers
  // and take priority over queued questions when the TUI chooses its next prompt.
  std::deque<std::shared_ptr<PendingPermissionRequest>> pending_permission_requests_;
  // Under queue mutex: shared requests keep questions alive alongside their waiting workers.
  std::deque<std::shared_ptr<PendingQuestionRequest>> pending_question_requests_;
  // Under queue mutex: the TUI accepts requests between open_admission() and stop.
  bool accept_prompt_requests_ = false;
  // Under queue mutex: cancellation asks TUI service to release its active request;
  // it does not interrupt that claim or overwrite an automatic answer.
  bool cancellation_pending_ = false;
  // Serializes shared endpoint replacement/snapshots; no callable operation runs inside it.
  std::mutex prompt_audit_mutex_;
  // Coordinator and in-flight snapshots share the immutable stored callable so
  // replacement cannot destroy it during an unlocked copy. Captures protect their
  // own owners through invocation; either thread may release the last snapshot.
  std::shared_ptr<ava::event::RuntimeEventSink const> prompt_audit_sink_;

  // Only the TUI thread owns this hold. Cancellation is stored separately under
  // prompt_request_mutex_; service() checks automatic answers before handling it.
  // Retire every borrowed callback before completing and releasing its worker.
  ClaimedPromptRequest active_request_;
  // TUI thread sets this when claiming a prompt and clears it only after decoding
  // to Empty or retiring the claim. Pre-prompt keys must never become answers.
  bool input_quarantined_ = false;
  // TUI-retained stable-owner predicate installed with a claim and cleared before worker release.
  std::function<bool()> stop_requested_;
  // TUI caches current prompt eligibility; only a checked eligible choice can insert a grant.
  bool allow_session_available_ = false;
  // TUI caches whether this prompt can persist Allow; resolution rechecks this gate.
  bool allow_remember_available_ = false;
  // TUI caches whether Deny persistence is offered independently of Allow eligibility.
  bool deny_remember_available_ = false;
  // TUI timer/input marks discovery due; auto_resolve_question consumes the flag.
  bool auto_resolution_due_ = false;
  // TUI-only burst filter; prompt retirement or suppression expiry resets its history.
  WheelBurstGovernor wheel_governor_;
  // TUI-only quiet-period deadline set after navigation and cleared on non-wheel input/retirement.
  std::optional<TuiTimerHandle::Clock::time_point> wheel_suppression_deadline_;
  // Weak wake handle; notify after enqueueing, cancellation, or completion so the
  // next application turn can acquire prompts or resume features they had held.
  TuiWakeup wakeup_;
  // TUI timer marks automatic discovery due every 100 ms while a question
  // offers it. The borrowed registry and stable target outlive both timers.
  TuiTimerHandle auto_resolution_timer_;
  // TUI timer ends the wheel quiet period; disarm it before releasing a claim.
  TuiTimerHandle wheel_expiry_timer_;
};

}  // namespace ava::tui
