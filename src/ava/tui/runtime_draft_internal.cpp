#include "sys.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/core/Application.h"

#include <algorithm>

namespace ava::tui {

std::size_t RuntimeDraftState::max_draft_scroll_offset(ComposerSnapshot const& snapshot, std::size_t height) const
{
  auto const main_width = composer_main_width(snapshot);
  auto const composer_lines = detail::composer_block_line_count(snapshot, height, main_width);
  auto const input_lines = detail::input_render_line_spans(draft.text, main_width).size();
  auto const policy = detail::composer_layout_policy(snapshot, height);
  auto const layout = detail::composer_input_layout(input_lines, composer_lines, 0, policy.composer_top_padding_lines);
  return input_lines > layout.visible_input_lines ? input_lines - layout.visible_input_lines : std::size_t{0};
}

void RuntimeDraftState::clear_selection()
{
  draft_selection_anchor = std::string::npos;
  draft_selection_cursor = std::string::npos;
  mouse_selecting = false;
}

void RuntimeDraftState::reset_for_session_transition()
{
  clear_selection();
  reset_composer_draft(draft);
  input_history.clear();
  history_index.reset();
  draft_input.clear();
  jump_mode = ComposerJumpMode::None;
  selected_slash_command_index = 0;
  slash_palette_suppressed = false;
  path_completion_force_active = false;
  draft_scroll_offset = 0;
  pending_escape_clear = false;
}

std::optional<std::pair<std::size_t, std::size_t>> RuntimeDraftState::selection_bounds() const
{
  if (draft_selection_anchor == std::string::npos || draft_selection_cursor == std::string::npos)
    return std::nullopt;
  auto start = clamp_composer_draft_cursor(draft.text, draft_selection_anchor);
  auto end = clamp_composer_draft_cursor(draft.text, draft_selection_cursor);
  if (end < start)
    std::swap(start, end);
  if (start == end)
    return std::nullopt;
  return std::pair{start, end};
}

bool RuntimeDraftState::replace_selection(std::string_view replacement)
{
  auto const bounds = selection_bounds();
  if (!bounds)
    return false;
  auto const changed = replace_composer_draft_range(draft, bounds->first, bounds->second, replacement);
  clear_selection();
  return changed;
}

bool RuntimeDraftState::delete_selection()
{
  return replace_selection(std::string_view{});
}

std::optional<std::string> RuntimeDraftState::selected_text() const
{
  auto const bounds = selection_bounds();
  if (!bounds || bounds->first >= bounds->second || bounds->second > draft.text.size())
    return std::nullopt;
  return draft.text.substr(bounds->first, bounds->second - bounds->first);
}

bool RuntimeDraftState::copy_selection(ComposerSnapshot& snapshot)
{
  auto const text = selected_text();
  if (!text || text->empty())
  {
    snapshot.status = "no selection to copy";
    ava::core::Application::instance().terminal_context().beep();
    return false;
  }
  auto const copied = runtime_transcript::copy_text_to_terminal_clipboard(*text);
  snapshot.status = copied ? "selection copy request sent" : "clipboard copy failed";
  if (!copied)
    ava::core::Application::instance().terminal_context().beep();
  return copied;
}

void RuntimeDraftState::extend_selection(TuiAction movement, ComposerSnapshot& snapshot)
{
  pending_escape_clear = false;
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  slash_palette_suppressed = false;
  path_completion_force_active = false;
  draft_scroll_offset = 0;
  auto const anchor = draft_selection_anchor == std::string::npos ? clamp_composer_draft_cursor(draft.text, draft.cursor) : draft_selection_anchor;
  static_cast<void>(apply_composer_draft_action(draft, movement));
  draft.cursor = clamp_composer_draft_cursor(draft.text, draft.cursor);
  draft_selection_anchor = anchor;
  draft_selection_cursor = draft.cursor;
  snapshot.status = selection_bounds() ? "selection active" : "selection boundary";
}

void RuntimeDraftState::extend_selection_to(std::size_t target, ComposerSnapshot& snapshot)
{
  pending_escape_clear = false;
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  slash_palette_suppressed = false;
  path_completion_force_active = false;
  draft_scroll_offset = 0;
  auto const anchor = draft_selection_anchor == std::string::npos ? clamp_composer_draft_cursor(draft.text, draft.cursor) : draft_selection_anchor;
  draft.cursor = clamp_composer_draft_cursor(draft.text, target);
  draft.vertical_column = std::string::npos;
  draft.yank_start = std::string::npos;
  draft.yank_end = std::string::npos;
  draft_selection_anchor = anchor;
  draft_selection_cursor = draft.cursor;
  snapshot.status = selection_bounds() ? "selection active" : "selection boundary";
}

bool RuntimeDraftState::extend_selection_for_key(Key key, ComposerSnapshot& snapshot)
{
  switch (key)
  {
    case Key::ShiftArrowUp:
      extend_selection(TuiAction::CursorUp, snapshot);
      return true;
    case Key::ShiftArrowDown:
      extend_selection(TuiAction::CursorDown, snapshot);
      return true;
    case Key::ShiftArrowLeft:
      extend_selection(TuiAction::CursorLeft, snapshot);
      return true;
    case Key::ShiftArrowRight:
      extend_selection(TuiAction::CursorRight, snapshot);
      return true;
    case Key::ShiftCtrlArrowLeft:
    case Key::ShiftAltArrowLeft:
      extend_selection(TuiAction::CursorWordLeft, snapshot);
      return true;
    case Key::ShiftCtrlArrowRight:
    case Key::ShiftAltArrowRight:
      extend_selection(TuiAction::CursorWordRight, snapshot);
      return true;
    case Key::ShiftHome:
      extend_selection(TuiAction::CursorLineStart, snapshot);
      return true;
    case Key::ShiftEnd:
      extend_selection(TuiAction::CursorLineEnd, snapshot);
      return true;
    case Key::ShiftCtrlHome:
      extend_selection_to(0, snapshot);
      return true;
    case Key::ShiftCtrlEnd:
      extend_selection_to(draft.text.size(), snapshot);
      return true;
    default:
      return false;
  }
}

void RuntimeDraftState::insert_newline()
{
  pending_escape_clear = false;
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  path_completion_force_active = false;
  draft_scroll_offset = 0;
  if (!replace_selection("\n"))
    static_cast<void>(insert_composer_draft_text(draft, "\n"));
}

bool RuntimeDraftState::convert_backslash_enter_to_newline(ComposerSnapshot& snapshot)
{
  if (selection_bounds())
    return false;
  if (!replace_composer_backslash_before_cursor_with_newline(draft))
    return false;
  pending_escape_clear = false;
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  slash_palette_suppressed = false;
  path_completion_force_active = false;
  draft_scroll_offset = 0;
  snapshot.status = "newline inserted";
  return true;
}

}  // namespace ava::tui
