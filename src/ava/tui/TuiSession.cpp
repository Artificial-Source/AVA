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
namespace {
constexpr std::size_t kKeyboardScrollRows = 3;
constexpr std::size_t kInputBatchLimit = 32;
constexpr auto kControlObservationInterval = std::chrono::milliseconds(100);
} // namespace
using Key = terminal::Key;

TuiSession::TuiSession(TuiFrontEnd& frontend, terminal::Context& terminal_context, std::string const& initial_status_line)
    : frontend_(frontend),
      terminal_context_(terminal_context),
      loop_(::fileno(terminal_context.input_stream())),
      presentation_state_(&frontend, initial_status_line),
      plugin_ui_(presentation_state_.snapshot),
      renderer_(&frontend, presentation_state_.snapshot, presentation_state_.sidebar, draft_state_, &terminal_context),
      navigation_(frontend, presentation_state_.snapshot, draft_state_, renderer_),
      selectors_(presentation_state_.snapshot),
      selector_input_(frontend, presentation_state_, selectors_, renderer_, terminal_context),
      settings_(frontend, presentation_state_, selectors_, terminal_context),
      session_selector_(frontend, presentation_state_, selectors_, renderer_, terminal_context),
      branch_summary_(frontend, presentation_state_, selectors_, renderer_, terminal_context),
      prompt_stash_(presentation_state_, draft_state_, renderer_, selectors_, frontend.key_bindings()),
      transcript_search_(presentation_state_, renderer_, navigation_, selectors_),
      prompt_coordinator_(frontend, presentation_state_.snapshot, presentation_state_.command_session_grants, renderer_, loop_, &selectors_),
      mermaid_presentation_(presentation_state_.snapshot, renderer_, transcript_search_, frontend.mermaid_bridge()),
      action_controller_(frontend, presentation_state_, draft_state_, renderer_, selectors_, session_selector_),
      subagent_workspace_(frontend, presentation_state_.snapshot, selectors_),
      session_transitions_(presentation_state_, draft_state_, renderer_, transcript_search_, subagent_workspace_, selectors_),
      active_run_controller_(loop_, frontend, presentation_state_, draft_state_, renderer_, prompt_coordinator_, prompt_stash_, plugin_ui_, navigation_,
                             action_controller_, transcript_search_, subagent_workspace_, session_transitions_, mermaid_presentation_),
      submit_controller_(frontend, presentation_state_, draft_state_, renderer_, navigation_, action_controller_, active_run_controller_, prompt_stash_,
                         transcript_search_, subagent_workspace_, selectors_, settings_, session_selector_),
      input_dispatcher_({std::bind_front(&TuiSession::handle_resize_input, this), std::bind_front(&TuiSession::handle_wheel_input, this),
                         std::bind_front(&TuiSession::handle_command_output_input, this), std::bind_front(&TuiSession::handle_workspace_input, this),
                         std::bind_front(&TuiSession::handle_branch_summary_input, this), std::bind_front(&TuiSession::handle_search_input, this),
                         TuiInputDispatcher::event_handler(std::bind_front(&RuntimePromptStashController::handle_selector_input, &prompt_stash_)),
                         std::bind_front(&TuiSession::handle_selector_input, this),
                         TuiInputDispatcher::event_handler(std::bind_front(&RuntimeNavigationController::handle_sidebar_drawer_input, &navigation_)),
                         std::bind_front(&TuiSession::handle_jump_input, this), std::bind_front(&TuiSession::handle_primary_input, this),
                         std::bind_front(&TuiSession::handle_editor_input, this), std::bind_front(&TuiSession::handle_session_input, this),
                         std::bind_front(&TuiSession::handle_transcript_input, this), std::bind_front(&TuiSession::handle_composer_navigation_input, this),
                         std::bind_front(&TuiSession::handle_tail_input, this)}),
      control_timer_(loop_.member_timer(*this, &TuiSession::service_control))
{
  renderer_.attach_loop(loop_);
  plugin_ui_.attach_loop(loop_);
  action_controller_.attach_loop(loop_);
  branch_summary_.attach_loop(loop_);
  subagent_workspace_.attach_loop(loop_, renderer_);
  mermaid_presentation_.attach_loop(loop_);
  control_timer_.repeat(kControlObservationInterval);
}

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
  static_cast<void>(session_transitions_.apply_and_refresh(frontend_, std::move(committed_session_id)));
  if (!status.empty())
    settle_local_command_status(presentation_state_.snapshot, std::move(status));
}

bool TuiSession::synchronize_committed_session()
{
  auto const committed_session_id = frontend_.read_committed_session_id();
  return session_transitions_.apply(std::move(committed_session_id));
}

TuiInputOutcome TuiSession::handle_signals()
{
  using Signals = core::Signals;
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
      // We failed to claim either terminal signal; fall-through.
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
      presentation_state_.snapshot.selected_slash_command_index = draft_state_.completion_index();
      if (!renderer_.render())
      {
        return TuiInputOutcome::Failure;
      }
      return TuiInputOutcome::Handled;
    }
    // Failed to claim either terminal signal; fall-through.
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
  if (presentation_state_.snapshot.command_output && !presentation_state_.snapshot.permission_prompt && !presentation_state_.snapshot.question_prompt)
  {
    auto const geometry = command_output_geometry(presentation_state_.snapshot.width, presentation_state_.snapshot.height);
    auto const output_input = ava::tui::handle_command_output_input(*presentation_state_.snapshot.command_output, input.event, geometry.width, geometry.height,
                                                                    presentation_state_.snapshot.tool_presentation);
    if (output_input.action == CommandOutputInputAction::Dismiss)
    {
      presentation_state_.snapshot.command_output.reset();
      presentation_state_.snapshot.status = "command output closed";
      if (!renderer_.request_render())
      {
        return TuiInputOutcome::Failure;
      }
      return TuiInputOutcome::Handled;
    }
    if (output_input.action == CommandOutputInputAction::Redraw)
    {
      presentation_state_.snapshot.command_output->scroll_offset = output_input.scroll_offset;
      if (!renderer_.request_render())
      {
        return TuiInputOutcome::Failure;
      }
      return TuiInputOutcome::Handled;
    }
    // Typing or clearing starts the next draft directly; pointer input remains
    // captured by the modal so transcript/composer hit targets stay suppressed.
    if (input.event.key == Key::Character || input.event.key == Key::Space || input.event.key == Key::CtrlU)
    {
      presentation_state_.snapshot.command_output.reset();
      presentation_state_.snapshot.status.clear();
    }
    else
    {
      return TuiInputOutcome::Handled;
    }
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
  clear_reasoning_feedback_for_user_input(presentation_state_.snapshot);
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

TuiInputOutcome TuiSession::handle_selector_input(runtime_input::RuntimeInput const& input)
{
  if (selectors_.view())
  {
    settings_.synchronize_selector();
    if (selectors_.kind() != ActiveSelectList::Session)
      session_selector_.cancel_confirmation();

    auto input_result = selector_input_.classify(input);
    if (input_result.action == SelectListInputAction::Redraw && selectors_.view())
    {
      session_selector_.cancel_confirmation();
      if (selectors_.kind() == ActiveSelectList::Settings)
        settings_.highlight(input_result.selected_item_index, std::move(input_result.query));
      else
        selectors_.update_selection(input_result.selected_item_index, std::move(input_result.query));
    }
    else if (input_result.action == SelectListInputAction::Cancel && selectors_.kind() == ActiveSelectList::Settings)
    {
      settings_.cancel();
    }
    else if (input_result.action == SelectListInputAction::Resolve && selectors_.kind() == ActiveSelectList::Settings)
    {
      auto result = settings_.resolve(input_result);
      if (auto* command = std::get_if<std::string>(&result))
      {
        bool const edits_keybindings = *command == "/keybindings set ";
        draft_state_.accept_completion(std::move(*command), std::string::npos);
        presentation_state_.snapshot.status = edits_keybindings ? "keybinding edit command drafted" : "command drafted";
      }
      else
      {
        switch (std::get<SettingsDialogRequest>(result))
        {
          case SettingsDialogRequest::Models:
            static_cast<void>(action_controller_.open_model_selector());
            break;
          case SettingsDialogRequest::ScopedModels:
            static_cast<void>(action_controller_.open_scoped_model_selector());
            break;
          case SettingsDialogRequest::Reasoning:
            static_cast<void>(action_controller_.open_reasoning_selector(false));
            break;
          case SettingsDialogRequest::Hotkeys:
            selectors_.open(ActiveSelectList::Hotkeys, hotkeys_select_list_view(frontend_.key_bindings()));
            presentation_state_.snapshot.status = "keybindings opened";
            renderer_.transcript().request_live_tail();
            break;
          case SettingsDialogRequest::None:
            break;
        }
      }
    }
    else if (input_result.action == SelectListInputAction::SummarizeParent && selectors_.kind() == ActiveSelectList::Session && selectors_.view())
    {
      session_selector_.cancel_confirmation();
      if (input_result.selected_item_index >= selectors_.view()->items.size() || !selectors_.view()->items[input_result.selected_item_index].enabled ||
          selectors_.view()->items[input_result.selected_item_index].value.empty())
      {
        presentation_state_.snapshot.status = "parent summary is unavailable from this row";
        terminal_context_.beep();
      }
      else
      {
        auto prior_view = *selectors_.view();
        prior_view.query = input_result.query;
        prior_view.selected_item_index = input_result.selected_item_index;
        auto const selected_value = prior_view.items[input_result.selected_item_index].value;
        if (stop_for_branch_summary(branch_summary_.start(std::move(prior_view), input_result.selected_item_index, selected_value)))
          return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
      }
    }
    else if (selectors_.kind() == ActiveSelectList::Session &&
             (input_result.action == SelectListInputAction::CycleSort || input_result.action == SelectListInputAction::ToggleNamedFilter ||
              input_result.action == SelectListInputAction::TogglePathDisplay || input_result.action == SelectListInputAction::ToggleArchivedFilter ||
              input_result.action == SelectListInputAction::ToggleLabelTimestamp))
    {
      session_selector_.browse(input_result);
    }
    else if (selectors_.kind() == ActiveSelectList::Session &&
             (input_result.action == SelectListInputAction::Rename || input_result.action == SelectListInputAction::Label))
    {
      if (auto command = session_selector_.draft_command(input_result))
      {
        draft_state_.draft_command(std::move(*command));
      }
    }
    else if ((input_result.action == SelectListInputAction::BranchParent || input_result.action == SelectListInputAction::BranchChild) &&
             selectors_.kind() == ActiveSelectList::Session)
    {
      if (auto opened = session_selector_.navigate_branch(input_result))
      {
        if (*opened)
          settle_opened_session(std::move(**opened));
        else
        {
          static_cast<void>(synchronize_committed_session());
          presentation_state_.snapshot.status = opened->error().format();
          terminal_context_.beep();
        }
      }
    }
    else if (input_result.action == SelectListInputAction::Archive || input_result.action == SelectListInputAction::ArchiveNoninvasive)
    {
      session_selector_.archive(input_result);
    }
    else if (input_result.action == SelectListInputAction::Resolve && selectors_.kind() == ActiveSelectList::Session)
    {
      auto opened = session_selector_.resolve(input_result);
      if (opened)
        settle_opened_session(std::move(*opened));
      else
      {
        static_cast<void>(synchronize_committed_session());
        presentation_state_.snapshot.status = opened.error().format();
        terminal_context_.beep();
      }
    }
    else
    {
      if (input_result.action == SelectListInputAction::Resolve || input_result.action == SelectListInputAction::Cancel)
        session_selector_.cancel_confirmation();
      auto outcome = selector_input_.handle(input_result);
      if (auto const* request = std::get_if<SelectorInputRequest>(&outcome))
      {
        if (*request == SelectorInputRequest::RenderFailed)
          return TuiInputOutcome::Failure;
        if (*request == SelectorInputRequest::OpenReasoning && !action_controller_.open_reasoning_selector(true))
          return TuiInputOutcome::Failure;
      }
      else
      {
        auto& [kind, text] = std::get<std::pair<SelectorInputTextKind, std::string>>(outcome);
        switch (kind)
        {
          case SelectorInputTextKind::DraftCommand:
            draft_state_.accept_completion(std::move(text), std::string::npos);
            presentation_state_.snapshot.status = "keybinding edit command drafted";
            break;
          case SelectorInputTextKind::OpenedSession:
            settle_opened_session(std::move(text));
            break;
          case SelectorInputTextKind::ReconcileSessionError:
            static_cast<void>(synchronize_committed_session());
            presentation_state_.snapshot.status = std::move(text);
            terminal_context_.beep();
            break;
        }
      }
    }
    if (!renderer_.request_render())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  return TuiInputOutcome::Pass;
}

TuiInputOutcome TuiSession::handle_jump_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (draft_state_.jump_mode() != ComposerJumpMode::None)
  {
    if (key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ||
        key_matches_action(frontend_.key_bindings(), TuiAction::JumpBackward, event.key))
    {
      draft_state_.cancel_jump();
      presentation_state_.snapshot.status = "jump cancelled";
    }
    else if (auto const target = printable_jump_target(input))
    {
      bool const forward = draft_state_.jump_mode() == ComposerJumpMode::Forward;
      presentation_state_.snapshot.status =
          draft_state_.jump_to_character(*target) ? (forward ? "jumped forward" : "jumped backward") : "jump character not found";
    }
    else
    {
      draft_state_.cancel_jump();
    }
    if (event.key == Key::Character || event.key == Key::Space || key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ||
        key_matches_action(frontend_.key_bindings(), TuiAction::JumpBackward, event.key))
    {
      presentation_state_.snapshot.selected_slash_command_index = draft_state_.completion_index();
      if (!renderer_.request_render())
      {
        return TuiInputOutcome::Failure;
      }
      return TuiInputOutcome::Handled;
    }
  }
  return TuiInputOutcome::Pass;
}

TuiInputOutcome TuiSession::handle_primary_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (key_matches_action(frontend_.key_bindings(), TuiAction::CopyLatestAssistant, event.key))
  {
    if (!action_controller_.copy_latest_assistant_message())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PromptStash, event.key))
  {
    if (!prompt_stash_.trigger())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  else if (event.key == Key::Character)
  {
    draft_state_.insert_input(input, presentation_state_.snapshot);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::MessageFollowUp, event.key))
  {
    auto const action = handle_submit();
    if (action == RuntimeSubmitDisposition::BreakLoop)
      return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
    if (action == RuntimeSubmitDisposition::ContinueLoop)
      return TuiInputOutcome::Handled;
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::NewLine, event.key))
  {
    draft_state_.insert_newline();
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::ExternalEditor, event.key))
  {
    if (!action_controller_.open_external_editor())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::Suspend, event.key))
  {
    if (!action_controller_.suspend_to_background())
    {
      return TuiInputOutcome::Failure;
    }
    return TuiInputOutcome::Handled;
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::ClipboardPasteImage, event.key))
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

TuiInputOutcome TuiSession::handle_editor_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  bool const ctrl_d_delete_forward =
      event.key == Key::CtrlD && key_matches_action(frontend_.key_bindings(), TuiAction::DeleteForward, event.key) && !draft_state_.draft().text.empty();
  bool const delete_forward_action =
      key_matches_action(frontend_.key_bindings(), TuiAction::DeleteForward, event.key) && (event.key != Key::CtrlD || ctrl_d_delete_forward);
  if (key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ||
      key_matches_action(frontend_.key_bindings(), TuiAction::JumpBackward, event.key))
  {
    draft_state_.begin_jump(key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ? ComposerJumpMode::Forward
                                                                                                            : ComposerJumpMode::Backward);
    presentation_state_.snapshot.status =
        key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ? "jump forward: type character" : "jump backward: type character";
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteBackward, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::DeleteBackward));
  }
  else if (delete_forward_action)
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::DeleteForward));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteWordBackward, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::DeleteWordBackward));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteWordForward, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::DeleteWordForward));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteToLineStart, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::DeleteToLineStart));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteToLineEnd, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::DeleteToLineEnd));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CopySelection, event.key) && draft_state_.selection_bounds())
  {
    draft_state_.cancel_escape();
    draft_state_.cancel_path_completion();
    static_cast<void>(draft_state_.copy_selection(presentation_state_.snapshot));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CopySelection, event.key) && renderer_.transcript().has_selection())
  {
    draft_state_.cancel_escape();
    draft_state_.cancel_path_completion();
    static_cast<void>(renderer_.copy_transcript_selection());
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::ClearInput, event.key) &&
           (!draft_state_.draft().text.empty() || !key_matches_action(frontend_.key_bindings(), TuiAction::Interrupt, event.key)))
  {
    presentation_state_.snapshot.status = draft_state_.apply_edit(TuiAction::ClearInput) ? "input cleared" : "input already empty";
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::AutocompleteAccept, event.key) && navigation_.slash_palette_active())
  {
    draft_state_.cancel_escape();
    draft_state_.select_slash_command(presentation_state_.snapshot, terminal_context_);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::AutocompleteAccept, event.key) && navigation_.file_reference_palette_active())
  {
    draft_state_.cancel_escape();
    draft_state_.select_file_reference(navigation_, presentation_state_.snapshot, terminal_context_);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::AutocompleteAccept, event.key) && navigation_.path_completion_palette_active())
  {
    draft_state_.cancel_escape();
    draft_state_.select_path_completion(navigation_, presentation_state_.snapshot, terminal_context_);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::AutocompleteAccept, event.key) &&
           draft_state_.force_path_completion(navigation_, presentation_state_.snapshot, terminal_context_))
  {
    draft_state_.cancel_escape();
  }
  else
    return TuiInputOutcome::Pass;
  return render_input();
}

TuiInputOutcome TuiSession::handle_session_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (key_matches_action(frontend_.key_bindings(), TuiAction::ModeToggle, event.key))
  {
    draft_state_.cancel_escape();
    draft_state_.cancel_path_completion();
    if (auto result = frontend_.on_toggle_mode(); !result)
    {
      presentation_state_.snapshot.status = result.error().format();
    }
    else
    {
      presentation_state_.refresh_prompt_metadata();
      presentation_state_.snapshot.status = "mode switched to " + presentation_state_.snapshot.mode;
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::Interrupt, event.key))
  {
    draft_state_.cancel_path_completion();
    if (!draft_state_.draft().text.empty())
    {
      static_cast<void>(action_controller_.clear_draft_for_interrupt());
      presentation_state_.snapshot.selected_slash_command_index = draft_state_.completion_index();
      if (!renderer_.render())
      {
        return TuiInputOutcome::Failure;
      }
      return TuiInputOutcome::Handled;
    }
    return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::Exit, event.key))
  {
    return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::VariantCycle, event.key))
  {
    draft_state_.cancel_escape();
    action_controller_.cycle_reasoning();
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::ReasoningSelect, event.key))
  {
    draft_state_.cancel_escape();
    if (!action_controller_.open_reasoning_selector())
    {
      return TuiInputOutcome::Failure;
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::ThinkingToggle, event.key))
  {
    draft_state_.cancel_escape();
    action_controller_.toggle_thinking_visibility();
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::OverviewToggle, event.key))
  {
    draft_state_.cancel_escape();
    if (!toggle_startup_overview())
    {
      return TuiInputOutcome::Failure;
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelSelect, event.key))
  {
    if (!action_controller_.open_model_selector())
    {
      return TuiInputOutcome::Failure;
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelCycleForward, event.key))
  {
    draft_state_.cancel_escape();
    action_controller_.cycle_model(true);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelCycleBackward, event.key))
  {
    draft_state_.cancel_escape();
    action_controller_.cycle_model(false);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::SessionResume, event.key) ||
           key_matches_action(frontend_.key_bindings(), TuiAction::SessionTree, event.key))
  {
    draft_state_.cancel_escape();
    if (!session_selector_.open())
    {
      return TuiInputOutcome::Failure;
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::SessionNew, event.key) ||
           key_matches_action(frontend_.key_bindings(), TuiAction::SessionFork, event.key))
  {
    auto const action = handle_submit(key_matches_action(frontend_.key_bindings(), TuiAction::SessionNew, event.key) ? "/new" : "/fork");
    if (action == RuntimeSubmitDisposition::BreakLoop)
      return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
    if (action == RuntimeSubmitDisposition::ContinueLoop)
      return TuiInputOutcome::Handled;
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::MessageDequeue, event.key))
  {
    draft_state_.cancel_escape();
    presentation_state_.snapshot.status = "queued-message restore is available during active runs";
  }
  else
    return TuiInputOutcome::Pass;
  return render_input();
}

TuiInputOutcome TuiSession::handle_transcript_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (key_matches_action(frontend_.key_bindings(), TuiAction::TranscriptHalfPageUp, event.key))
  {
    navigation_.scroll_up(navigation_.transcript_page_size());
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::TranscriptHalfPageDown, event.key))
  {
    navigation_.scroll_down(navigation_.transcript_page_size());
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PageUp, event.key))
  {
    navigation_.scroll_up(navigation_.transcript_page_size());
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PageDown, event.key))
  {
    navigation_.scroll_down(navigation_.transcript_page_size());
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::MessagePrev, event.key))
  {
    navigation_.scroll_to_message_boundary(true);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::MessageNext, event.key))
  {
    navigation_.scroll_to_message_boundary(false);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::JumpToBottom, event.key))
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
  else if (event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick || event.key == Key::MouseLeftDrag || event.key == Key::MouseLeftRelease ||
           event.key == Key::MousePointerCancel)
  {
    draft_state_.cancel_escape();
    auto const begins_click = event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick;
    bool palette_claimed = false;
    if (begins_click)
    {
      if (auto const clicked = slash_palette_selection_for_screen_position(presentation_state_.snapshot, event.mouse_row, event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.clear_selection();
        draft_state_.select_completion(*clicked);
        draft_state_.select_slash_command(presentation_state_.snapshot, terminal_context_);
        palette_claimed = true;
      }
      else if (auto const clicked = renderer_.file_reference_completion_at(event.mouse_row, event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.clear_selection();
        draft_state_.select_completion(*clicked);
        draft_state_.select_file_reference(navigation_, presentation_state_.snapshot, terminal_context_);
        palette_claimed = true;
      }
      else if (auto const clicked = renderer_.path_completion_at(event.mouse_row, event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.clear_selection();
        draft_state_.select_completion(*clicked);
        draft_state_.select_path_completion(navigation_, presentation_state_.snapshot, terminal_context_);
        palette_claimed = true;
      }
    }
    if (!palette_claimed)
    {
      auto const transcript_mouse =
          renderer_.handle_transcript_selection_mouse(event, std::bind_front(&RuntimeNavigationController::toggle_tool_details_at, &navigation_),
                                                      std::bind_front(&RuntimeNavigationController::toggle_thinking_at, &navigation_));
      if (transcript_mouse == TranscriptSelectionMouseResult::Ignored)
      {
        if (begins_click)
        {
          if (auto const cursor = composer_input_cursor_for_screen_position(presentation_state_.snapshot, event.mouse_row, event.mouse_column))
          {
            renderer_.clear_transcript_selection();
            draft_state_.begin_pointer_selection(*cursor, event.key == Key::MouseLeftPress);
            presentation_state_.snapshot.status = "cursor moved";
          }
        }
        else if (draft_state_.mouse_selecting())
        {
          if (auto const cursor = composer_input_cursor_for_screen_position(presentation_state_.snapshot, event.mouse_row, event.mouse_column))
          {
            draft_state_.extend_pointer_selection(*cursor);
            presentation_state_.snapshot.status = draft_state_.selection_bounds() ? "selection active" : "cursor moved";
          }
          if (event.key == Key::MouseLeftRelease)
            draft_state_.end_pointer_selection();
        }
      }
    }
  }
  else
    return TuiInputOutcome::Pass;
  return render_input();
}

TuiInputOutcome TuiSession::handle_composer_navigation_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (draft_state_.extend_selection_for_key(event.key, presentation_state_.snapshot))
  {
    renderer_.clear_transcript_selection();
  }
  else if (event.key == Key::CtrlHome)
  {
    draft_state_.move_document_boundary(false);
  }
  else if (event.key == Key::CtrlEnd)
  {
    draft_state_.move_document_boundary(true);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorLeft, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::CursorLeft));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorRight, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::CursorRight));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorLineStart, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::CursorLineStart));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorLineEnd, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::CursorLineEnd));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorWordLeft, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::CursorWordLeft));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorWordRight, event.key))
  {
    static_cast<void>(draft_state_.apply_edit(TuiAction::CursorWordRight));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PalettePrev, event.key) && navigation_.slash_palette_active())
  {
    draft_state_.cancel_escape();
    auto const match_count = filter_slash_commands(draft_state_.draft().text, draft_state_.draft().cursor, presentation_state_.snapshot.slash_commands).size();
    if (match_count == 0)
    {
      presentation_state_.snapshot.status = "no matching slash commands";
    }
    else
    {
      draft_state_.select_completion(previous_slash_palette_selection(draft_state_.draft().text, draft_state_.draft().cursor,
                                                                      presentation_state_.snapshot.slash_commands, draft_state_.completion_index()));
      presentation_state_.snapshot.status = "command " + std::to_string(draft_state_.completion_index() + 1) + "/" + std::to_string(match_count);
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PalettePrev, event.key) && navigation_.file_reference_palette_active())
  {
    draft_state_.cancel_escape();
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      presentation_state_.snapshot.status = "no matching file references";
    }
    else
    {
      draft_state_.select_completion(navigation_.previous_completion(draft_state_.completion_index()));
      presentation_state_.snapshot.status = "reference " + std::to_string(draft_state_.completion_index() + 1) + "/" + std::to_string(match_count);
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PalettePrev, event.key) && navigation_.path_completion_palette_active())
  {
    draft_state_.cancel_escape();
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      presentation_state_.snapshot.status = "no matching paths";
    }
    else
    {
      draft_state_.select_completion(navigation_.previous_completion(draft_state_.completion_index()));
      presentation_state_.snapshot.status = "path " + std::to_string(draft_state_.completion_index() + 1) + "/" + std::to_string(match_count);
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::HistoryPrev, event.key))
  {
    if (draft_state_.browse_history(true))
    {
      presentation_state_.snapshot.status = "history previous";
    }
    else
    {
      navigation_.scroll_up(kKeyboardScrollRows);
    }
  }
  else if (event.key == Key::ArrowUp)
  {
    navigation_.scroll_up(kKeyboardScrollRows);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorUp, event.key) && draft_state_.move_vertical(TuiAction::CursorUp))
  {
    // Composer applied the movement and its successful-movement reset.
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PaletteNext, event.key) && navigation_.slash_palette_active())
  {
    draft_state_.cancel_escape();
    auto const match_count = filter_slash_commands(draft_state_.draft().text, draft_state_.draft().cursor, presentation_state_.snapshot.slash_commands).size();
    if (match_count == 0)
    {
      presentation_state_.snapshot.status = "no matching slash commands";
    }
    else
    {
      draft_state_.select_completion(next_slash_palette_selection(draft_state_.draft().text, draft_state_.draft().cursor,
                                                                  presentation_state_.snapshot.slash_commands, draft_state_.completion_index()));
      presentation_state_.snapshot.status = "command " + std::to_string(draft_state_.completion_index() + 1) + "/" + std::to_string(match_count);
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PaletteNext, event.key) && navigation_.file_reference_palette_active())
  {
    draft_state_.cancel_escape();
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      presentation_state_.snapshot.status = "no matching file references";
    }
    else
    {
      draft_state_.select_completion(navigation_.next_completion(draft_state_.completion_index()));
      presentation_state_.snapshot.status = "reference " + std::to_string(draft_state_.completion_index() + 1) + "/" + std::to_string(match_count);
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PaletteNext, event.key) && navigation_.path_completion_palette_active())
  {
    draft_state_.cancel_escape();
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      presentation_state_.snapshot.status = "no matching paths";
    }
    else
    {
      draft_state_.select_completion(navigation_.next_completion(draft_state_.completion_index()));
      presentation_state_.snapshot.status = "path " + std::to_string(draft_state_.completion_index() + 1) + "/" + std::to_string(match_count);
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::HistoryNext, event.key))
  {
    if (draft_state_.browse_history(false))
    {
      presentation_state_.snapshot.status = draft_state_.history_index() ? "history next" : "history draft";
    }
    else
    {
      navigation_.scroll_down(kKeyboardScrollRows);
    }
  }
  else if (event.key == Key::ArrowDown)
  {
    navigation_.scroll_down(kKeyboardScrollRows);
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorDown, event.key) && draft_state_.move_vertical(TuiAction::CursorDown))
  {
    // Composer applied the movement and its successful-movement reset.
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::Undo, event.key))
  {
    presentation_state_.snapshot.status = draft_state_.apply_edit(TuiAction::Undo) ? "undo" : "nothing to undo";
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::Redo, event.key))
  {
    presentation_state_.snapshot.status = draft_state_.apply_edit(TuiAction::Redo) ? "redo" : "nothing to redo";
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::Yank, event.key))
  {
    presentation_state_.snapshot.status = draft_state_.apply_edit(TuiAction::Yank) ? "yanked text" : "nothing to yank";
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::YankPop, event.key))
  {
    presentation_state_.snapshot.status = draft_state_.apply_edit(TuiAction::YankPop) ? "yank-pop" : "nothing to yank-pop";
  }
  else
    return TuiInputOutcome::Pass;
  return render_input();
}

TuiInputOutcome TuiSession::handle_tail_input(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  if (key_matches_action(frontend_.key_bindings(), TuiAction::DetailsToggle, event.key))
  {
    draft_state_.cancel_escape();
    renderer_.synchronize_detached_transcript_layout();
    presentation_state_.snapshot.tool_presentation =
        presentation_state_.snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich : ToolPresentation::Expanded;
    presentation_state_.snapshot.status = "tool details " + std::string(to_string(presentation_state_.snapshot.tool_presentation));
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::PromptAllow, event.key) ||
           key_matches_action(frontend_.key_bindings(), TuiAction::PromptDeny, event.key))
  {
    draft_state_.cancel_escape();
    presentation_state_.snapshot.status = "prompt action is only available while a prompt is active";
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::Cancel, event.key))
  {
    if (draft_state_.selection_bounds())
    {
      draft_state_.clear_selection();
      draft_state_.cancel_escape();
      presentation_state_.snapshot.status.clear();
    }
    else if (renderer_.transcript().has_selection())
    {
      renderer_.clear_transcript_selection();
      draft_state_.cancel_escape();
      presentation_state_.snapshot.status.clear();
    }
    else if (navigation_.slash_palette_active() || navigation_.file_reference_palette_active() || navigation_.path_completion_palette_active())
    {
      draft_state_.dismiss_completion();
      presentation_state_.snapshot.status.clear();
    }
    else
    {
      draft_state_.handle_escape(presentation_state_.snapshot);
    }
  }
  else if (key_matches_action(frontend_.key_bindings(), TuiAction::Submit, event.key))
  {
    if (event.key == Key::Enter && draft_state_.convert_backslash_enter_to_newline(presentation_state_.snapshot))
    {
      if (!renderer_.request_render())
      {
        return TuiInputOutcome::Failure;
      }
      return TuiInputOutcome::Handled;
    }
    auto const action = handle_submit();
    if (action == RuntimeSubmitDisposition::BreakLoop)
      return terminal_write_failed_ ? TuiInputOutcome::Failure : TuiInputOutcome::Exit;
    if (action == RuntimeSubmitDisposition::ContinueLoop)
      return TuiInputOutcome::Handled;
  }
  else if (event.key == Key::Space)
  {
    draft_state_.insert_input(input, presentation_state_.snapshot);
  }
  return render_input();
}

TuiInputOutcome TuiSession::render_input()
{
  presentation_state_.snapshot.selected_slash_command_index = draft_state_.completion_index();
  return renderer_.request_render() ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
}

void TuiSession::apply_input_outcome(TuiInputOutcome outcome)
{
  if (outcome == TuiInputOutcome::Failure)
    terminal_write_failed_ = true;
  if (outcome == TuiInputOutcome::Failure || outcome == TuiInputOutcome::Exit)
    loop_.request_stop();
}

void TuiSession::service_control(TuiEventLoop::Clock::time_point)
{
  if (active_run_controller_.active())
  {
    if (!active_run_controller_.service_control())
      apply_input_outcome(TuiInputOutcome::Failure);
  }
  else
    apply_input_outcome(handle_signals());

  if (loop_.notification_failed() || renderer_.render_failed())
    apply_input_outcome(TuiInputOutcome::Failure);
  if (loop_.stopped() || terminal_closing_ || (!loop_.terminal_hangup() && !loop_.terminal_error()))
    return;

  // Stop admission before draining final buffered bytes. HUP is a normal close;
  // descriptor errors are failures. Neither can remain a level-triggered wait.
  terminal_closing_ = true;
  terminal_write_failed_ = terminal_write_failed_ || loop_.terminal_error();
  if (active_run_controller_.active())
    static_cast<void>(active_run_controller_.cancel(ActiveRunCancel::Close));
  if (branch_summary_.active())
    static_cast<void>(branch_summary_.cancel(BranchSummaryCancelMode::ExitAfterSettlement));
}

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
    // A signal completion must exit; continuing would lose its claimed outcome.
    ASSERT(!completed->terminal_signal_received() || completed->break_loop());
    if (completed->break_loop())
      loop_.request_stop();
  }
  return active_run_controller_.has_pending_updates();
}

void TuiSession::service_presentations()
{
  if (loop_.stopped() || terminal_closing_)
    return;
  if (auto outcome = branch_summary_.take_service_outcome(); outcome && stop_for_branch_summary(*outcome))
  {
    loop_.request_stop();
    return;
  }
  if (subagent_workspace_.take_render_failure() || mermaid_presentation_.take_service_outcome() == MermaidServiceOutcome::RenderFailed ||
      mermaid_presentation_.service_if_changed() == MermaidServiceOutcome::RenderFailed)
  {
    apply_input_outcome(TuiInputOutcome::Failure);
    return;
  }
  auto const display_eligible = !active_run_controller_.active() || (!subagent_workspace_.active() && renderer_.transcript().scroll_offset() == 0);
  if (display_eligible && !maybe_reload_display_settings())
    apply_input_outcome(TuiInputOutcome::Failure);
}

bool TuiSession::service_input_batch()
{
  for (std::size_t count = 0; count < kInputBatchLimit && !loop_.stopped(); ++count)
  {
    // A published completion and its finite frozen receipt backlog settle before
    // more input is decoded. Those bytes remain in ncurses/startup buffering.
    if (!terminal_closing_ && active_run_controller_.active() && !active_run_controller_.accepts_input())
      return true;
    auto input = runtime_input::poll_curses_input();
    if (!input)
    {
      if (terminal_closing_)
        loop_.request_stop();
      return false;
    }
    if (active_run_controller_.active())
      service_control(TuiEventLoop::Clock::now());
    else
    {
      // Preserve idle dispatch precedence when a signal arrives during decode:
      // a handled draft clear/branch cancellation consumes this retained input.
      auto const control = handle_signals();
      apply_input_outcome(control);
      if (control != TuiInputOutcome::Pass)
        continue;
    }
    if (terminal_closing_ || loop_.stopped())
      continue;
    auto const outcome = active_run_controller_.active() ? (active_run_controller_.handle_input(*input) ? TuiInputOutcome::Handled : TuiInputOutcome::Failure)
                                                         : input_dispatcher_.dispatch(*input);
    apply_input_outcome(outcome);
  }
  // A full batch may leave bytes in ncurses even without fd readiness. Give
  // application work and due timers another turn, then poll input without sleep.
  return !loop_.stopped();
}

int TuiSession::run(std::function<bool()> const& before_shutdown)
{
  using Signals = core::Signals;
  if (Signals::received(terminal_signals))
    return 130;
  if (mermaid_presentation_.service_if_changed() == MermaidServiceOutcome::RenderFailed || !renderer_.paint_for_handoff())
    return 1;

  while (!loop_.stopped())
  {
    service_control(TuiEventLoop::Clock::now());
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

    // Existing bounded decoder subreads can cross deadlines. Service due work
    // even under continuous input, then revisit application callbacks next turn.
    auto const timer_work = loop_.has_due_work();
    loop_.service_due();
    service_presentations();
    if (loop_.stopped())
      break;
    if (!terminal_closing_ && !renderer_.paint_ready_frame())
    {
      apply_input_outcome(TuiInputOutcome::Failure);
      break;
    }
    auto const runnable =
        terminal_closing_ || application_pending || input_pending || timer_work || active_run_controller_.has_pending_updates() || loop_.has_due_work();
    loop_.wait(runnable ? TuiWaitMode::ReadyOnly : TuiWaitMode::Block);
  }

  control_timer_.cancel();
  auto const stopped = active_run_controller_.shutdown();
  terminal_write_failed_ = terminal_write_failed_ || stopped.terminal_write_failed();
  terminal_signal_received_ = terminal_signal_received_ || stopped.terminal_signal_received();
  mermaid_presentation_.shutdown();
  plugin_ui_.shutdown();
  prompt_coordinator_.shutdown();
  if (!before_shutdown())
    terminal_write_failed_ = true;
  return terminal_signal_received_ ? 130 : (terminal_write_failed_ ? 1 : 0);
}

} // namespace ava::tui
