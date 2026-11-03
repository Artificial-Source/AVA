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
class ActiveRunInbox;
class ActiveRunEvents;
class Composer;
class PendingAttachments;
class RuntimeActionController;
class RuntimeNavigationController;
class RuntimePromptCoordinator;
class RuntimePromptStashController;
class RuntimePluginUiCoordinator;
class RuntimeRenderer;
class RuntimeMermaidPresentationController;
class RuntimeSubagentWorkspaceController;
class SessionTransitionCoordinator;
class TranscriptSearchController;
class TuiEventLoop;
enum class TuiAction;
enum class PromptServiceOutcome;
struct InputEvent;

enum class ActiveRunCancel
{
  Stop,
  Close,
  TerminalSignal
};

namespace detail {
enum class ActiveRunCancelDisposition
{
  ClearDraftSelection,
  ClearTranscriptSelection,
  RequestStop,
};

[[nodiscard]] ActiveRunCancelDisposition active_run_cancel_disposition(bool has_draft_selection, bool has_transcript_selection) noexcept;

} // namespace detail

// The session owns this stable TUI-thread operation and all borrowed collaborators.
// Producers retain only synchronized inbox/request values. Shutdown retires prompt
// callbacks and plugin waits before joining, while every collaborator is alive.
class ActiveRun final
{
 public:
  ActiveRun(TuiEventLoop& loop, TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state, RuntimeRenderer& renderer,
            RuntimePromptCoordinator& prompt_coordinator, RuntimePromptStashController& prompt_stash, RuntimePluginUiCoordinator& plugin_ui,
            RuntimeNavigationController& navigation, RuntimeActionController& action_controller, TranscriptSearchController& transcript_search,
            RuntimeSubagentWorkspaceController& subagent_workspace, SessionTransitionCoordinator& transitions, RuntimeMermaidPresentationController& mermaid,
            PendingAttachments& attachments);
  ~ActiveRun();
  ActiveRun(ActiveRun const&) = delete;
  ActiveRun& operator=(ActiveRun const&) = delete;
  ActiveRun(ActiveRun&&) = delete;
  ActiveRun& operator=(ActiveRun&&) = delete;

  [[nodiscard]] bool start(std::string submitted);
  // Called once in the session control stage, before timers/application updates.
  // Inactive operations leave signals for the idle/session policy.
  [[nodiscard]] bool service_control();
  [[nodiscard]] RuntimeEventDrainResult consume(std::size_t max_events = 256);
  [[nodiscard]] bool handle_input(runtime_input::RuntimeInput const& input);
  [[nodiscard]] std::optional<ActiveRunOutcome> complete();
  [[nodiscard]] bool cancel(ActiveRunCancel reason);
  [[nodiscard]] bool active() const { return phase_ != Phase::Idle; }
  // Preserve terminal buffering once completion publishes, including while its
  // frozen final events are drained over subsequent application turns.
  [[nodiscard]] bool accepts_input() const;
  [[nodiscard]] bool has_pending_updates();
  ActiveRunOutcome shutdown() noexcept;

  // Submission bodies, result/event authority and borrowed projection contain
  // arbitrary user data; generated traversal must not print those aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  enum class Phase
  {
    Idle,
    Running,
    Finalizing
  };
  [[nodiscard]] bool handle_search_input_event(runtime_input::RuntimeInput const& input);
  [[nodiscard]] bool dispatch_active_input(runtime_input::RuntimeInput const& input);
  [[nodiscard]] RuntimeEventDrainResult drain_events(std::size_t max_events);
  [[nodiscard]] bool request_stop();
  void request_close_after_submit();
  [[nodiscard]] bool stop_requested() const;
  void before_prompt();
  [[nodiscard]] bool service_prompt();
  [[nodiscard]] bool apply_prompt_outcome(PromptServiceOutcome outcome);
  [[nodiscard]] bool refresh_plugin_surface_fit();
  [[nodiscard]] bool render();
  [[nodiscard]] bool poll_plugin_ui();
  [[nodiscard]] bool plugin_host_modal_conflict() const;
  void publish_spinner_frame(std::size_t frame);
  void apply_non_transcript_event_authority();
  void upsert_stopping_activity();
  void settle_turn_activity();
  [[nodiscard]] ActiveRunOutcome settle_completion();
  void begin_completion(TuiSubmitResult result);
  void retire_operation();
  void finish_queues();
  static void submit_worker(TuiFrontEnd* frontend, std::shared_ptr<ActiveRunInbox> inbox, std::string submitted, TuiSubmitContext context);
  // Ordered input-handling decomposition. handle_input() dispatches to these
  // focused precedence handlers in the exact order listed below; each returns
  // Unhandled when it does not claim the input so dispatch falls through.
  enum class InputHandling
  {
    Unhandled,
    Handled,
    RenderFailed
  };

  [[nodiscard]] bool is_action(InputEvent const& event, TuiAction action) const;
  [[nodiscard]] static InputHandling to_input_handling(bool render_result);

  [[nodiscard]] ComposerSnapshot const& completion_snapshot();
  [[nodiscard]] bool restore_latest_queued_message();
  [[nodiscard]] std::optional<bool> run_active_command();
  [[nodiscard]] std::optional<bool> reject_disabled_visible_completion();
  [[nodiscard]] bool queue_active_draft(bool follow_up_only);

  InputHandling handle_preemptive_input(runtime_input::RuntimeInput const& active_input);
  InputHandling handle_completion_acceptance(runtime_input::RuntimeInput const& active_input);
  InputHandling handle_active_command_input(runtime_input::RuntimeInput const& active_input);
  InputHandling handle_composer_edit(runtime_input::RuntimeInput const& active_input);
  InputHandling handle_navigation_input(runtime_input::RuntimeInput const& active_input);
  InputHandling handle_navigation_next_input(runtime_input::RuntimeInput const& active_input);
  InputHandling handle_mouse_input(runtime_input::RuntimeInput const& active_input);
  InputHandling handle_restricted_toggle_input(runtime_input::RuntimeInput const& active_input);

  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  Composer& draft_state_;
  RuntimeRenderer& renderer_;
  RuntimePromptCoordinator& prompt_coordinator_;
  RuntimePromptStashController& prompt_stash_;
  RuntimePluginUiCoordinator& plugin_ui_;
  RuntimeNavigationController& navigation_;
  RuntimeActionController& action_controller_;
  TranscriptSearchController& transcript_search_;
  RuntimeSubagentWorkspaceController& subagent_workspace_;
  SessionTransitionCoordinator& transitions_;
  RuntimeMermaidPresentationController& mermaid_;
  PendingAttachments& attachments_;

  TuiWakeup wakeup_;
  // Shared ownership lets copied producer/audit endpoints outlive registration;
  // retirement rejects late publication without exposing any presentation state.
  std::shared_ptr<ActiveRunInbox> inbox_;
  std::unique_ptr<ActiveRunEvents> events_;
  std::future<void> worker_;
  ava::core::NoLogString submitted_;
  std::optional<TuiActiveRunQueues> active_queues_;
  std::optional<TuiSubmitResult> result_;
  std::chrono::steady_clock::time_point turn_started_at_;
  Phase phase_ = Phase::Idle;
  bool is_command_submission_ = false;
  bool close_after_submit_ = false;
  bool terminal_write_failed_ = false;
  bool terminal_signal_received_ = false;
  bool session_changed_ = false;
  bool queues_finished_ = false;
  bool completion_failed_ = false;
  bool final_projection_serviced_ = false;
  Spinner spinner_;
};

} // namespace ava::tui
