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
// it. ActiveRunInput handles ordinary keys; this owner keeps prompt claims next
// to worker cancellation so a permission Escape publishes stop before release.
// Workers retain synchronized inbox/request values, never presentation objects.
class ActiveRun final
{
 public:
  ActiveRun(TuiEventLoop& loop, TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, RuntimeRenderer& renderer,
            RuntimePromptCoordinator& prompt_coordinator, RuntimePluginUiCoordinator& plugin_ui, TranscriptSearchController& transcript_search,
            RuntimeSubagentWorkspaceController& subagent_workspace, SessionTransitionCoordinator& transitions, RuntimeMermaidPresentationController& mermaid,
            PendingAttachments& attachments, terminal::Context& terminal_context);
  ~ActiveRun();
  ActiveRun(ActiveRun const&) = delete;
  ActiveRun& operator=(ActiveRun const&) = delete;
  ActiveRun(ActiveRun&&) = delete;
  ActiveRun& operator=(ActiveRun&&) = delete;

  // TUI thread: start only while inactive. Submission history is recorded by the caller.
  [[nodiscard]] bool start(std::string submitted);
  [[nodiscard]] RuntimeEventDrainResult consume(std::size_t max_events = 256);
  // TUI thread: arbitrate worker prompts, plugin dialogs and the atomic prompt/search
  // race before ordinary input. No value leaves the key for ActiveRunInput.
  [[nodiscard]] std::optional<bool> handle_modal_input(runtime_input::RuntimeInput const& input);
  // TUI thread: a ready result joins, freezes admission, drains, settles and retires
  // in this call. No value means the worker has not published completion yet.
  [[nodiscard]] std::optional<ActiveRunOutcome> complete();
  [[nodiscard]] bool cancel(ActiveRunCancel reason);
  [[nodiscard]] bool active() const { return operation_.has_value(); }
  // A published completion is consumed before polling another terminal key.
  [[nodiscard]] bool accepts_input() const;
  [[nodiscard]] bool has_pending_updates();
  ActiveRunOutcome shutdown() noexcept;

  // Active input calls these on the TUI thread. No queue or inbox reference escapes.
  // Stop is published before any queued prompt waiter is released.
  [[nodiscard]] bool request_stop();
  // Close retains the worker until completion/shutdown joins it; no new input is admitted.
  void request_close_after_submit();
  // Return true only after the backend accepted the text; false publishes the reason.
  [[nodiscard]] bool queue_message(std::string message, bool steering);
  // Transfer a restored message to the editor, or publish an unavailable/error status.
  [[nodiscard]] std::optional<TuiRestoredQueuedMessage> restore_message();
  // Execute only the backend's permitted active command after completion gating.
  [[nodiscard]] TuiActiveNonblockingCommandDispatchResult dispatch_command(ComposerSnapshot const& completion, std::string const& command);

  // Submission bodies, result/event authority and borrowed projection contain user data.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI-owned resources for exactly one submission. The worker uses only copied
  // endpoints. Shutdown/complete must join it before destroying this value.
  struct Operation
  {
    // Copied producer endpoints retain this mailbox; retirement rejects late publications.
    std::shared_ptr<ActiveRunInbox> inbox;
    // TUI-only reducer accumulates event authority until final settlement.
    std::unique_ptr<ActiveRunEvents> events;
    // TUI thread joins after retiring callbacks which can borrow worker-local data.
    std::future<void> worker;
    // Immutable submitted text retained for receipt filtering and fallback projection.
    ava::core::NoLogString submitted;
    // Backend queue capabilities live until finish receipts have been collected.
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

  [[nodiscard]] bool handle_search_input_event(runtime_input::RuntimeInput const& input);
  [[nodiscard]] RuntimeEventDrainResult drain_events(std::size_t max_events);
  [[nodiscard]] bool stop_requested() const;
  void before_prompt();
  void service_prompt();
  [[nodiscard]] bool apply_prompt_outcome(PromptServiceOutcome outcome);
  [[nodiscard]] bool refresh_plugin_surface_fit();
  [[nodiscard]] bool render();
  [[nodiscard]] bool poll_plugin_ui();
  [[nodiscard]] bool plugin_host_modal_conflict() const;
  void publish_spinner_frame(std::size_t frame);
  void apply_non_transcript_event_authority();
  void upsert_stopping_activity();
  void settle_turn_activity();
  [[nodiscard]] ActiveRunOutcome settle_completion(TuiSubmitResult const& result, bool session_changed);
  // Capture both signal and terminal failure before operation reset discards flags.
  [[nodiscard]] ActiveRunOutcome exit_outcome() const;
  void retire_operation();
  void finish_queues();
  static void submit_worker(TuiFrontEnd* frontend, std::shared_ptr<ActiveRunInbox> inbox, std::string submitted, TuiSubmitContext context);

  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  RuntimeRenderer& renderer_;
  RuntimePromptCoordinator& prompt_coordinator_;
  RuntimePluginUiCoordinator& plugin_ui_;
  TranscriptSearchController& transcript_search_;
  RuntimeSubagentWorkspaceController& subagent_workspace_;
  SessionTransitionCoordinator& transitions_;
  RuntimeMermaidPresentationController& mermaid_;
  PendingAttachments& attachments_;
  // Session's terminal context outlives this owner; only the TUI thread uses it.
  terminal::Context& terminal_context_;
  TuiWakeup wakeup_;
  // Absent while idle. Reset only after callback retirement and worker join.
  std::optional<Operation> operation_;
  Spinner spinner_;
};
} // namespace ava::tui
