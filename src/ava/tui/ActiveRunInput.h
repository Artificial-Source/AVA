// Routes keys during a submission; see docs/engineering/tui-architecture.md.
#pragma once

#include "TuiInputDispatcher.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/Composer.h"

namespace ava::tui {
class ActiveRun;
class CommandOutput;
class RuntimeRenderer;
class RuntimePromptStashController;
class RuntimeNavigationController;
class RuntimeActionController;
class RuntimeSubagentWorkspaceController;
class TranscriptSearchController;
class TuiFrontEnd;

namespace detail {
// TUI-thread Cancel precedence; selections are dismissed before stopping a worker.
enum class ActiveRunCancelDisposition
{
  ClearDraftSelection, // Draft selection owns Cancel before transcript selection.
  ClearTranscriptSelection, // Clear the transcript range before requesting stop.
  RequestStop // No selection captures Cancel; publish run cancellation.
};
// Call-scoped selection facts determine Cancel precedence without touching a worker.
[[nodiscard]] ActiveRunCancelDisposition active_run_cancel_disposition(bool has_draft_selection, bool has_transcript_selection) noexcept;
} // namespace detail

// Session owns this TUI-thread dispatcher after all borrowed feature owners.
// It edits drafts and routes views; ActiveRun alone owns worker/queue lifetime.
// Ordered stages stop at the first handled key, preserving binding collisions.
// All methods and members are TUI-thread only. Borrowed owners outlive destruction;
// dispatcher callbacks are destroyed with this stable owner and never reach workers.
class ActiveRunInput final
{
 public:
  // Borrow stable session owners; each must outlive this dispatcher and its callbacks.
  ActiveRunInput(TuiFrontEnd& frontend, RuntimePresentationState& presentation, Composer& composer, RuntimeRenderer& renderer,
                 RuntimePromptStashController& stash, RuntimeNavigationController& navigation, RuntimeActionController& actions,
                 TranscriptSearchController& search, RuntimeSubagentWorkspaceController& workspace, ActiveRun& run, CommandOutput& output,
                 terminal::Context& terminal_context);
  // Not copyable: dispatch callbacks retain this address and its session bindings.
  ActiveRunInput(ActiveRunInput const&) = delete;
  ActiveRunInput& operator=(ActiveRunInput const&) = delete;
  // TUI thread after control and updates; the active run arbitrates worker prompts first.
  [[nodiscard]] TuiInputOutcome dispatch(runtime_input::RuntimeInput const& input);
  // Every borrowed owner is printed as an address, never traversed for user content.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Consume editor feedback before the stage requests its next frame.
  [[nodiscard]] bool apply_composer_result(ComposerInputResult result);
  // Resolve this session's configurable bindings without duplicating lookup policy.
  [[nodiscard]] bool is_action(InputEvent const& event, TuiAction action) const;
  // Map a synchronous invalidation/paint result to the dispatch stop condition.
  [[nodiscard]] static TuiInputOutcome render_outcome(bool render_result);
  // Publish the current editor and prepare a read-only model before completion decisions.
  [[nodiscard]] ComposerSnapshot const& completion_snapshot();
  // Restore owned backend queue text only after the lifecycle owner authorizes dequeue.
  [[nodiscard]] bool restore_latest_queued_message();
  // Run allowed local/view commands; no value leaves submission/queue policy in charge.
  [[nodiscard]] std::optional<bool> run_active_command();
  // A disabled visible candidate consumes submission without discarding its draft.
  [[nodiscard]] std::optional<bool> reject_disabled_visible_completion();
  // Validate the current draft, submit through ActiveRun, and clear only on acceptance.
  [[nodiscard]] bool queue_active_draft(bool follow_up_only);
  // Active worker prompts and queued prompt checks precede ordinary feature input.
  TuiInputOutcome handle_modal_input(runtime_input::RuntimeInput const& input);
  // Resize/wheel filtering precedes output, workspace and prompt-stash capture.
  TuiInputOutcome handle_surface_input(runtime_input::RuntimeInput const& input);
  // Selection/cancel/copy/interrupt and direct typing precede completion and commands.
  TuiInputOutcome handle_preemptive_input(runtime_input::RuntimeInput const& input);
  // Accept an enabled palette row or explicitly activate path completion.
  TuiInputOutcome handle_completion_acceptance(runtime_input::RuntimeInput const& input);
  // Local active commands and queued submissions precede ordinary editor keys.
  TuiInputOutcome handle_active_command_input(runtime_input::RuntimeInput const& input);
  // Editing preserves the active-run reset rules before navigation is attempted.
  TuiInputOutcome handle_composer_edit(runtime_input::RuntimeInput const& input);
  // Transcript bindings and previous-history/palette actions keep their original priority.
  TuiInputOutcome handle_navigation_input(runtime_input::RuntimeInput const& input);
  // Next-history/palette actions follow previous bindings when user bindings overlap.
  TuiInputOutcome handle_navigation_next_input(runtime_input::RuntimeInput const& input);
  // Pointer completions retain active-run acceptance/reset policy before view hit-testing.
  TuiInputOutcome handle_mouse_input(runtime_input::RuntimeInput const& input);
  // Display-only toggles run after editor and pointer stages; backend changes remain blocked.
  TuiInputOutcome handle_restricted_toggle_input(runtime_input::RuntimeInput const& input);

  TuiFrontEnd& frontend_; // TUI-thread bindings and permitted local frontend actions.
  RuntimePresentationState& presentation_state_; // TUI-thread display updates through named operations.
  Composer& draft_state_; // Session's sole editor; never copied or retained by workers.
  RuntimeRenderer& renderer_; // Layout and frame requests remain with the session renderer.
  RuntimePromptStashController& prompt_stash_; // Captures stash selection before ordinary keys.
  RuntimeNavigationController& navigation_; // Owns transcript/sidebar navigation policy.
  RuntimeActionController& action_controller_; // Executes permitted display/editor handoffs.
  TranscriptSearchController& transcript_search_; // Opens the search view for active local commands.
  RuntimeSubagentWorkspaceController& subagent_workspace_; // Captures workspace keys without changing backend job ownership.
  ActiveRun& run_; // Owns cancellation and queue capabilities; returns values, never mutable borrows.
  CommandOutput& command_output_; // Shares modal input policy with idle dispatch.
  terminal::Context& terminal_context_; // TUI-thread feedback only; workers never borrow it.
  TuiInputDispatcher dispatcher_; // Registers this stable object's stages in precedence order.
};
} // namespace ava::tui
