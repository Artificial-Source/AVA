#include "sys.h"
#include "WindowPad.h"

namespace ava::tui::terminal {

WindowPad::WindowPad(columns_t window_width, Position window_bottom_left, Rendition rendition, Border border)
  : WindowBorder(window_width, window_bottom_left, rendition, border)
{
}

void WindowPad::do_pnoutrefresh(uint32_t pad_row)
{
  DoutEntering(dc::terminal, "WindowPad::do_prefresh(" << pad_row << ")");

  // pad_row must be a valid row, or point to one beyond the last content row.
  ASSERT(pad_row <= content_rows_);

  if (!pad_.is_initialized())
  {
    // Construct the ncurses pad and write the grapheme clusters to it.
    build();
    pad_.set_background(outer_window_.get_background(), false);
    if (has_cursor_)
    {
      // This pad is the compose area. Keep the cursor inside and make it visible.
      pad_.leaveok(false);
      pad_.curs_set(config::cursor_visibility);
      core::Application::instance().terminal_context().reapply_cursor_settings();
    }
  }

  Margin const margin{border_.margin()};
  Position const pos = outer_window_.getbegyx() + margin;
  Dimension const viewport_size = outer_window_.getmaxyx() - border_.margin();

  // 0: ╳ row 0             ⎤
  // 1: ╳ row 1             ⎥
  // 2: ╳                   ⎥- content_rows_ = 8
  // 3: ╳                   ⎥
  // 4: ╳╳╳viewport:╳╳╳╳╳╳╳╳⎥╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳
  // 5: ╳pad_row: text      ⎥                     ⎤  pad_row = 5
  // 6: ╳pad_row+1: text    ⎥                     ⎥
  // 7: ╳pad_row+2: text    ⎦                     ⎥- viewport_size.height()
  // 8: ╳             <empty line>                ⎦
  // 9: ╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳
  //
  bool const empty_line_available = content_rows_ - pad_row > viewport_size.height();

  // Make room for the cursor if the last line spans the full width.
  if (has_cursor_ && last_line_is_full_ && !empty_line_available)
    ++pad_row;

  if (need_border_refresh_)
  {
    outer_window_.wnoutrefresh();
    need_border_refresh_ = false;
  }

  pad_.pnoutrefresh({pad_row, 0}, pos, viewport_size);
}

void WindowPad::pnoutrefresh(ScrollPosition scroll_position)
{
  // Call set_height before calling this function.
  ASSERT(outer_window_.is_initialized());

  uint32_t const viewport_height = outer_window_.getmaxyx().height() - border_.margin().height();
  uint32_t const content_rows = Pad::content_rows();
  if (content_rows < viewport_height)
    scroll_position = ScrollPosition::begin;

  // We always start appending from the top-left.
  if (scroll_position == ScrollPosition::begin)
    do_pnoutrefresh(0);
  else
    do_pnoutrefresh(content_rows - viewport_height);
}

} // namespace ava::tui::terminal
