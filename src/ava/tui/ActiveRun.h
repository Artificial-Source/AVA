// Owns submission lifetime and worker-facing modal arbitration in the TUI architecture.
#pragma once

#include "ActiveRunOutcome.h"
#include "Spinner.h"
#include "TuiWakeup.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/core/NoLogString.h"

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace ava::tui {
namespace runtime_input {
struct RuntimeInput;
}
namespace terminal {
class Context;
}
class ActiveRunInbox;
class ActiveRunEvents;
class PendingAttachments;
class RuntimePromptCoordinator;
class RuntimePluginUiCoordinator;
class RuntimeRenderer;
class RuntimeMermaidPresentationController;
class RuntimeSubagentWorkspaceController;
class SessionTransitionCoordinator;
class TranscriptSearchController;
class TuiEventLoop;
enum class PromptServiceOutcome;
struct TuiActiveNonblockingCommandDispatchResult;

// TUI-thread cancellation policy; only an observed signal produces signal exit status.
enum class ActiveRunCancel
{
  Stop, // Stop this run while leaving the session available.
  Close, // Stop and leave after the worker settles.
  TerminalSignal // Close after a signal, retaining its distinct exit status.
};

// Session owns this stable TUI-thread lifecycle owner. Its collaborators outlive
// it and its spinner registration. See docs/engineering/tui-architecture.md.
// ActiveRunInput handles ordinary keys; this owner keeps prompt claims next
// to worker cancellation so a permission Escape publishes stop before release.
// Workers retain synchronized inbox/request values, never presentation objects.
class ActiveRun final
{
 public:
  // TUI thread: bind session owners and a spinner callback to this stable address.
  // All collaborators and the loop must survive shutdown and destruction.
  ActiveRun(TuiEventLoop& loop, TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, RuntimeRenderer& renderer,
            RuntimePromptCoordinator& prompt_coordinator, RuntimePluginUiCoordinator& plugin_ui, TranscriptSearchController& transcript_search,
            RuntimeSubagentWorkspaceController& subagent_workspace, SessionTransitionCoordinator& transitions, RuntimeMermaidPresentationController& mermaid,
            PendingAttachments& attachments, terminal::Context& terminal_context);
  // TUI thread: stop and join any submission before releasing owned resources.
  ~ActiveRun();
  // A second owner cannot share responsibility for joining one worker.
  ActiveRun(ActiveRun const&) = delete;
  // Do not replace collaborators or worker ownership while callbacks are registered.
  ActiveRun& operator=(ActiveRun const&) = delete;
  // Spinner callbacks bind this address, so moving would leave a stale target.
  ActiveRun(ActiveRun&&) = delete;
  // Keep the registered target and submission lifetime at one stable address.
  ActiveRun& operator=(ActiveRun&&) = delete;

  // TUI thread: start only while inactive. Submission history is recorded by the caller.
  [[nodiscard]] bool start(std::string submitted);
  // TUI thread: service prompts/plugins before a bounded event batch. Host prompts
  // keep provider projection queued until borrowed prompt callbacks retire.
  [[nodiscard]] RuntimeEventDrainResult consume(std::size_t max_events = 256);
  // TUI thread: arbitrate worker prompts, plugin dialogs and the atomic prompt/search
  // race before ordinary input. std::nullopt lets ActiveRunInput handle the key;
  // true means it was consumed or discarded, and false reports input/render failure.
  [[nodiscard]] std::optional<bool> handle_modal_input(runtime_input::RuntimeInput const& input);
  // TUI thread: a ready result joins, freezes admission, drains, settles and retires
  // in this call. While active, no value means completion is not yet published.
  // Inactive owners also return no value. It does not wait for terminal publication;
  // once published, joining may still wait for the worker epilogue.
  [[nodiscard]] std::optional<ActiveRunOutcome> complete();
  // TUI thread: resolve a ready automatic answer before control cancellation.
  // Close/signals stop ordinary input; complete() or shutdown() joins the worker.
  // True means cancellation was accepted, not that the worker has stopped. False
  // means Stop could not request a frame because rendering has already failed.
  [[nodiscard]] bool cancel(ActiveRunCancel reason);
  // TUI thread: operation presence covers launch through post-join settlement.
  [[nodiscard]] bool active() const { return operation_.has_value(); }
  // TUI thread: permit decoding until the worker publishes its final result.
  // Closing keys must still be decoded; handle_modal_input discards them so the
  // watched fd cannot stay readable. A final result is processed before more input.
  [[nodiscard]] bool accepts_input() const;
  // TUI thread: report runnable mailboxes; a claimed prompt deliberately pauses
  // provider projection, so its backlog alone must not keep the loop spinning.
  [[nodiscard]] bool has_pending_updates();
  // TUI thread: release worker waits before joining, then retire per-run state.
  // A release failure logs fixed metadata and terminates, because joining could
  // block forever. Later failures are recorded in the returned cleanup outcome.
  ActiveRunOutcome shutdown() noexcept;

  // Active input calls this on the TUI thread with an operation present.
  // No queue or inbox reference escapes.
  // Stop is published before any queued prompt waiter is released.
  // True means the feedback frame was requested; false means rendering already
  // failed. Neither result means the worker has finished; complete/shutdown joins it.
  [[nodiscard]] bool request_stop();
  // TUI thread while active: retain the worker until completion/shutdown joins it;
  // stop precedes queued-waiter release, and later keys are decoded and discarded.
  void request_close_after_submit();
  // TUI thread while active: true means the backend accepted the text and the
  // editor may clear it; false publishes why the draft must be retained.
  [[nodiscard]] bool queue_message(std::string message, bool steering);
  // TUI thread while active: transfer restored queue text to the editor, or publish
  // an unavailable/error status. The editor owns the returned value.
  [[nodiscard]] std::optional<TuiRestoredQueuedMessage> restore_message();
  // TUI thread while active: execute only backend-permitted commands after the
  // visible completion gate. No result grants permission for arbitrary commands.
  [[nodiscard]] TuiActiveNonblockingCommandDispatchResult dispatch_command(ComposerSnapshot const& completion, std::string const& command);

  // Submission bodies, result/event authority and borrowed projection contain user data.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // ActiveRun owns this value on the TUI thread for exactly one submission.
  // Its reducer and flags never cross threads; the worker uses copied endpoints.
  // Shutdown/complete must join it before reset destroys its queues and reducer.
  struct Operation
  {
    // Copied producer endpoints retain this mailbox; retirement rejects late publications.
    std::shared_ptr<ActiveRunInbox> inbox;
    // TUI-only reducer accumulates event authority until final settlement.
    std::unique_ptr<ActiveRunEvents> events;
    // TUI launches and joins this future after retiring callbacks that may borrow
    // worker-local data; Operation reset must never precede that join.
    std::future<void> worker;
    // Immutable submitted text retained for receipt filtering and fallback projection.
    ava::core::NoLogString submitted;
    // TUI installs backend queue capabilities before launch; worker gets copies.
    // Retain this set until finish receipts have been collected after join.
    std::optional<TuiActiveRunQueues> active_queues;
    // TUI thread records launch time for final assistant metadata.
    std::chrono::steady_clock::time_point turn_started_at;
    // Chosen before launch; command receipts decide whether output enters conversation.
    bool is_command_submission = false;
    // TUI input/control closes admission while allowing worker settlement.
    bool close_after_submit = false;
    // TUI rendering failure survives cleanup until its exit outcome is captured.
    bool terminal_write_failed = false;
    // Only real signal cancellation sets this; descriptor closure does not.
    bool terminal_signal_received = false;
    // Set before calling finish so exceptional shutdown never invokes it twice.
    bool queues_finished = false;
    // Queue receipt failure requires session exit even if terminal writes still work.
    bool completion_failed = false;
    // Owned event authority and submitted text must never enter generated diagnostics.
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  // TUI thread: synchronous search-only callback after the coordinator has decided
  // no queued prompt takes this key. The queue mutex is unlocked before this call;
  // new arrivals stay queued for a later prompt check. False reports input failure.
  [[nodiscard]] bool handle_search_input_event(runtime_input::RuntimeInput const& input);
  // TUI thread with an operation: reduce FIFO events, then publish authorized
  // transcript changes with viewport/search bookkeeping before requesting paint.
  [[nodiscard]] RuntimeEventDrainResult drain_events(std::size_t max_events);
  // TUI-thread prompt predicate combines explicit stop and plugin deadline.
  // No operation means there is no worker whose prompt needs cancellation.
  [[nodiscard]] bool stop_requested() const;
  // TUI thread before claim presentation: freeze spinner and dismiss search so
  // worker prompt authority owns the visible surface and its input.
  void before_prompt();
  // TUI thread: service the current claim, then try pending requests if it retired.
  // This path does not decode input or produce permission Escape stop requests.
  void service_prompt();
  // TUI thread after the prompt handler returns: publish permission Escape stop
  // before releasing its claim, avoiding reentry into the active input handler.
  [[nodiscard]] bool apply_prompt_outcome(PromptServiceOutcome outcome);
  // TUI thread: refresh kernel geometry before allowing any plugin surface input.
  // Returns whether an unfittable surface was removed and needs repaint.
  [[nodiscard]] bool refresh_plugin_surface_fit();
  // TUI thread: validate plugin fit before asking the renderer for a frame.
  [[nodiscard]] bool render();
  // TUI thread: defer plugin projection under a host prompt; otherwise apply
  // transport updates and turn deadline expiry into run cancellation.
  [[nodiscard]] bool poll_plugin_ui();
  // TUI thread: treat existing host surfaces and completion palettes as input
  // owners so plugin modal publication cannot steal their keys.
  [[nodiscard]] bool plugin_host_modal_conflict() const;
  // TUI timer callback: publish only while processing and remember paint failure
  // for the next lifecycle service; workspace views suppress footer requests.
  void publish_spinner_frame(std::size_t frame);
  // TUI thread with an operation: update sidebar/queue projections independently
  // of command receipt permission to append conversation text.
  void apply_non_transcript_event_authority();
  // TUI thread: retain visible stop feedback while backend cleanup is still running.
  void upsert_stopping_activity();
  // TUI thread after final drainage: explicit stop outranks aggregate error/success
  // when replacing the responding activity with a terminal status.
  void settle_turn_activity();
  // TUI thread after join, receipt authorization and final drain: select command
  // versus conversation output, fill missing streamed results, then render once.
  [[nodiscard]] ActiveRunOutcome settle_completion(TuiSubmitResult const& result, bool session_changed);
  // TUI thread: capture signal and terminal failure before operation reset discards
  // flags. With no operation, produce an ordinary close decision.
  [[nodiscard]] ActiveRunOutcome exit_outcome() const;
  // TUI thread after worker join: close remaining callback endpoints and clear
  // processing before destroying the per-submission value. Requires an operation.
  void retire_operation();
  // TUI thread after join: finish at most once, allowing final receipts to enqueue
  // before inbox retirement; receipt failure requires session exit.
  void finish_queues();
  // Worker entry: frontend outlives the joined call; shared inbox pins only transport.
  // Destroy backend callback scopes before publishing the sole terminal payload.
  static void submit_worker(TuiFrontEnd* frontend, std::shared_ptr<ActiveRunInbox> inbox, std::string submitted, TuiSubmitContext context);

  // Session borrow: TUI reads metadata/queues; the joined worker invokes on_submit.
  // Its lifetime must cover both threads through shutdown.
  TuiFrontEnd& frontend_;
  // Session-owned projection; all reads and publications here stay on the TUI thread.
  RuntimePresentationState& presentation_state_;
  // Session-owned layout/paint authority used only on the TUI thread.
  RuntimeRenderer& renderer_;
  // Session owner of worker rendezvous; retire its claims before worker join.
  RuntimePromptCoordinator& prompt_coordinator_;
  // Session owner of plugin transport; workers use its synchronized deadline query
  // while this TUI owner controls modal presentation and teardown.
  RuntimePluginUiCoordinator& plugin_ui_;
  // Session-owned TUI search; prompt acquisition closes it before borrowing a view.
  TranscriptSearchController& transcript_search_;
  // Session-owned TUI view used to suppress background transcript/footer paints.
  RuntimeSubagentWorkspaceController& subagent_workspace_;
  // Session-owned TUI authority applies committed session changes before final drain.
  SessionTransitionCoordinator& transitions_;
  // Session-owned TUI projection service refreshed before the final frame.
  RuntimeMermaidPresentationController& mermaid_;
  // Session-owned TUI attachment staging; start transfers ordinary prompt images.
  PendingAttachments& attachments_;
  // Session's terminal context outlives this owner; only the TUI thread uses it.
  terminal::Context& terminal_context_;
  // Weak loop capability copied into inboxes; publication precedes producer notify.
  TuiWakeup wakeup_;
  // Absent while idle. Reset only after callback retirement and worker join.
  std::optional<Operation> operation_;
  // TUI registration binds this owner; stop it before operation reset or destruction.
  Spinner spinner_;
};
} // namespace ava::tui
