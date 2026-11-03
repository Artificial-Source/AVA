#include "sys.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/Composer.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/terminal.h"

#include <cstddef>
#include <initializer_list>
#include <string>

namespace ava::tui {

namespace {

constexpr std::size_t kKeyboardScrollRows = 3;

}  // namespace

// Composer edit/delete/cursor/undo/redo/yank.
ActiveRun::InputHandling ActiveRun::handle_composer_edit(runtime_input::RuntimeInput const& active_input)
{
  using Key = terminal::Key;
  auto const& active_event = active_input.event;
  bool const active_ctrl_d_delete_forward =
      active_event.key == Key::CtrlD && is_action(active_event, TuiAction::DeleteForward) && !draft_state_.draft().text.empty();
  for (auto const action :
       {TuiAction::DeleteBackward, TuiAction::DeleteForward, TuiAction::DeleteWordBackward, TuiAction::DeleteWordForward, TuiAction::DeleteToLineStart,
        TuiAction::DeleteToLineEnd, TuiAction::ClearInput, TuiAction::CursorLeft, TuiAction::CursorRight, TuiAction::CursorLineStart, TuiAction::CursorLineEnd,
        TuiAction::CursorWordLeft, TuiAction::CursorWordRight, TuiAction::Undo, TuiAction::Redo, TuiAction::Yank, TuiAction::YankPop})
  {
    if (!is_action(active_event, action) || (action == TuiAction::DeleteForward && active_event.key == Key::CtrlD && !active_ctrl_d_delete_forward))
      continue;
    static_cast<void>(draft_state_.apply_edit(action, ComposerEditContext::ActiveRun));
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

// Transcript/palette/history/vertical navigation in exact original order.
ActiveRun::InputHandling ActiveRun::handle_navigation_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto const& snapshot = presentation_state_.screen();

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
  if (is_action(active_event, TuiAction::PalettePrev) && apply_composer_result(draft_state_.step_completion(CompletionStep::Previous, navigation_, snapshot)))
    return to_input_handling(renderer_.request_render());
  if (is_action(active_event, TuiAction::HistoryPrev))
  {
    if (draft_state_.browse_history(true))
    {
      presentation_state_.publish_status("history previous");
    }
    else
    {
      navigation_.scroll_up(kKeyboardScrollRows);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (active_event.key == terminal::Key::ArrowUp)
  {
    navigation_.scroll_up(kKeyboardScrollRows);
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CursorUp) && draft_state_.move_vertical(TuiAction::CursorUp))
    return to_input_handling(renderer_.request_render());
  return InputHandling::Unhandled;
}

// Palette-next/history-next/arrow-down/cursor-down navigation in exact original order.
ActiveRun::InputHandling ActiveRun::handle_navigation_next_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto const& snapshot = presentation_state_.screen();

  if (is_action(active_event, TuiAction::PaletteNext) && apply_composer_result(draft_state_.step_completion(CompletionStep::Next, navigation_, snapshot)))
    return to_input_handling(renderer_.request_render());
  if (is_action(active_event, TuiAction::HistoryNext))
  {
    if (draft_state_.browse_history(false))
    {
      presentation_state_.publish_status(draft_state_.history_index() ? "history next" : "history draft");
    }
    else
    {
      navigation_.scroll_down(kKeyboardScrollRows);
    }
    return to_input_handling(renderer_.request_render());
  }
  if (active_event.key == terminal::Key::ArrowDown)
  {
    navigation_.scroll_down(kKeyboardScrollRows);
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CursorDown) && draft_state_.move_vertical(TuiAction::CursorDown))
    return to_input_handling(renderer_.request_render());
  return InputHandling::Unhandled;
}

}  // namespace ava::tui
