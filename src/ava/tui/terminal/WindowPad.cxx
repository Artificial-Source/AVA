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

  if (!pad_.is_initialized())
  {
    // Construct the ncurses pad and write the grapheme clusters to it.
    build();
    pad_.set_background(outer_window_.get_background(), false);
  }

  Margin const margin{border_.margin()};
  Position const pos = outer_window_.getbegyx() + margin;
  Dimension const viewport_size = outer_window_.getmaxyx() - border_.margin();
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
