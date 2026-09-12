#include "sys.h"
#include "Window.h"

#include <utility>
#include "debug.h"

namespace ava::tui::terminal {

Window::Window(Dimension size, Position pos, Rendition rendition, Border const& border) : WindowBorder(size, pos, rendition, border)
{
  if (has_margin())
  {
    InnerWindow::operator=(outer_window_.derwin(border_.margin()));
    set_background(rendition, false);
  }
  else
    // Keep exactly one ncurses handle when no parent/child relationship is needed.
    InnerWindow::operator=(std::move(outer_window_));
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
