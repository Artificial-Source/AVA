#include "sys.h"
#include "WindowPad.h"

namespace ava::tui::terminal {

WindowPad::WindowPad(Dimension window_size, Position window_pos, Rendition rendition, Border border)
  : WindowBorder(window_size, window_pos, rendition, border)
{
}

void WindowPad::do_pnoutrefresh(uint32_t pad_row, uint32_t n)
{
  DoutEntering(dc::terminal, "WindowPad::do_pnoutrefresh(" << pad_row << ", " << n << ")");
  Margin const margin{border_.margin()};
  Position const pos{margin.top, margin.left};
  Dimension const window_dimension = outer_window_.getmaxyx() - border_.margin();
  pad_->pnoutrefresh({pad_row, 0}, pos, window_dimension);
}

void WindowPad::prefresh(ScrollPosition scroll_position)
{
  Dimension window_dimension = outer_window_.getmaxyx() - border_.margin();
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
