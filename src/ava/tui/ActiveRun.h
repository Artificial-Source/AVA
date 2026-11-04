// Owns a submission worker and routes its prompt/plugin input on the TUI thread.
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
  Close, // Stop and leave after worker cleanup.
  TerminalSignal // Close after a signal, retaining its distinct exit status.
};

// TuiSession owns one ActiveRun at a stable address; all collaborators outlive it
// and its spinner registration. All methods except submit_worker use the TUI thread.
// ActiveRunInput handles ordinary keys; this owner routes prompts and cancellation
// so permission Escape requests worker stop before releasing the waiting resolver.
// Workers use synchronized inboxes/requests and never read screen views.
// See docs/engineering/tui-architecture.md for the surrounding session lifetime.
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
  // Not copyable or movable: one owner joins the worker, and callbacks retain its address.
  ActiveRun(ActiveRun const&) = delete;
  ActiveRun& operator=(ActiveRun const&) = delete;
  ActiveRun(ActiveRun&&) = delete;
  ActiveRun& operator=(ActiveRun&&) = delete;

  // TUI thread: start only while inactive. Submission history is recorded by the caller.
  [[nodiscard]] bool start(std::string submitted);
  // TUI thread: service prompts/plugins before a bounded event batch. Host prompts
  // keep provider events queued until TUI callbacks borrowing worker data are removed.
  [[nodiscard]] RuntimeEventDrainResult consume(std::size_t max_events = 256);
  // TUI thread: route worker prompts and plugin dialogs, checking queued prompts
  // before search or ordinary input. std::nullopt lets ActiveRunInput handle the key;
  // true means it was consumed or discarded, and false reports input/render failure.
  [[nodiscard]] std::optional<bool> handle_modal_input(runtime_input::RuntimeInput const& input);
  // TUI thread: process a ready result by joining the worker, finishing queues,
  // closing the inbox, draining events and clearing the operation in this call.
  // No value means inactive or no result yet. It does not wait for a result;
  // once one is stored, joining may still wait for the worker epilogue.
  [[nodiscard]] std::optional<ActiveRunOutcome> complete();
  // TUI thread: resolve a ready automatic answer before control cancellation.
  // Close/signals stop ordinary input; complete() or shutdown() joins the worker.
  // True means cancellation was accepted, not that the worker has stopped. False
  // means Stop could not request a frame because rendering has already failed.
  [[nodiscard]] bool cancel(ActiveRunCancel reason);
  // TUI thread: an operation exists from launch through cleanup after worker join.
  [[nodiscard]] bool active() const { return operation_.has_value(); }
  // TUI thread: permit decoding until the worker publishes its final result.
  // Closing keys must still be decoded; handle_modal_input discards them so the
  // watched fd cannot stay readable. A final result is processed before more input.
  [[nodiscard]] bool accepts_input() const;
  // TUI thread: report inbox or prompt-queue work. An active prompt pauses provider
  // events, so that backlog alone must not keep the loop spinning.
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
  // TUI thread while active: retain the worker until complete()/shutdown() joins it;
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

  // Submission bodies, results, events and borrowed screen views contain user data.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // ActiveRun owns this value on the TUI thread for exactly one submission.
  // Its reducer and flags never cross threads; the worker uses copied endpoints.
  // Shutdown/complete must join it before reset destroys its queues and reducer.
  struct Operation
  {
    // Copied producer callbacks share this mailbox; retire() rejects later writes.
    std::shared_ptr<ActiveRunInbox> inbox;
    // TUI-only reducer collects events until the result and final events are applied.
    std::unique_ptr<ActiveRunEvents> events;
    // TUI launches and joins this future after retiring callbacks that may borrow
    // worker-local data; Operation reset must never precede that join.
    std::future<void> worker;
    // Immutable submitted text used to filter command output and fill missing transcript text.
    ava::core::NoLogString submitted;
    // TUI installs backend queue capabilities before launch; worker gets copies.
    // Retain these callbacks until finish has emitted its final queue events after join.
    std::optional<TuiActiveRunQueues> active_queues;
    // TUI thread records launch time for final assistant metadata.
    std::chrono::steady_clock::time_point turn_started_at;
    // Chosen before launch; backend ordinary_turn_request_ids select command conversation events.
    bool is_command_submission = false;
    // TUI input/control discards later keys while waiting for worker cleanup.
    bool close_after_submit = false;
    // TUI rendering failure survives cleanup until its exit outcome is captured.
    bool terminal_write_failed = false;
    // Only real signal cancellation sets this; descriptor closure does not.
    bool terminal_signal_received = false;
    // Set before calling finish so exceptional shutdown never invokes it twice.
    bool queues_finished = false;
    // TUI-thread queue completion failure; both complete and shutdown return a failure exit.
    bool completion_failed = false;
    // Owned events and submitted text must never enter generated diagnostics.
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  // TUI thread: synchronous search-only callback after the coordinator has decided
  // no queued prompt takes this key. The queue mutex is unlocked before this call;
  // new arrivals stay queued for a later prompt check. False reports input failure.
  [[nodiscard]] bool handle_search_input_event(runtime_input::RuntimeInput const& input);
  // TUI thread with an operation: reduce events in queue order, then update eligible
  // transcript text and viewport/search positions before requesting paint.
  [[nodiscard]] RuntimeEventDrainResult drain_events(std::size_t max_events);
  // TUI-thread prompt predicate combines explicit stop and plugin deadline.
  // No operation means there is no worker whose prompt needs cancellation.
  [[nodiscard]] bool stop_requested() const;
  // TUI thread before showing a prompt: stop the spinner and close search so the
  // permission/question prompt receives keys.
  void before_prompt();
  // TUI thread: service the active prompt, then try queued requests if it completed.
  // This path does not decode input or produce permission Escape stop requests.
  void service_prompt();
  // TUI thread after the prompt handler returns: publish permission Escape stop
  // before releasing its waiting worker, avoiding reentry into the active input handler.
  [[nodiscard]] bool apply_prompt_outcome(PromptServiceOutcome outcome);
  // TUI thread: refresh kernel geometry before allowing any plugin surface input.
  // Returns whether an unfittable surface was removed and needs repaint.
  [[nodiscard]] bool refresh_plugin_surface_fit();
  // TUI thread: validate plugin fit before asking the renderer for a frame.
  [[nodiscard]] bool render();
  // TUI thread: defer plugin updates under a host prompt; otherwise apply
  // transport updates and turn deadline expiry into run cancellation.
  [[nodiscard]] bool poll_plugin_ui();
  // TUI thread: treat existing host surfaces and completion palettes as input
  // owners so opening a plugin modal cannot steal their keys.
  [[nodiscard]] bool plugin_host_modal_conflict() const;
  // TUI timer callback: update the spinner only while processing. If a frame request
  // reports an earlier paint failure, retain it; workspace views suppress footer requests.
  void publish_spinner_frame(std::size_t frame);
  // TUI thread with an operation: update sidebar and queued-message display even
  // when command events cannot yet enter the conversation transcript.
  void apply_non_transcript_event_authority();
  // TUI thread: retain visible stop feedback while backend cleanup is still running.
  void upsert_stopping_activity();
  // TUI thread after final drainage: explicit stop outranks aggregate error/success
  // when replacing the responding activity with its final status.
  void settle_turn_activity();
  // TUI thread after join, request-ID filtering and final drain: select command
  // versus conversation output, fill missing streamed results, then request the final frame.
  [[nodiscard]] ActiveRunOutcome settle_completion(TuiSubmitResult const& result, bool session_changed);
  // TUI thread: capture signal, terminal and queue-completion failures before
  // resetting the operation. With no operation, produce an ordinary close decision.
  [[nodiscard]] ActiveRunOutcome exit_outcome() const;
  // TUI thread after worker join: close remaining callback endpoints and clear
  // processing before destroying the per-submission value. Requires an operation.
  void retire_operation();
  // TUI thread after join: finish backend queues at most once so their final events
  // enter the inbox before it closes. Record returned failure in completion_failed.
  void finish_queues();
  // Worker entry: frontend outlives the joined call; shared inbox pins only transport.
  // Destroy backend callback scopes before storing the sole final result.
  static void submit_worker(TuiFrontEnd* frontend, std::shared_ptr<ActiveRunInbox> inbox, std::string submitted, TuiSubmitContext context);

  // Session borrow: TUI reads metadata/queues; the joined worker invokes on_submit.
  // Its lifetime must cover both threads through shutdown.
  TuiFrontEnd& frontend_;
  // Session-owned screen data; all reads and updates here stay on the TUI thread.
  RuntimePresentationState& presentation_state_;
  // Session-owned layout and painter used only on the TUI thread.
  RuntimeRenderer& renderer_;
  // Session prompt owner; remove TUI callbacks and release waiting resolvers before worker join.
  RuntimePromptCoordinator& prompt_coordinator_;
  // Session owner of plugin transport; workers use its synchronized deadline query
  // while this TUI owner controls modal presentation and teardown.
  RuntimePluginUiCoordinator& plugin_ui_;
  // Session-owned TUI search; close it before showing a worker's prompt.
  TranscriptSearchController& transcript_search_;
  // Session-owned TUI view used to suppress background transcript/footer paints.
  RuntimeSubagentWorkspaceController& subagent_workspace_;
  // Session-owned TUI coordinator applies committed session changes before final drain.
  SessionTransitionCoordinator& transitions_;
  // Session-owned TUI diagram display refreshed before the final frame.
  RuntimeMermaidPresentationController& mermaid_;
  // Session-owned TUI attachment staging; start transfers ordinary prompt images.
  PendingAttachments& attachments_;
  // Session's terminal context outlives this owner; only the TUI thread uses it.
  terminal::Context& terminal_context_;
  // Weak wake handle copied into inboxes; producers store data before notifying the loop.
  TuiWakeup wakeup_;
  // Absent while idle. Reset only after callback retirement and worker join.
  std::optional<Operation> operation_;
  // TUI registration binds this owner; stop it before operation reset or destruction.
  Spinner spinner_;
};
} // namespace ava::tui
