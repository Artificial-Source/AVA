#include "sys.h"
#include "WindowPad.h"

namespace ava::tui::terminal {

WindowPad::WindowPad(Dimension window_size, Position window_pos, Rendition rendition, Border border) : WindowBorder(window_size, window_pos, rendition, border)
{
}

void WindowPad::do_pnoutrefresh(uint32_t pad_row, uint32_t n)
{
  DoutEntering(dc::terminal, "WindowPad::do_prefresh(" << pad_row << ", " << n << ")");
  Margin const margin{border_.margin()};
  Position const pos = outer_window_.getbegyx() + margin;
  Dimension const window_dimension = outer_window_.getmaxyx() - border_.margin();
  Dimension const screen_dimension{n, window_dimension.width()};
  if (need_border_refresh_)
  {
    outer_window_.wnoutrefresh();
    need_border_refresh_ = false;
  }
  pad_->pnoutrefresh({pad_row, 0}, pos, screen_dimension);
}

void WindowPad::pnoutrefresh(ScrollPosition scroll_position)
{
  Dimension window_dimension = outer_window_.getmaxyx() - border_.margin();
  if (!pad_.has_value())
  {
    // Generate the content of the pad for a window with the given width.
    Dimension const window_dimension = outer_window_.getmaxyx() - border_.margin();
    generate(window_dimension.width());
  }
  Dimension pad_dimension = pad_->getmaxyx();
  // Call Pad::generate with the correct number of columns, the width of this window, before calling this function.
  ASSERT(pad_dimension.width() == window_dimension.width());
  if (pad_dimension.height() < window_dimension.height())
    scroll_position = ScrollPosition::begin;

  // We always start appending from the top-left.
  pad_->move({0, 0});
  if (scroll_position == ScrollPosition::begin)
    do_pnoutrefresh(0, std::min(pad_dimension.height(), window_dimension.height()));
  else
    do_pnoutrefresh(pad_dimension.height() - window_dimension.height(), window_dimension.height());
}

} // namespace ava::tui::terminal
