#include "sys.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_active_run_internal.h"
#include "ava/tui/runtime_active_run_state_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/terminal.h"
#include "ava/core/Application.h"

#include <string>
#include <utility>

namespace ava::tui {
using runtime_input::printable_jump_target;

detail::ActiveRunCancelDisposition detail::active_run_cancel_disposition(bool has_draft_selection, bool has_transcript_selection) noexcept
{
  if (has_draft_selection)
    return ActiveRunCancelDisposition::ClearDraftSelection;
  if (has_transcript_selection)
    return ActiveRunCancelDisposition::ClearTranscriptSelection;
  return ActiveRunCancelDisposition::RequestStop;
}

// Sidebar drawer, jump mode, cancel/copy/interrupt/exit, character/selection
// and CtrlHome/CtrlEnd. These preempt all draft, completion, and view input.
RuntimeActiveRunController::InputHandling RuntimeActiveRunController::handle_preemptive_input(RuntimeActiveRunState& state,
                                                                                              runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_state = draft_state_;
  auto& jump_mode = draft_state_.jump_mode;
  auto& pending_escape_clear = draft_state_.pending_escape_clear;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;

  if (auto handled = navigation_.handle_sidebar_drawer_input(active_event))
    return to_input_handling(*handled);
  if (jump_mode != ComposerJumpMode::None)
  {
    if (is_action(active_event, TuiAction::JumpForward) || is_action(active_event, TuiAction::JumpBackward))
    {
      jump_mode = ComposerJumpMode::None;
      snapshot.status = "jump cancelled";
      return to_input_handling(renderer_.request_render());
    }
    if (auto const target = printable_jump_target(active_input))
    {
      bool const forward = jump_mode == ComposerJumpMode::Forward;
      jump_mode = ComposerJumpMode::None;
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      path_completion_force_active = false;
      draft_scroll_offset = 0;
      draft_state.clear_selection();
      snapshot.status =
          jump_composer_draft_to_character(draft, *target, forward) ? (forward ? "jumped forward" : "jumped backward") : "jump character not found";
      return to_input_handling(renderer_.request_render());
    }
    jump_mode = ComposerJumpMode::None;
  }
  if (is_action(active_event, TuiAction::CopyLatestAssistant))
    return to_input_handling(action_controller_.copy_latest_assistant_message());
  if (is_action(active_event, TuiAction::PromptStash))
    return to_input_handling(prompt_stash_.trigger());
  if (active_event.key == Key::Escape || is_action(active_event, TuiAction::Cancel))
  {
    switch (detail::active_run_cancel_disposition(draft_state.selection_bounds().has_value(), renderer_.has_transcript_selection()))
    {
      case detail::ActiveRunCancelDisposition::ClearDraftSelection:
        draft_state.clear_selection();
        snapshot.status.clear();
        return to_input_handling(renderer_.request_render());
      case detail::ActiveRunCancelDisposition::ClearTranscriptSelection:
        renderer_.clear_transcript_selection();
        snapshot.status.clear();
        return to_input_handling(renderer_.request_render());
      case detail::ActiveRunCancelDisposition::RequestStop:
        return to_input_handling(request_stop(state));
    }
  }
  if (is_action(active_event, TuiAction::CopySelection) && draft_state.selection_bounds())
  {
    pending_escape_clear = false;
    static_cast<void>(draft_state.copy_selection(snapshot));
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CopySelection) && renderer_.has_transcript_selection())
  {
    pending_escape_clear = false;
    static_cast<void>(renderer_.copy_transcript_selection());
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::Interrupt))
  {
    if (!draft.text.empty())
    {
      static_cast<void>(action_controller_.clear_draft_for_interrupt());
      snapshot.selected_slash_command_index = selected_slash_command_index;
      return to_input_handling(renderer_.render());
    }
    request_close_after_submit(state);
    return InputHandling::Handled;
  }
  bool const active_ctrl_d_delete_forward = active_event.key == Key::CtrlD && is_action(active_event, TuiAction::DeleteForward) && !draft.text.empty();
  if (is_action(active_event, TuiAction::Exit) && !active_ctrl_d_delete_forward)
  {
    request_close_after_submit(state);
    return InputHandling::Handled;
  }
  if (active_event.key == Key::Character)
  {
    insert_active_text(active_input);
    return to_input_handling(renderer_.request_render());
  }
  if (draft_state.extend_selection_for_key(active_event.key, snapshot))
  {
    renderer_.clear_transcript_selection();
    return to_input_handling(renderer_.request_render());
  }
  if (active_event.key == Key::CtrlHome || active_event.key == Key::CtrlEnd)
  {
    pending_escape_clear = false;
    history_index.reset();
    draft_input.clear();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    path_completion_force_active = false;
    draft_scroll_offset = 0;
    draft_state.clear_selection();
    draft.cursor = active_event.key == Key::CtrlHome ? 0 : draft.text.size();
    draft.vertical_column = std::string::npos;
    draft.yank_start = std::string::npos;
    draft.yank_end = std::string::npos;
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

// Slash, file-reference, path-forced and open-selection autocomplete acceptance.
RuntimeActiveRunController::InputHandling RuntimeActiveRunController::handle_completion_acceptance(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_state = draft_state_;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;

  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.slash_palette_active())
  {
    selected_slash_command_index = clamp_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
    if (auto const disabled_reason = slash_command_selection_disabled_reason(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index))
    {
      snapshot.status = "command disabled: " + *disabled_reason;
      ava::core::Application::instance().terminal_context().beep();
    }
    else
    {
      draft_state.clear_selection();
      auto selection = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
      static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
      selected_slash_command_index = 0;
      path_completion_force_active = false;
      draft_scroll_offset = 0;
      history_index.reset();
      draft_input.clear();
      snapshot.status = "command selected - press Enter to queue";
    }
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.file_reference_palette_active())
  {
    selected_slash_command_index = navigation_.clamp_completion(selected_slash_command_index);
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(selected_slash_command_index))
    {
      snapshot.status = "reference disabled: " + *disabled_reason;
      ava::core::Application::instance().terminal_context().beep();
      return to_input_handling(renderer_.request_render());
    }
    auto selection = navigation_.selected_completion_text(selected_slash_command_index);
    draft_state.clear_selection();
    static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
    selected_slash_command_index = 0;
    path_completion_force_active = false;
    draft_scroll_offset = 0;
    history_index.reset();
    draft_input.clear();
    snapshot.status = "file reference selected";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.path_completion_palette_active())
  {
    selected_slash_command_index = navigation_.clamp_completion(selected_slash_command_index);
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(selected_slash_command_index))
    {
      snapshot.status = "path disabled: " + *disabled_reason;
      ava::core::Application::instance().terminal_context().beep();
      return to_input_handling(renderer_.request_render());
    }
    auto selection = navigation_.selected_completion_text(selected_slash_command_index);
    draft_state.clear_selection();
    static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
    selected_slash_command_index = 0;
    path_completion_force_active = false;
    draft_scroll_offset = 0;
    history_index.reset();
    draft_input.clear();
    snapshot.status = "path selected";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept))
  {
    auto const was_suppressed = slash_palette_suppressed;
    slash_palette_suppressed = false;
    path_completion_force_active = true;
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      slash_palette_suppressed = was_suppressed;
      path_completion_force_active = false;
    }
    else
    {
      if (match_count == 1)
      {
        if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(0))
        {
          path_completion_force_active = false;
          snapshot.status = "path disabled: " + *disabled_reason;
          ava::core::Application::instance().terminal_context().beep();
          return to_input_handling(renderer_.request_render());
        }
        auto selection = navigation_.selected_completion_text(0);
        draft_state.clear_selection();
        static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
        path_completion_force_active = false;
        draft_scroll_offset = 0;
        snapshot.status = "path selected";
      }
      else
      {
        selected_slash_command_index = 0;
        snapshot.status = "path suggestions";
      }
      return to_input_handling(renderer_.request_render());
    }
  }
  return InputHandling::Unhandled;
}

// Restore, nonblocking command, disabled-completion rejection, steering/follow-up
// queue, NewLine/editor/suspend/image/dequeue/jump/submit.
RuntimeActiveRunController::InputHandling RuntimeActiveRunController::handle_active_command_input(RuntimeActiveRunState& state,
                                                                                                  runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;
  auto& draft_state = draft_state_;
  auto& jump_mode = draft_state_.jump_mode;
  auto& pending_escape_clear = draft_state_.pending_escape_clear;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;

  if (is_action(active_event, TuiAction::NewLine))
  {
    draft_state.insert_newline();
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ExternalEditor))
  {
    return to_input_handling(action_controller_.open_external_editor());
  }
  if (is_action(active_event, TuiAction::Suspend))
  {
    return to_input_handling(action_controller_.suspend_to_background());
  }
  if (is_action(active_event, TuiAction::ClipboardPasteImage))
  {
    return to_input_handling(action_controller_.paste_clipboard_image());
  }
  if (is_action(active_event, TuiAction::MessageDequeue))
  {
    return to_input_handling(restore_latest_queued_message(state));
  }
  if (is_action(active_event, TuiAction::JumpForward) || is_action(active_event, TuiAction::JumpBackward))
  {
    pending_escape_clear = false;
    history_index.reset();
    draft_input.clear();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    path_completion_force_active = false;
    jump_mode = is_action(active_event, TuiAction::JumpForward) ? ComposerJumpMode::Forward : ComposerJumpMode::Backward;
    snapshot.status = is_action(active_event, TuiAction::JumpForward) ? "jump forward: type character" : "jump backward: type character";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessageFollowUp))
  {
    renderer_.clear_transcript_selection();
    return to_input_handling(queue_active_draft(state, true));
  }
  if (is_action(active_event, TuiAction::Submit))
  {
    if (auto handled = reject_disabled_visible_completion())
      return to_input_handling(*handled);
    if (active_event.key == Key::Enter && draft_state.convert_backslash_enter_to_newline(snapshot))
      return to_input_handling(renderer_.request_render());
    renderer_.clear_transcript_selection();
    if (auto handled = run_active_command(state))
      return to_input_handling(*handled);
    return to_input_handling(queue_active_draft(state, false));
  }
  return InputHandling::Unhandled;
}

}  // namespace ava::tui
