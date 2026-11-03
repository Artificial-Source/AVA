// Coordinates worker prompt rendezvous and TUI modal claims; see docs/engineering/tui-architecture.md.
#pragma once

#include "PendingPermissionRequest.h"
#include "PendingQuestionRequest.h"
#include "TuiTimer.h"
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

class TuiEventLoop;

namespace runtime_input {
struct RuntimeInput;
}

// TUI control result; only permission input can retain a claim while requesting run stop.
enum class PromptServiceOutcome
{
  None, // No active or newly claimed prompt owns this service turn.
  Active, // The claim still protects worker-borrowed callbacks.
  Completed, // Callbacks retired and the worker may consume its result.
  // The claim remains held: publish run stop, then cancel_active(RunStop).
  RequestRunStop,
};

// TUI cancellation policy distinguishes interactive auditing from teardown release.
enum class PromptCancelReason
{
  RunStop, // Deny/cancel interactively after checking a ready automatic question answer.
  TerminalSignal, // Report interruption while preserving a ready automatic answer.
  Shutdown, // Retire callbacks and use prepared cancellation without automatic resolution.
};

// TUI caller learns which owner consumed a retained search key under admission arbitration.
enum class SearchInputPromptDispatchResult
{
  PromptServiced, // Prompt authority won; never replay this key into its new view.
  InputHandled, // Search ran while new prompt admission was excluded.
  InputFailed, // Search ran but its input/render path reported failure.
};

// Session-owned modal coordinator. UI state and active claims belong to the TUI
// thread; resolver workers use the locked queues and block in shared requests.
// Session collaborators and this stable address must outlive every resolver call.
// A claim protects worker-borrowed callbacks until they retire, even after a result
// exists. Stop admission, retire the claim and join workers before destruction.
class RuntimePromptCoordinator final
{
 public:
  // TUI thread: bind session collaborators and member timers before opening admission.
  // The optional selector and loop must outlive this stable callback target.
  RuntimePromptCoordinator(TuiFrontEnd const& frontend, RuntimePresentationState& presentation, TuiSessionGrantRegistry& session_grants,
                           RuntimeRenderer& renderer, TuiEventLoop& loop, SelectorHost* selectors = nullptr);
  // One coordinator owns the claim and timers; duplicating it would split retirement.
  RuntimePromptCoordinator(RuntimePromptCoordinator const&) = delete;
  // Keep resolver and timer targets bound to the same synchronization identity.
  RuntimePromptCoordinator& operator=(RuntimePromptCoordinator const&) = delete;

  // TUI thread after resolver workers join: shut down residual UI state and timers.
  // The destructor alone cannot make outstanding raw-this resolver calls safe.
  ~RuntimePromptCoordinator();

  // The session and borrowed UI collaborators outlive every resolver invocation;
  // stop admission and join workers before destroying this coordinator.
  // Open only after prior submission workers and claimed UI resolution retire.
  void open_admission();
  // TUI-thread teardown closes admission, retires a claim with prepared cancellation,
  // clears audit/projection state, and cancels timers before destruction.
  void shutdown();
  // TUI setup: return a raw-this callback for workers. Its callers must finish before
  // coordinator destruction; denied admission returns Deny without waiting.
  [[nodiscard]] ava::permissions::PermissionResolver permission_resolver();
  // TUI setup: return a raw-this worker callback with the same join requirement.
  // Admitted callers remain blocked until both answer and claim retirement exist.
  [[nodiscard]] ava::agent::QuestionResolver question_resolver();
  // Either thread: close admission and interrupt queued waiters without allocation.
  // A claimed request stays with the TUI; service/cancel retires its callbacks first.
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
  // TUI thread: normal cancellation checks a ready automatic answer first. Shutdown
  // uses the prepared result and retires callbacks before releasing the worker.
  [[nodiscard]] PromptServiceOutcome cancel_active(PromptCancelReason reason);
  // TUI thread only, after a failed batch paint and before shutdown. Retire any
  // borrowed prompt callbacks before waking its worker with the original Io error.
  // A failure must not run automatic question resolution and turn into success.
  void fail_active_render();
  // TUI thread: test claim ownership, which can outlast the visible prompt projection.
  [[nodiscard]] bool has_active() const noexcept;
  // Either thread: synchronized queue readiness, separate from TUI-only active claim state.
  [[nodiscard]] bool has_pending_requests() const;
  // TUI thread: serialize claim-or-search under the admission mutex. The synchronous
  // search callback must not enqueue prompts or run commands; prompt startup runs unlocked.
  [[nodiscard]] SearchInputPromptDispatchResult dispatch_search_input_with_prompt_precedence(std::function<bool()> const& dispatch_search_input,
                                                                                             std::function<bool()> stop_requested = {},
                                                                                             std::function<void()> before_prompt = {});
  // Either thread: replace the audit endpoint under its own mutex. In-flight copied
  // sinks may still run, so their captured owners need independent lifetime protection.
  void set_audit_sink(ava::event::RuntimeEventSink sink);

  // Queued prompts, decisions and audit callbacks contain arbitrary user text.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Shared ownership keeps a request alive independently for its waiting worker,
  // queue and claimed UI resolution. Only the request owns result synchronization.
  using ClaimedPromptRequest = std::variant<std::monostate, std::shared_ptr<PendingPermissionRequest>, std::shared_ptr<PendingQuestionRequest>>;
  // Owned permission outcome moved to a request only after its callbacks retire.
  using PermissionResult = ava::core::Result<ava::permissions::PermissionResolutionDecision>;
  // Owned answer/error moved to the sole waiting question resolver after retirement.
  using QuestionResult = ava::core::Result<ava::agent::QuestionAnswer>;

  // Worker thread: construct and enqueue under admission lock, notify after unlock,
  // then wait without retaining the queue mutex. The coordinator must outlive the call.
  [[nodiscard]] ava::core::Result<ava::permissions::PermissionResolutionDecision> request_permission(ava::permissions::PermissionPrompt const& prompt);
  // Worker thread: retain a request share while waiting, so removal from the queue
  // cannot destroy the rendezvous or release borrowed callbacks prematurely.
  [[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> request_question(ava::agent::QuestionPrompt const& prompt);
  // TUI thread with prompt_request_mutex_: prefer permission over question FIFO.
  // Claim the request before unlocking so cancellation cannot release its worker early.
  [[nodiscard]] ClaimedPromptRequest claim_pending_request_locked();
  // TUI thread after unlocking admission: install the claim before invoking hooks
  // or presenting the prompt. Exceptions abandon only after retiring callbacks.
  [[nodiscard]] PromptServiceOutcome start_claimed_request(ClaimedPromptRequest request, std::function<bool()> stop_requested,
                                                           std::function<void()> before_prompt);
  // TUI thread: copy the sink under its mutex and invoke it unlocked; the sink may
  // publish through another owner and must never reenter while this lock is held.
  void emit_prompt_audit(std::string status, std::string text, std::string permission_request_id = {}, std::string tool_name = {}, std::string reason = {},
                         std::string resolution_reason = {});
  // TUI thread with a permission claim: audit and reuse an eligible session grant,
  // otherwise publish a default-deny view. A returned result completes immediately.
  [[nodiscard]] std::optional<PermissionResult> start_permission_prompt(ava::permissions::PermissionPrompt const& prompt);
  // TUI thread with a question claim: publish the view before arming automatic polling.
  // A render error is returned for completion; absence leaves the claim active.
  [[nodiscard]] std::optional<QuestionResult> start_question_prompt(ava::agent::QuestionPrompt const& prompt);
  // TUI thread with a permission claim: validate/store remembered or session choices
  // before returning execution authority. Storage/eligibility failure denies.
  [[nodiscard]] PermissionResult resolve_permission_choice(PermissionPromptChoice selected, std::string user_guidance = {});
  // TUI thread with a question claim: audit dismissal and clear its view; caller
  // still owns callback retirement and final delivery of the cancellation error.
  [[nodiscard]] QuestionResult cancel_question();
  // TUI thread while claim protects the worker: invoke the optional borrowed callback.
  // No answer keeps the claim; successful discovery returns an answer or render error.
  [[nodiscard]] std::optional<QuestionResult> auto_resolve_question();
  // TUI thread: keep Escape claimed until ActiveRun publishes stop. Other choices
  // resolve through permission policy before releasing the request.
  [[nodiscard]] PromptServiceOutcome handle_permission_input(runtime_input::RuntimeInput const& input);
  // TUI thread: update selection/text before deriving an answer, audit/copy as needed,
  // then retire callbacks before delivering the owned result.
  [[nodiscard]] PromptServiceOutcome handle_question_input(runtime_input::RuntimeInput const& input);
  // TUI thread with permission claim: clear view and retire timers/callbacks before
  // request completion wakes the worker whose stack those callbacks may borrow.
  [[nodiscard]] PromptServiceOutcome complete_permission(PermissionResult result);
  // TUI thread with question claim: apply the same retire-before-wake order for answers.
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
  // then attempt projection cleanup so UI failure cannot strand the waiter.
  void abandon_active_request();
  // TUI timer callback: request a later service check; never invoke worker callbacks
  // from timer dispatch itself.
  void mark_auto_resolution_due(TuiTimer::Clock::time_point now);
  // TUI timer callback: restore wheel eligibility and reset burst state after the quiet period.
  void expire_wheel_suppression(TuiTimer::Clock::time_point now);
  // TUI thread after accepted prompt wheel navigation: arm a quiet period to keep a
  // burst from changing multiple choices across redraws.
  void suppress_wheel_input();
  // TUI thread on prompt acquisition: close overview/stash presentation so it cannot
  // hide under the prompt and later reclaim input unexpectedly.
  void close_competing_selector();

  // Session borrow used on the TUI thread for persistent permission-rule decisions.
  TuiFrontEnd const& frontend_;
  // Session-owned TUI projection; workers never access prompt views.
  RuntimePresentationState& presentation_;
  // Session-owned TUI grant authority; checked before displaying eligible permission prompts.
  TuiSessionGrantRegistry& session_grants_;
  // Session-owned TUI renderer; rendering never occurs under the audit mutex.
  RuntimeRenderer& renderer_;
  // Optional session borrow used only on the TUI thread. Prompt acquisition closes
  // overview/stash lists that would otherwise hide beneath its new surface.
  SelectorHost* selectors_ = nullptr;
  // Serializes worker admission, queue removal and pending cancellation. Claim/start
  // releases it before callbacks; the bounded search arbitration is the stated exception.
  mutable std::mutex prompt_request_mutex_;
  // Under admission mutex: permission FIFO shares pair with blocked workers
  // and take priority over queued questions when the TUI chooses a new claim.
  std::deque<std::shared_ptr<PendingPermissionRequest>> pending_permission_requests_;
  // Under admission mutex: queued shares keep question rendezvous alive alongside workers.
  std::deque<std::shared_ptr<PendingQuestionRequest>> pending_question_requests_;
  // Under admission mutex: TUI opens between submissions; either thread closes on stop.
  bool accept_prompt_requests_ = false;
  // Under admission mutex: cancellation signals TUI service to retire its active claim;
  // it does not interrupt that claim or overwrite an automatic answer.
  bool cancellation_pending_ = false;
  // Protects sink replacement/copy only; callbacks always run after unlock.
  std::mutex prompt_audit_mutex_;
  // Synchronized endpoint copied for audit emission; its captures must survive in-flight calls.
  ava::event::RuntimeEventSink prompt_audit_sink_;

  // Only the TUI thread owns a claim. Other threads publish cancellation under
  // prompt_request_mutex_; service() resolves it after checking automatic answers.
  // Retire every borrowed callback before completing and releasing its worker.
  ClaimedPromptRequest active_request_;
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
  std::optional<TuiTimer::Clock::time_point> wheel_suppression_deadline_;
  // Weak loop capability; workers notify only after queue publication or cancellation.
  TuiWakeup wakeup_;
  // TUI registration marks automatic discovery due every 100 ms while a question
  // offers it. The borrowed loop and stable target outlive both registrations.
  TuiTimer auto_resolution_timer_;
  // TUI registration ends the wheel quiet period; cancel it before releasing a claim.
  TuiTimer wheel_expiry_timer_;
};

}  // namespace ava::tui
