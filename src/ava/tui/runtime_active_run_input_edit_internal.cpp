#include "sys.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime_active_run_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/terminal.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace ava::tui {

namespace {

constexpr std::size_t kKeyboardScrollRows = 3;

}  // namespace

// Composer edit/delete/cursor/undo/redo/yank.
RuntimeActiveRunController::InputHandling RuntimeActiveRunController::handle_composer_edit(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& draft = draft_state_.draft;
  auto& draft_state = draft_state_;
  auto& pending_escape_clear = draft_state_.pending_escape_clear;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;

  bool const active_ctrl_d_delete_forward = active_event.key == Key::CtrlD && is_action(active_event, TuiAction::DeleteForward) && !draft.text.empty();
  bool const active_delete_forward = is_action(active_event, TuiAction::DeleteForward) && (active_event.key != Key::CtrlD || active_ctrl_d_delete_forward);
  if (is_action(active_event, TuiAction::DeleteBackward) || active_delete_forward || is_action(active_event, TuiAction::DeleteWordBackward) ||
      is_action(active_event, TuiAction::DeleteWordForward) || is_action(active_event, TuiAction::DeleteToLineStart) ||
      is_action(active_event, TuiAction::DeleteToLineEnd) || is_action(active_event, TuiAction::ClearInput) || is_action(active_event, TuiAction::CursorLeft) ||
      is_action(active_event, TuiAction::CursorRight) || is_action(active_event, TuiAction::CursorLineStart) ||
      is_action(active_event, TuiAction::CursorLineEnd) || is_action(active_event, TuiAction::CursorWordLeft) ||
      is_action(active_event, TuiAction::CursorWordRight) || is_action(active_event, TuiAction::Undo) || is_action(active_event, TuiAction::Redo) ||
      is_action(active_event, TuiAction::Yank) || is_action(active_event, TuiAction::YankPop))
  {
    pending_escape_clear = false;
    history_index.reset();
    draft_input.clear();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    draft_scroll_offset = 0;
    if (is_action(active_event, TuiAction::DeleteBackward))
    {
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteBackward));
    }
    else if (active_delete_forward)
    {
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteForward));
    }
    else if (is_action(active_event, TuiAction::DeleteWordBackward))
    {
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteWordBackward));
    }
    else if (is_action(active_event, TuiAction::DeleteWordForward))
    {
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteWordForward));
    }
    else if (is_action(active_event, TuiAction::DeleteToLineStart))
    {
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteToLineStart));
    }
    else if (is_action(active_event, TuiAction::DeleteToLineEnd))
    {
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteToLineEnd));
    }
    else if (is_action(active_event, TuiAction::ClearInput))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::ClearInput));
    }
    else if (is_action(active_event, TuiAction::CursorLeft))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorLeft));
    }
    else if (is_action(active_event, TuiAction::CursorRight))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorRight));
    }
    else if (is_action(active_event, TuiAction::CursorLineStart))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorLineStart));
    }
    else if (is_action(active_event, TuiAction::CursorLineEnd))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorLineEnd));
    }
    else if (is_action(active_event, TuiAction::CursorWordLeft))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorWordLeft));
    }
    else if (is_action(active_event, TuiAction::CursorWordRight))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorWordRight));
    }
    else if (is_action(active_event, TuiAction::Undo))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::Undo));
    }
    else if (is_action(active_event, TuiAction::Redo))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::Redo));
    }
    else if (is_action(active_event, TuiAction::Yank))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::Yank));
    }
    else if (is_action(active_event, TuiAction::YankPop))
    {
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::YankPop));
    }
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

// Transcript/palette/history/vertical navigation in exact original order.
RuntimeActiveRunController::InputHandling RuntimeActiveRunController::handle_navigation_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_state = draft_state_;
  auto& pending_escape_clear = draft_state_.pending_escape_clear;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& input_history = draft_state_.input_history;

  if (is_action(active_event, TuiAction::TranscriptHalfPageUp))
  {
    navigation_.scroll_up(navigation_.transcript_page_size());
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::TranscriptHalfPageDown))
  {
    navigation_.scroll_down(navigation_.transcript_page_size());
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PageUp))
  {
    navigation_.scroll_up(navigation_.transcript_page_size());
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PageDown))
  {
    navigation_.scroll_down(navigation_.transcript_page_size());
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessagePrev))
  {
    navigation_.scroll_to_message_boundary(true);
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessageNext))
  {
    navigation_.scroll_to_message_boundary(false);
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::JumpToBottom))
  {
    navigation_.jump_to_bottom("live tail");
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PalettePrev) && navigation_.slash_palette_active())
  {
    pending_escape_clear = false;
    auto const match_count = filter_slash_commands(draft.text, draft.cursor, snapshot.slash_commands).size();
    if (match_count == 0)
    {
      snapshot.status = "no matching slash commands";
    }
    else
    {
      selected_slash_command_index = previous_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
      snapshot.status = "command " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PalettePrev) && navigation_.file_reference_palette_active())
  {
    pending_escape_clear = false;
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      snapshot.status = "no matching file references";
    }
    else
    {
      selected_slash_command_index = navigation_.previous_completion(selected_slash_command_index);
      snapshot.status = "reference " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PalettePrev) && navigation_.path_completion_palette_active())
  {
    pending_escape_clear = false;
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      snapshot.status = "no matching paths";
    }
    else
    {
      selected_slash_command_index = navigation_.previous_completion(selected_slash_command_index);
      snapshot.status = "path " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::HistoryPrev))
  {
    pending_escape_clear = false;
    draft_state.clear_selection();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    path_completion_force_active = false;
    draft_scroll_offset = 0;
    if (browse_composer_input_history(draft, input_history, history_index, draft_input, true))
    {
      snapshot.status = "history previous";
    }
    else
    {
      navigation_.scroll_up(kKeyboardScrollRows);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (active_event.key == Key::ArrowUp)
  {
    navigation_.scroll_up(kKeyboardScrollRows);
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CursorUp) && apply_composer_draft_action(draft, TuiAction::CursorUp))
  {
    pending_escape_clear = false;
    draft_state.clear_selection();
    history_index.reset();
    draft_input.clear();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

// Palette-next/history-next/arrow-down/cursor-down navigation in exact original order.
RuntimeActiveRunController::InputHandling RuntimeActiveRunController::handle_navigation_next_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_state = draft_state_;
  auto& pending_escape_clear = draft_state_.pending_escape_clear;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& input_history = draft_state_.input_history;

  if (is_action(active_event, TuiAction::PaletteNext) && navigation_.slash_palette_active())
  {
    pending_escape_clear = false;
    auto const match_count = filter_slash_commands(draft.text, draft.cursor, snapshot.slash_commands).size();
    if (match_count == 0)
    {
      snapshot.status = "no matching slash commands";
    }
    else
    {
      selected_slash_command_index = next_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
      snapshot.status = "command " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PaletteNext) && navigation_.file_reference_palette_active())
  {
    pending_escape_clear = false;
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      snapshot.status = "no matching file references";
    }
    else
    {
      selected_slash_command_index = navigation_.next_completion(selected_slash_command_index);
      snapshot.status = "reference " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PaletteNext) && navigation_.path_completion_palette_active())
  {
    pending_escape_clear = false;
    auto const match_count = navigation_.completion_match_count();
    if (match_count == 0)
    {
      snapshot.status = "no matching paths";
    }
    else
    {
      selected_slash_command_index = navigation_.next_completion(selected_slash_command_index);
      snapshot.status = "path " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::HistoryNext))
  {
    pending_escape_clear = false;
    draft_state.clear_selection();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    path_completion_force_active = false;
    draft_scroll_offset = 0;
    if (browse_composer_input_history(draft, input_history, history_index, draft_input, false))
    {
      snapshot.status = history_index ? "history next" : "history draft";
    }
    else
    {
      navigation_.scroll_down(kKeyboardScrollRows);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (active_event.key == Key::ArrowDown)
  {
    navigation_.scroll_down(kKeyboardScrollRows);
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CursorDown) && apply_composer_draft_action(draft, TuiAction::CursorDown))
  {
    pending_escape_clear = false;
    draft_state.clear_selection();
    history_index.reset();
    draft_input.clear();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

}  // namespace ava::tui
