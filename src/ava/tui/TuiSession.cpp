// Serializes feature work in the loop described by docs/engineering/tui-architecture.md.
#include "sys.h"
#include "ActiveRun.h"
#include "Composer.h"
#include "SelectorHost.h"
#include "TuiFrontEnd.h"
#include "TuiSession.h"
#include "command_output.h"
#include "composer_internal.h"
#include "prompt_stash_internal.h"
#include "runtime_actions_internal.h"
#include "runtime_input_internal.h"
#include "runtime_internal.h"
#include "runtime_mermaid_internal.h"
#include "runtime_navigation_internal.h"
#include "runtime_plugin_ui_internal.h"
#include "runtime_prompts_internal.h"
#include "runtime_render_internal.h"
#include "runtime_state_internal.h"
#include "runtime_subagent_workspace_internal.h"
#include "runtime_submit_internal.h"
#include "runtime_transcript_search_internal.h"
#include "runtime_user_turn_selection_internal.h"
#include "runtime_views_internal.h"

#include <cstdio>
#include <functional>
#include <utility>

namespace ava::tui {
bool TuiSession::is_action(TuiAction action, InputEvent const& event) const
{
  return key_matches_action(frontend_.key_bindings(), action, event.key);
}

namespace {
constexpr std::size_t kKeyboardScrollRows = 3;
// Fairness budget includes discarded replies, so buffered keys cannot starve updates.
constexpr std::size_t kInputBatchLimit = 32;
// Signals only set atomic bits, and the OS wait retries EINTR. This retained
// timer supplies a nominal 100 ms observation cadence (ten idle wakes/second).
// Decode, callbacks and synchronous work can delay the next control turn.
// A plain wake gives control another opportunity; synchronous work can delay it.
constexpr auto kControlObservationInterval = std::chrono::milliseconds(100);
} // namespace
using Key = terminal::Key;

// TUI thread: construct owners in header declaration order, then attach their
// registrations. No callback may run until every borrowed target exists.
TuiSession::TuiSession(TuiFrontEnd& frontend, terminal::Context& terminal_context, std::string const& initial_status_line)
    : frontend_(frontend),
      terminal_context_(terminal_context),
      loop_(::fileno(terminal_context.input_stream())),
      presentation_state_(&frontend, initial_status_line),
      command_output_(presentation_state_),
      attachments_(presentation_state_),
      plugin_ui_(presentation_state_),
      renderer_(&frontend, presentation_state_, draft_state_, &terminal_context),
      navigation_(frontend, presentation_state_, draft_state_, renderer_),
      selectors_(presentation_state_),
      selector_input_(frontend, presentation_state_, selectors_, renderer_, terminal_context),
      settings_(frontend, presentation_state_, selectors_, terminal_context),
      session_selector_(frontend, presentation_state_, selectors_, renderer_, terminal_context),
      branch_summary_(frontend, presentation_state_, selectors_, renderer_, terminal_context),
      prompt_stash_(presentation_state_, draft_state_, renderer_, selectors_, frontend.key_bindings(), attachments_),
      transcript_search_(presentation_state_, renderer_, navigation_, selectors_),
      prompt_coordinator_(frontend, presentation_state_, command_session_grants_, renderer_, loop_, &selectors_),
      mermaid_presentation_(presentation_state_, renderer_, transcript_search_, frontend_.mermaid_bridge()),
      action_controller_(frontend, presentation_state_, draft_state_, renderer_, selectors_, session_selector_, attachments_),
      subagent_workspace_(frontend, presentation_state_, selectors_),
      session_transitions_(presentation_state_, draft_state_, renderer_, transcript_search_, subagent_workspace_, selectors_, attachments_,
                           command_session_grants_),
      active_run_controller_(loop_, frontend, presentation_state_, renderer_, prompt_coordinator_, plugin_ui_, transcript_search_, subagent_workspace_,
                             session_transitions_, mermaid_presentation_, attachments_, terminal_context),
      active_run_input_(frontend, presentation_state_, draft_state_, renderer_, prompt_stash_, navigation_, action_controller_, transcript_search_,
                        subagent_workspace_, active_run_controller_, command_output_, terminal_context),
      submit_controller_(frontend, presentation_state_, draft_state_, renderer_, navigation_, action_controller_, active_run_controller_, prompt_stash_,
                         transcript_search_, subagent_workspace_, selectors_, settings_, session_selector_),
      // Host modals precede jump/text input. Editor clearing precedes session
      // Interrupt; transcript bindings precede cursor/history; Tail keeps Submit
      // ahead of Space. Keep these positions when adding a configurable binding.
      input_dispatcher_(
          {std::bind_front(&TuiSession::handle_resize_input, this), std::bind_front(&TuiSession::handle_wheel_input, this),
           std::bind_front(&TuiSession::handle_command_output_input, this), std::bind_front(&TuiSession::handle_workspace_input, this),
           std::bind_front(&TuiSession::handle_branch_summary_input, this), std::bind_front(&TuiSession::handle_search_input, this),
           TuiInputDispatcher::event_handler(std::bind_front(&RuntimePromptStashController::handle_selector_input, &prompt_stash_)),
           std::bind_front(&TuiSession::handle_selector_input, this),
           TuiInputDispatcher::event_handler(std::bind_front(&RuntimeNavigationController::handle_sidebar_drawer_input, &navigation_)),
           std::bind_front(&TuiSession::handle_composer_input, this, ComposerInputPhase::Jump), std::bind_front(&TuiSession::handle_primary_input, this),
           std::bind_front(&TuiSession::handle_composer_input, this, ComposerInputPhase::Editor), std::bind_front(&TuiSession::handle_session_input, this),
           std::bind_front(&TuiSession::handle_transcript_input, this),
           std::bind_front(&TuiSession::handle_composer_input, this, ComposerInputPhase::Navigation), std::bind_front(&TuiSession::handle_tail_input, this)}),
      control_timer_(loop_.member_timer(*this, &TuiSession::wake_for_control))
{
  // Some members registered timers during initialization; the remaining owners
  // attach here. Registration never dispatches callbacks. Only run() services
  // due timers, after every member and attachment is ready at a stable address.
  renderer_.attach_loop(loop_);
  plugin_ui_.attach_loop(loop_);
  // Share an origin so the 500 ms reload deadline adds no wake between control
  // ticks. Each timer still skips missed ticks independently after a slow turn.
  auto const control_epoch = TuiTimer::Clock::now();
  action_controller_.attach_loop(loop_, control_epoch);
  branch_summary_.attach_loop(loop_);
  subagent_workspace_.attach_loop(loop_, renderer_);
  mermaid_presentation_.attach_loop(loop_);
  control_timer_.repeat(kControlObservationInterval, control_epoch);
}

// TUI thread: keep all members alive during explicit shutdown; reverse member
// destruction then removes registrations before their loop and target owners.
TuiSession::~TuiSession()
{
  // Each collaborator is still alive here; shutdown also covers early run returns.
  control_timer_.cancel();
  active_run_controller_.shutdown();
  mermaid_presentation_.shutdown();
  plugin_ui_.shutdown();
  prompt_coordinator_.shutdown();
}

bool TuiSession::stop_for_branch_summary(BranchSummaryOutcome outcome)
{
  if (outcome == BranchSummaryOutcome::RenderFailed)
    terminal_write_failed_ = true;
  return outcome == BranchSummaryOutcome::RenderFailed || outcome == BranchSummaryOutcome::ExitReady;
}

bool TuiSession::maybe_reload_display_settings()
{
  auto const outcome = action_controller_.maybe_reload_display_settings();
  if (outcome == DisplaySettingsReloadPollOutcome::TerminalFailure)
    return false;
  if (outcome != DisplaySettingsReloadPollOutcome::Applied)
    return true;

  settings_.rebase_after_display_reload();
  // Paint exactly once after overlay staging so the first frame is the final staged state.
  return renderer_.render();
}

bool TuiSession::toggle_startup_overview()
{
  settings_.close();
  return action_controller_.toggle_startup_overview();
}

RuntimeSubmitDisposition TuiSession::handle_submit(std::optional<std::string> forced_submission)
{
  auto const outcome = submit_controller_.submit(std::move(forced_submission));
  terminal_write_failed_ = outcome.terminal_write_failed;
  terminal_signal_received_ = outcome.terminal_signal_received;
  // A terminal signal must end submission; continuing would lose the session exit outcome.
  ASSERT(!terminal_signal_received_ || outcome.disposition == RuntimeSubmitDisposition::BreakLoop);
  return outcome.disposition;
}

void TuiSession::settle_opened_session(std::string status)
{
  auto committed_session_id = frontend_.read_committed_session_id();
  static_cast<void>(session_transitions_.apply_and_refresh(std::move(committed_session_id)));
  if (!status.empty())
    presentation_state_.publish_local_status(std::move(status));
}

bool TuiSession::synchronize_committed_session()
{
  auto const committed_session_id = frontend_.read_committed_session_id();
  return session_transitions_.apply(std::move(committed_session_id));
}

// Both control observation sites use this policy, so a signal arriving during
// decoding has the same draft/exit semantics as one observed before timers.
TuiInputOutcome TuiSession::handle_signals()
{
  using Signals = core::Signals;
  if (active_run_controller_.active())
  {
    if (!Signals::received(terminal_signals))
      return TuiInputOutcome::Pass;
    // An already answerable question resolves automatically before signal denial.
    // Consume the pending signal only after that prompt policy, matching active-run behavior.
    static_cast<void>(prompt_coordinator_.cancel_active(PromptCancelReason::TerminalSignal));
    bool const exit_requested = Signals::try_obtain(SIGTERM);
    bool const claimed_sigint = !exit_requested && Signals::try_obtain(SIGINT);
    if (exit_requested || (claimed_sigint && draft_state_.draft().text.empty()))
      return active_run_controller_.cancel(ActiveRunCancel::TerminalSignal) ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
    if (claimed_sigint)
    {
      static_cast<void>(action_controller_.clear_draft_for_interrupt());
      // Keep plugin geometry retirement before this urgent redraw, including a
      // signal observed between input decode and the ordinary presentation stage.
      refresh_geometry_from_kernel();
      auto const [width, height] = terminal_size();
      presentation_state_.publish_dimensions(width, height);
      static_cast<void>(plugin_ui_.cancel_unfittable_surfaces());
      return renderer_.render() ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Pass;
  }

  if (Signals::received(terminal_signals))
  {
    bool const exit_requested = Signals::try_obtain(SIGTERM);
    if (exit_requested)
      terminal_signal_received_ = true;

    // ┌─────────────────────┬─────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────┐
    // │Successfully claimed │UI state                                 │Intended action                                                      │
    // ├─────────────────────┼─────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┤
    // │SIGTERM or SIGINT    │Active branch summary                    │Request cancellation; additionally request eventual exit for SIGTERM │
    // ├─────────────────────┼─────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┤
    // │SIGTERM              │No active branch summary                 │Leave the loop (break), regardless of draft contents                 │
    // ├─────────────────────┼─────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┤
    // │SIGINT               │No active branch summary, empty draft    │Leave the loop too (break)                                           │
    // ├─────────────────────┼─────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┤
    // │SIGINT               │No active branch summary, nonempty draft │Clear draft and continue                                             │
    // └─────────────────────┴─────────────────────────────────────────┴─────────────────────────────────────────────────────────────────────┘
    if (branch_summary_.active())
    {
      if (AI_LIKELY(exit_requested || Signals::try_obtain(SIGINT)))
      {
        // We have an active branch summary and successfully claimed either terminal signal.
        // Request cancellation; additionally request eventual exit for SIGTERM.
        auto const mode = exit_requested ? BranchSummaryCancelMode::ExitAfterSettlement : BranchSummaryCancelMode::ContinueSession;
        if (stop_for_branch_summary(branch_summary_.cancel(mode)))
          return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
        return TuiInputOutcome::Handled;
      }
      // Neither terminal signal was available to consume; fall through.
    }
    else if (exit_requested)
    {
      // Claimed SIGTERM and no active branch summary. Leave the loop, regardless of draft contents.
      return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
    }
    else if (Signals::try_obtain(SIGINT))
    {
      // Claimed SIGINT, no active branch summary
      if (draft_state_.draft().text.empty())
      {
        // and an empty draft. Leave the loop too.
        terminal_signal_received_ = true;
        return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
      }
      // and nonempty draft. Clear draft and continue if successful.
      static_cast<void>(action_controller_.clear_draft_for_interrupt());
      if (!renderer_.render())
      {
        return TuiInputOutcome::Failure;
      }
      return TuiInputOutcome::Handled;
    }
    // Neither terminal signal was available to consume; fall through.
  }
  return TuiInputOutcome::Pass;
}

TuiInputOutcome TuiSession::handle_resize_input(runtime_input::RuntimeInput const& input)
{
  if (input.resize)
  {
    renderer_.reset_wheel_input();
    transcript_search_.refresh_after_resize();
    if (!renderer_.render())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  return TuiInputOutcome::Pass;
}

TuiInputOutcome TuiSession::handle_wheel_input(runtime_input::RuntimeInput const& input)
{
  if (!renderer_.accept_wheel_input(input.event.key))
    return TuiInputOutcome::Handled;
  return TuiInputOutcome::Pass;
}

TuiInputOutcome TuiSession::handle_command_output_input(runtime_input::RuntimeInput const& input)
{
  switch (command_output_.handle_input(input.event))
  {
    case CommandOutputDisposition::Pass:
      return TuiInputOutcome::Pass;
    case CommandOutputDisposition::Consumed:
      return TuiInputOutcome::Handled;
    case CommandOutputDisposition::Redraw:
      return renderer_.request_render() ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
  }
  return TuiInputOutcome::Pass;
}

TuiInputOutcome TuiSession::handle_workspace_input(runtime_input::RuntimeInput const& input)
{
  if (subagent_workspace_.active())
  {
    auto const handled = subagent_workspace_.handle_input(input.event);
    if (handled.beep)
      terminal_context_.beep();
    if (handled.changed && !renderer_.request_render())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  return TuiInputOutcome::Pass;
}

TuiInputOutcome TuiSession::handle_branch_summary_input(runtime_input::RuntimeInput const& input)
{
  if (branch_summary_.active())
  {
    if (stop_for_branch_summary(branch_summary_.handle_input(input.event)))
      return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
    return TuiInputOutcome::Handled;
  }
  return TuiInputOutcome::Pass;
}

TuiInputOutcome TuiSession::handle_search_input(runtime_input::RuntimeInput const& input)
{
  presentation_state_.clear_input_feedback();
  if (auto handled = transcript_search_.handle_input(input.event))
  {
    if (!*handled)
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  return TuiInputOutcome::Pass;
}

// Selector owners decide row behavior. Composition performs only returned
// transitions whose collaborators belong to different owners.
TuiInputOutcome TuiSession::handle_selector_input(runtime_input::RuntimeInput const& input)
{
  if (!selectors_.view())
    return TuiInputOutcome::Pass;
  auto result = selectors_.dispatch(input, selector_input_, settings_, session_selector_);
  // Dispatch has already resolved selector-local state. Apply its owned result
  // before requesting a frame, so no intermediate selector state is painted.
  if (auto* draft = std::get_if<SelectorDraftCompletion>(&result))
  {
    draft_state_.accept_completion(std::move(draft->command), std::string::npos);
    presentation_state_.publish_status(std::move(draft->status));
  }
  else if (auto* draft = std::get_if<SelectorDraftCommand>(&result))
    draft_state_.draft_command(std::move(draft->command));
  else if (auto* opened = std::get_if<SelectorOpenedSession>(&result))
    settle_opened_session(std::move(opened->feedback));
  else if (auto* error = std::get_if<SelectorSessionError>(&result))
  {
    // A failed backend action may already have changed sessions. Reconcile first.
    static_cast<void>(synchronize_committed_session());
    presentation_state_.publish_status(std::move(error->feedback));
    terminal_context_.beep();
  }
  else if (auto* summary = std::get_if<SelectorParentSummary>(&result))
  {
    if (stop_for_branch_summary(branch_summary_.start(std::move(summary->previous_view), summary->selected_index, summary->selected_source)))
      return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
  }
  else
  {
    using Request = SelectorDispatchRequest;
    switch (std::get<Request>(result))
    {
      case Request::None:
        break;
      case Request::Models:
        static_cast<void>(action_controller_.open_model_selector());
        break;
      case Request::ScopedModels:
        static_cast<void>(action_controller_.open_scoped_model_selector());
        break;
      case Request::Reasoning:
        static_cast<void>(action_controller_.open_reasoning_selector(false));
        break;
      case Request::CurrentReasoning:
        if (!action_controller_.open_reasoning_selector(true))
          return TuiInputOutcome::Failure;
        break;
      case Request::Hotkeys:
        selectors_.open(ActiveSelectList::Hotkeys, hotkeys_select_list_view(frontend_.key_bindings()));
        presentation_state_.publish_status("keybindings opened");
        renderer_.show_live_tail(LiveTailReason::Requested);
        break;
      case Request::RenderFailed:
        return TuiInputOutcome::Failure;
      case Request::Beep:
        terminal_context_.beep();
        break;
    }
  }
  return renderer_.request_render() ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
}

// Preserve binding collisions: copy and stash precede ordinary editor input;
// terminal handoffs run only after those earlier owners decline the key.
TuiInputOutcome TuiSession::handle_primary_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (is_action(TuiAction::CopyLatestAssistant, event))
  {
    if (!action_controller_.copy_latest_assistant_message())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  else if (is_action(TuiAction::PromptStash, event))
  {
    if (!prompt_stash_.trigger())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  // Editing wins over a colliding terminal-handoff binding. A consumed key
  // cannot accidentally open an external editor or suspend the terminal.
  else if (auto const outcome = handle_composer_input(ComposerInputPhase::Primary, input); outcome != TuiInputOutcome::Pass)
  {
    return outcome;
  }
  else if (is_action(TuiAction::ExternalEditor, event))
  {
    if (!action_controller_.open_external_editor())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  else if (is_action(TuiAction::Suspend, event))
  {
    if (!action_controller_.suspend_to_background())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  else if (is_action(TuiAction::ClipboardPasteImage, event))
  {
    if (!action_controller_.paste_clipboard_image())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  else
    return TuiInputOutcome::Pass;
  return render_input();
}

// This stage follows editor clearing. For example Ctrl+C clears a nonempty
// draft before it can request exit; later transcript keys never override it.
TuiInputOutcome TuiSession::handle_session_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (is_action(TuiAction::ModeToggle, event))
  {
    draft_state_.cancel_escape();
    draft_state_.cancel_path_completion();
    if (auto result = frontend_.on_toggle_mode(); !result)
    {
      presentation_state_.publish_status(result.error().format());
    }
    else
    {
      presentation_state_.refresh_prompt_metadata();
      presentation_state_.publish_status("mode switched to " + presentation_state_.screen().mode);
    }
  }
  else if (is_action(TuiAction::Interrupt, event))
  {
    draft_state_.cancel_path_completion();
    if (!draft_state_.draft().text.empty())
    {
      static_cast<void>(action_controller_.clear_draft_for_interrupt());
      if (!renderer_.render())
      {
        return TuiInputOutcome::Failure;
      }
      return TuiInputOutcome::Handled;
    }
    return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
  }
  else if (is_action(TuiAction::Exit, event))
  {
    return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
  }
  else if (is_action(TuiAction::VariantCycle, event))
  {
    draft_state_.cancel_escape();
    action_controller_.cycle_reasoning();
  }
  else if (is_action(TuiAction::ReasoningSelect, event))
  {
    draft_state_.cancel_escape();
    if (!action_controller_.open_reasoning_selector())
    {
      return TuiInputOutcome::Failure;
    }
  }
  else if (is_action(TuiAction::ThinkingToggle, event))
  {
    draft_state_.cancel_escape();
    action_controller_.toggle_thinking_visibility();
  }
  else if (is_action(TuiAction::OverviewToggle, event))
  {
    draft_state_.cancel_escape();
    if (!toggle_startup_overview())
    {
      return TuiInputOutcome::Failure;
    }
  }
  else if (is_action(TuiAction::ModelSelect, event))
  {
    if (!action_controller_.open_model_selector())
    {
      return TuiInputOutcome::Failure;
    }
  }
  else if (is_action(TuiAction::ModelCycleForward, event))
  {
    draft_state_.cancel_escape();
    action_controller_.cycle_model(true);
  }
  else if (is_action(TuiAction::ModelCycleBackward, event))
  {
    draft_state_.cancel_escape();
    action_controller_.cycle_model(false);
  }
  else if (is_action(TuiAction::SessionResume, event) || is_action(TuiAction::SessionTree, event))
  {
    draft_state_.cancel_escape();
    if (!session_selector_.open())
    {
      return TuiInputOutcome::Failure;
    }
  }
  // Session creation uses the same command/submission boundary as typed input,
  // preserving backend validation and the committed-session reset order.
  else if (is_action(TuiAction::SessionNew, event) || is_action(TuiAction::SessionFork, event))
  {
    auto const action = handle_submit(is_action(TuiAction::SessionNew, event) ? "/new" : "/fork");
    if (action == RuntimeSubmitDisposition::BreakLoop)
      return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
    if (action == RuntimeSubmitDisposition::ContinueLoop)
      return TuiInputOutcome::Handled;
  }
  else if (is_action(TuiAction::MessageDequeue, event))
  {
    draft_state_.cancel_escape();
    presentation_state_.publish_status("queued-message restore is available during active runs");
  }
  else
    return TuiInputOutcome::Pass;
  return render_input();
}

// Transcript navigation has priority over composer cursor/history bindings.
// Each branch changes its feature, then publishes editor fields for the next paint.
TuiInputOutcome TuiSession::handle_transcript_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (is_action(TuiAction::TranscriptHalfPageUp, event))
  {
    navigation_.scroll_up(navigation_.transcript_page_size());
  }
  else if (is_action(TuiAction::TranscriptHalfPageDown, event))
  {
    navigation_.scroll_down(navigation_.transcript_page_size());
  }
  else if (is_action(TuiAction::PageUp, event))
  {
    navigation_.scroll_up(navigation_.transcript_page_size());
  }
  else if (is_action(TuiAction::PageDown, event))
  {
    navigation_.scroll_down(navigation_.transcript_page_size());
  }
  else if (is_action(TuiAction::MessagePrev, event))
  {
    navigation_.scroll_to_message_boundary(true);
  }
  else if (is_action(TuiAction::MessageNext, event))
  {
    navigation_.scroll_to_message_boundary(false);
  }
  else if (is_action(TuiAction::JumpToBottom, event))
  {
    navigation_.jump_to_bottom("live tail");
  }
  else if (event.key == Key::MouseWheelUp)
  {
    navigation_.scroll_up(kTranscriptWheelScrollRows);
  }
  else if (event.key == Key::MouseWheelDown)
  {
    navigation_.scroll_down(kTranscriptWheelScrollRows);
  }
  // Pointer hit testing is last: a configured transcript binding has already
  // had its chance to consume the event against the current viewport.
  else if (!navigation_.handle_pointer_input(event, terminal_context_))
    return TuiInputOutcome::Pass;
  return render_input();
}

// Keep the prepared completion model consistent with this phase's editor state.
// Feedback is published before any cross-owner request and its invalidation.
TuiInputOutcome TuiSession::handle_composer_input(ComposerInputPhase phase, runtime_input::RuntimeInput const& input)
{
  presentation_state_.publish_completion(draft_state_);
  static_cast<void>(renderer_.refresh_completion_cache());
  auto result = draft_state_.handle_key(input, frontend_.key_bindings(), presentation_state_.screen(), renderer_.completion_cache(),
                                        renderer_.transcript().has_selection(), phase);
  if (!result.handled)
    return TuiInputOutcome::Pass;
  if (result.status)
    presentation_state_.publish_status(*result.status);
  if (result.beep)
    terminal_context_.beep();
  // Composer owns editing but not these collaborators. Execute its value
  // request here after publishing feedback, and stop immediately on exit.
  switch (result.request)
  {
    case ComposerInputRequest::None:
      break;
    case ComposerInputRequest::CopyTranscript:
      static_cast<void>(renderer_.copy_transcript_selection());
      break;
    case ComposerInputRequest::ClearTranscriptSelection:
      renderer_.clear_transcript_selection();
      break;
    case ComposerInputRequest::ScrollUp:
      navigation_.scroll_up(kKeyboardScrollRows);
      break;
    case ComposerInputRequest::ScrollDown:
      navigation_.scroll_down(kKeyboardScrollRows);
      break;
    case ComposerInputRequest::Submit: {
      auto const action = handle_submit();
      if (action == RuntimeSubmitDisposition::BreakLoop)
        return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
      if (action == RuntimeSubmitDisposition::ContinueLoop)
        return TuiInputOutcome::Handled;
      break;
    }
  }
  return render_input();
}

TuiInputOutcome TuiSession::handle_tail_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (is_action(TuiAction::DetailsToggle, event))
  {
    draft_state_.cancel_escape();
    // Freeze geometry before changing card detail, so a detached anchor keeps
    // referring to the view the user was reading.
    renderer_.synchronize_detached_transcript_layout();
    presentation_state_.publish_tool_presentation(presentation_state_.screen().tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich
                                                                                                                               : ToolPresentation::Expanded);
    presentation_state_.publish_status("tool details " + std::string(to_string(presentation_state_.screen().tool_presentation)));
  }
  else if (is_action(TuiAction::PromptAllow, event) || is_action(TuiAction::PromptDeny, event))
  {
    draft_state_.cancel_escape();
    presentation_state_.publish_status("prompt action is only available while a prompt is active");
  }
  else
  {
    auto const outcome = handle_composer_input(ComposerInputPhase::Tail, input);
    if (outcome != TuiInputOutcome::Pass)
      return outcome;
  }
  return render_input();
}

TuiInputOutcome TuiSession::render_input()
{
  presentation_state_.publish_completion(draft_state_);
  return renderer_.request_render() ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
}

void TuiSession::apply_input_outcome(TuiInputOutcome outcome)
{
  if (outcome == TuiInputOutcome::Failure)
    terminal_write_failed_ = true;
  if (outcome == TuiInputOutcome::Failure || outcome == TuiInputOutcome::Exit)
    loop_.request_stop();
}

void TuiSession::wake_for_control(TuiEventLoop::Clock::time_point)
{
  // The timer deadline already wakes wait(). Control actions belong to the
  // next control stage, never to a due-timer callback.
}

// Control runs outside timer dispatch because it can release prompt waiters
// and request cancellation. Terminal closure starts a drain, not another wait.
void TuiSession::service_control()
{
  apply_input_outcome(handle_signals());

  if (loop_.notification_failed() || renderer_.render_failed())
    apply_input_outcome(TuiInputOutcome::Failure);
  if (loop_.stopped() || terminal_closing_ || (!loop_.terminal_hangup() && !loop_.terminal_error()))
    return;

  // Stop ordinary dispatch before draining final buffered bytes. HUP is a normal close;
  // descriptor errors are failures. Neither can remain a level-triggered wait.
  terminal_closing_ = true;
  terminal_write_failed_ = terminal_write_failed_ || loop_.terminal_error();
  if (active_run_controller_.active())
    static_cast<void>(active_run_controller_.cancel(ActiveRunCancel::Close));
  if (branch_summary_.active())
    static_cast<void>(branch_summary_.cancel(BranchSummaryCancelMode::ExitAfterSettlement));
}

// Bounded live consumption preserves input fairness. A published completion
// freezes its producers and settles the final finite backlog within complete().
bool TuiSession::service_application_updates()
{
  if (!active_run_controller_.active())
    return false;
  if (active_run_controller_.consume(256) == RuntimeEventDrainResult::RenderFailed)
  {
    apply_input_outcome(TuiInputOutcome::Failure);
    return false;
  }
  if (auto completed = active_run_controller_.complete())
  {
    terminal_write_failed_ = terminal_write_failed_ || completed->terminal_write_failed();
    terminal_signal_received_ = terminal_signal_received_ || completed->terminal_signal_received();
    cleanup_failed_ = cleanup_failed_ || completed->cleanup_failed();
    // A signal completion must exit; continuing would lose its claimed outcome.
    ASSERT(!completed->terminal_signal_received() || completed->break_loop());
    if (completed->break_loop())
      loop_.request_stop();
  }
  return active_run_controller_.has_pending_updates();
}

// Apply feature results before input sees the changed modal or display state.
// Stop immediately on failure so later features cannot hide the exit cause.
void TuiSession::service_presentations()
{
  if (loop_.stopped() || terminal_closing_)
    return;
  if (auto outcome = branch_summary_.service_if_due(); outcome && stop_for_branch_summary(*outcome))
  {
    loop_.request_stop();
    return;
  }
  if (subagent_workspace_.take_render_failure())
  {
    apply_input_outcome(TuiInputOutcome::Failure);
    return;
  }
  // A host prompt freezes these display changes until it retires. Skip the
  // service calls themselves so completion and reload flags remain pending.
  // Workspace failures still end the session even while the prompt owns input.
  if (prompt_coordinator_.has_active())
    return;
  if (mermaid_presentation_.service_if_changed() == MermaidServiceOutcome::RenderFailed)
  {
    apply_input_outcome(TuiInputOutcome::Failure);
    return;
  }
  auto const display_eligible = !active_run_controller_.active() || (!subagent_workspace_.active() && renderer_.transcript().scroll_offset() == 0);
  if (display_eligible && !maybe_reload_display_settings())
    apply_input_outcome(TuiInputOutcome::Failure);
}

// Count decoded and discarded results alike. A full batch requests another
// turn, which services providers and timers before polling buffered input again.
bool TuiSession::service_input_batch()
{
  for (std::size_t count = 0; count < kInputBatchLimit && !loop_.stopped(); ++count)
  {
    // Process a published final result before decoding more keys. Closing an
    // unfinished run still decodes and discards input in the active handler;
    // leaving those bytes on a level-triggered fd would make wait spin.
    if (!terminal_closing_ && active_run_controller_.active() && !active_run_controller_.accepts_input())
      return active_run_controller_.has_pending_updates();
    auto polled = runtime_input::poll_curses_input();
    if (polled.kind == runtime_input::InputPollKind::Empty)
    {
      // A newly painted host prompt rejects typeahead until this empty boundary
      // and a final native flush, since a decoding error can also report Empty.
      if (!prompt_coordinator_.input_drained())
        apply_input_outcome(TuiInputOutcome::Failure);
      if (terminal_closing_)
        loop_.request_stop();
      return false;
    }
    // Discarded control replies consumed input too. Count them toward fairness,
    // but keep draining before an empty fd can hide ncurses-buffered keys.
    if (polled.kind == runtime_input::InputPollKind::Discarded)
      continue;
    auto const& input = polled.input;
    // Check and consume signals again after decoding: a handled signal consumes the retained
    // key before either active or idle feature dispatch can use it.
    auto const control = handle_signals();
    apply_input_outcome(control);
    if (control != TuiInputOutcome::Pass)
      continue;
    if (terminal_closing_ || loop_.stopped())
      continue;
    auto const outcome = active_run_controller_.active() ? active_run_input_.dispatch(input) : input_dispatcher_.dispatch(input);
    apply_input_outcome(outcome);
  }
  // A full batch may leave bytes in ncurses even without fd readiness. Give
  // application work and due timers another turn, then poll input without sleep.
  return !loop_.stopped();
}

// The only live composition loop. Keep the six phases together: timer or
// feature helpers must not secretly perform a second turn after painting.
int TuiSession::run(std::function<bool()> const& before_shutdown)
{
  using Signals = core::Signals;
  if (Signals::received(terminal_signals))
    return 130;
  // Establish one initial frame before waiting. Later ordinary changes share
  // the turn-end paint boundary; terminal handoffs remain synchronous.
  if (mermaid_presentation_.service_if_changed() == MermaidServiceOutcome::RenderFailed || !renderer_.paint_for_handoff())
    return 1;

  while (!loop_.stopped())
  {
    service_control();
    if (loop_.stopped())
      break;
    if (terminal_closing_)
    {
      // Final buffered input is finite. Do not launch discovery/presentation
      // work or wait on a permanently ready HUP while draining these bytes.
      static_cast<void>(service_input_batch());
      continue;
    }
    loop_.service_due();
    if (loop_.stopped() || terminal_closing_)
      continue;
    auto const application_pending = service_application_updates();
    service_presentations();
    auto const input_pending = !loop_.stopped() && service_input_batch();
    if (loop_.stopped() || terminal_closing_)
      continue;

    // Painting checks its own deadline, including time spent decoding input.
    // Other timers that became due get the next control-first turn.
    if (!terminal_closing_ && !renderer_.paint_ready_frame())
    {
      apply_input_outcome(TuiInputOutcome::Failure);
      break;
    }
    auto const runnable = terminal_closing_ || application_pending || input_pending || active_run_controller_.has_pending_updates() || loop_.has_due_work();
    loop_.wait(runnable ? TuiWaitMode::ReadyOnly : TuiWaitMode::Block);
  }

  // Release worker waits before joining, then retire producer transports while
  // the frontend and terminal still exist. Preserve exit causes across cleanup.
  // Every failed loop exit, including a handoff or update failure, must report
  // Io to a held prompt before ordinary shutdown can answer it as interrupted.
  if (terminal_write_failed_)
    prompt_coordinator_.fail_active_render();
  control_timer_.cancel();
  auto const stopped = active_run_controller_.shutdown();
  terminal_write_failed_ = terminal_write_failed_ || stopped.terminal_write_failed();
  terminal_signal_received_ = terminal_signal_received_ || stopped.terminal_signal_received();
  cleanup_failed_ = cleanup_failed_ || stopped.cleanup_failed();
  mermaid_presentation_.shutdown();
  plugin_ui_.shutdown();
  prompt_coordinator_.shutdown();
  if (!before_shutdown())
    terminal_write_failed_ = true;
  return terminal_signal_received_ ? 130 : ((terminal_write_failed_ || cleanup_failed_) ? 1 : 0);
}

} // namespace ava::tui
