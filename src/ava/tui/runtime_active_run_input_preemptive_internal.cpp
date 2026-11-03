#include "sys.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/ActiveRunInbox.h"
#include "ava/tui/Composer.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_actions_internal.h"
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
ActiveRun::InputHandling ActiveRun::handle_preemptive_input(runtime_input::RuntimeInput const& active_input)
{
  using Key = terminal::Key;
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;
  auto const& draft = draft_state_.draft();

  if (auto handled = navigation_.handle_sidebar_drawer_input(active_event))
    return to_input_handling(*handled);
  if (draft_state_.jump_mode() != ComposerJumpMode::None)
  {
    if (is_action(active_event, TuiAction::JumpForward) || is_action(active_event, TuiAction::JumpBackward))
    {
      draft_state_.cancel_jump();
      snapshot.status = "jump cancelled";
      return to_input_handling(renderer_.request_render());
    }
    if (auto const target = printable_jump_target(active_input))
    {
      bool const forward = draft_state_.jump_mode() == ComposerJumpMode::Forward;
      snapshot.status = draft_state_.jump_to_character(*target) ? (forward ? "jumped forward" : "jumped backward") : "jump character not found";
      return to_input_handling(renderer_.request_render());
    }
    draft_state_.cancel_jump();
  }
  if (is_action(active_event, TuiAction::CopyLatestAssistant))
    return to_input_handling(action_controller_.copy_latest_assistant_message());
  if (is_action(active_event, TuiAction::PromptStash))
    return to_input_handling(prompt_stash_.trigger());
  if (active_event.key == Key::Escape || is_action(active_event, TuiAction::Cancel))
  {
    switch (detail::active_run_cancel_disposition(draft_state_.selection_bounds().has_value(), renderer_.transcript().has_selection()))
    {
      case detail::ActiveRunCancelDisposition::ClearDraftSelection:
        draft_state_.clear_selection();
        snapshot.status.clear();
        return to_input_handling(renderer_.request_render());
      case detail::ActiveRunCancelDisposition::ClearTranscriptSelection:
        renderer_.clear_transcript_selection();
        snapshot.status.clear();
        return to_input_handling(renderer_.request_render());
      case detail::ActiveRunCancelDisposition::RequestStop:
        return to_input_handling(request_stop());
    }
  }
  if (is_action(active_event, TuiAction::CopySelection) && draft_state_.selection_bounds())
  {
    draft_state_.cancel_escape();
    static_cast<void>(draft_state_.copy_selection(snapshot));
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CopySelection) && renderer_.transcript().has_selection())
  {
    draft_state_.cancel_escape();
    static_cast<void>(renderer_.copy_transcript_selection());
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::Interrupt))
  {
    if (!draft.text.empty())
    {
      static_cast<void>(action_controller_.clear_draft_for_interrupt());
      snapshot.selected_slash_command_index = draft_state_.completion_index();
      return to_input_handling(renderer_.render());
    }
    request_close_after_submit();
    return InputHandling::Handled;
  }
  bool const active_ctrl_d_delete_forward = active_event.key == Key::CtrlD && is_action(active_event, TuiAction::DeleteForward) && !draft.text.empty();
  if (is_action(active_event, TuiAction::Exit) && !active_ctrl_d_delete_forward)
  {
    request_close_after_submit();
    return InputHandling::Handled;
  }
  if (active_event.key == Key::Character)
  {
    draft_state_.insert_input(active_input, snapshot, ComposerEditContext::ActiveRun);
    return to_input_handling(renderer_.request_render());
  }
  if (draft_state_.extend_selection_for_key(active_event.key, snapshot))
  {
    renderer_.clear_transcript_selection();
    return to_input_handling(renderer_.request_render());
  }
  if (active_event.key == Key::CtrlHome || active_event.key == Key::CtrlEnd)
  {
    draft_state_.move_document_boundary(active_event.key == Key::CtrlEnd, ComposerEditContext::ActiveRun);
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

// Slash, file-reference, path-forced and open-selection autocomplete acceptance.
ActiveRun::InputHandling ActiveRun::handle_completion_acceptance(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;
  auto const& draft = draft_state_.draft();

  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.slash_palette_active())
  {
    draft_state_.select_completion(clamp_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index()));
    if (auto const disabled_reason =
            slash_command_selection_disabled_reason(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index()))
    {
      snapshot.status = "command disabled: " + *disabled_reason;
      ava::core::Application::instance().terminal_context().beep();
    }
    else
    {
      auto selection = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index());
      draft_state_.accept_completion(std::move(selection.text), selection.cursor);
      snapshot.status = "command selected - press Enter to queue";
    }
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.file_reference_palette_active())
  {
    draft_state_.select_completion(navigation_.clamp_completion(draft_state_.completion_index()));
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(draft_state_.completion_index()))
    {
      snapshot.status = "reference disabled: " + *disabled_reason;
      ava::core::Application::instance().terminal_context().beep();
      return to_input_handling(renderer_.request_render());
    }
    auto selection = navigation_.selected_completion_text(draft_state_.completion_index());
    draft_state_.accept_completion(std::move(selection.text), selection.cursor);
    snapshot.status = "file reference selected";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.path_completion_palette_active())
  {
    draft_state_.select_completion(navigation_.clamp_completion(draft_state_.completion_index()));
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(draft_state_.completion_index()))
    {
      snapshot.status = "path disabled: " + *disabled_reason;
      ava::core::Application::instance().terminal_context().beep();
      return to_input_handling(renderer_.request_render());
    }
    auto selection = navigation_.selected_completion_text(draft_state_.completion_index());
    draft_state_.accept_completion(std::move(selection.text), selection.cursor);
    snapshot.status = "path selected";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept) &&
      draft_state_.force_path_completion(navigation_, snapshot, ava::core::Application::instance().terminal_context(), ComposerEditContext::ActiveRun))
    return to_input_handling(renderer_.request_render());
  return InputHandling::Unhandled;
}

// Restore, nonblocking command, disabled-completion rejection, steering/follow-up
// queue, NewLine/editor/suspend/image/dequeue/jump/submit.
ActiveRun::InputHandling ActiveRun::handle_active_command_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;

  if (is_action(active_event, TuiAction::NewLine))
  {
    draft_state_.insert_newline();
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
    return to_input_handling(restore_latest_queued_message());
  }
  if (is_action(active_event, TuiAction::JumpForward) || is_action(active_event, TuiAction::JumpBackward))
  {
    draft_state_.begin_jump(is_action(active_event, TuiAction::JumpForward) ? ComposerJumpMode::Forward : ComposerJumpMode::Backward);
    snapshot.status = is_action(active_event, TuiAction::JumpForward) ? "jump forward: type character" : "jump backward: type character";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessageFollowUp))
  {
    renderer_.clear_transcript_selection();
    return to_input_handling(queue_active_draft(true));
  }
  if (is_action(active_event, TuiAction::Submit))
  {
    if (auto handled = reject_disabled_visible_completion())
      return to_input_handling(*handled);
    if (active_event.key == terminal::Key::Enter && draft_state_.convert_backslash_enter_to_newline(snapshot))
      return to_input_handling(renderer_.request_render());
    renderer_.clear_transcript_selection();
    if (auto handled = run_active_command())
      return to_input_handling(*handled);
    return to_input_handling(queue_active_draft(false));
  }
  return InputHandling::Unhandled;
}

}  // namespace ava::tui
