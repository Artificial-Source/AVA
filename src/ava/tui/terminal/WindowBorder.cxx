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

void WindowBorder::draw_border()
{
  // Do not call this function on a window with an empty margin.
  ASSERT(has_margin());
  outer_window_.set_border(border_);
  need_border_refresh_ = true;
}

} // namespace ava::tui::terminal
