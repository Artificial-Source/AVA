#include "sys.h"
#include "Window.h"

#include <utility>
#include "debug.h"

namespace ava::tui::terminal {

Window::Window(Dimension size, Position pos, Border const& border) : outer_window_(size, pos), border_(border)
{
  if (!has_margin())
  {
    // Keep exactly one ncurses handle when no parent/child relationship is needed.
    InnerWindow::operator=(std::move(outer_window_));
    return;
  }

  // A Window margin must leave at least one writable row and column; increase the Window size or reduce its margin.
  ASSERT(border_.margin() < size);

  // Start the outer and inner surfaces with the border rendition. Call set_background after construction to choose a
  // different writable-area background.
  outer_window_.set_background({border_.colors(0)}, false);
  InnerWindow::operator=(outer_window_.derwin(border_.margin()));
  draw_border();

  // Clear the writable area after drawing. On a side with zero margin this may overwrite border spaces, but both surfaces
  // initially use the same rendition and the writable area remains authoritative there.
  erase();
}

Window::~Window()
{
  // ncurses requires a derived window to be deleted before its parent. An empty replacement destroys that child now,
  // before C++ starts destroying `outer_window_` and then the InnerWindow base.
  InnerWindow::operator=(InnerWindow{});
}

BasicWindow const& Window::outer_window() const
{
  return has_margin() ? outer_window_ : static_cast<InnerWindow const&>(*this);
}

BasicWindow& Window::outer_window()
{
  return has_margin() ? outer_window_ : static_cast<InnerWindow&>(*this);
}

void Window::draw_border()
{
  // Do not call this function on a window with an empty margin.
  ASSERT(has_margin());
  outer_window_.set_border(border_);
  need_border_refresh_ = true;
}

void Window::refresh()
{
  if (need_border_refresh_)
  {
    outer_window_.wnoutrefresh();
    need_border_refresh_ = false;
  }
  InnerWindow::refresh();
}

} // namespace ava::tui::terminal
