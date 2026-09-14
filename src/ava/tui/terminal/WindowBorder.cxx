#include "sys.h"
#include "WindowBorder.h"
#include "debug.h"

namespace ava::tui::terminal {

WindowBorder::WindowBorder(Dimension size, Position pos, Rendition rendition, Border const& border) : outer_window_(size, pos), border_(border)
{
  // Clear the everything with the rendition that was passed to the border, including the writable area (inner window) before drawing the border.
  outer_window_.set_background(rendition);

  if (has_margin())
    draw_border();
}

WindowBorder::WindowBorder(uint32_t window_width, Position window_bottom_left, Rendition rendition, Border const& border)
  : data_(window_width, window_bottom_left, rendition), border_(border)
{
}

void WindowBorder::set_height(uint32_t window_height)
{
  // Implement updating.
  ASSERT(!outer_window_.is_initialized());

  Dimension size(window_height, data_.window_width_);
  Position top_left = data_.window_bottom_left_ - PositionOffset{window_height, 0};
  outer_window_.initialize(size, top_left);
  outer_window_.set_background(data_.rendition_);

  if (has_margin())
    draw_border();
}

void WindowBorder::draw_border()
{
  // Do not call this function on a window with an empty margin.
  ASSERT(has_margin());
  outer_window_.set_border(border_);
  need_border_refresh_ = true;
}

} // namespace ava::tui::terminal
