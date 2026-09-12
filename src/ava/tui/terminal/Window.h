#pragma once

#include "WindowBorder.h"

namespace ava::tui::terminal {

using InnerWindow = BasicWindow;        // The writable area inside the margin.

// Own an ncurses window whose inherited BasicWindow interface addresses only the writable area inside an immutable border margin.
//
// A non-empty margin creates a derived ncurses window for the inherited interface and retains its parent as `outer_window()`.
// An empty margin creates no derived window: the inherited BasicWindow is the sole ncurses handle and is also returned by
// `outer_window()`. Margins must leave at least one writable row and column; an oversized margin is a programming error rather
// than being clamped.
//
// Window always creates and owns a new ncurses window. It cannot wrap `stdscr` or another ncurses-owned WINDOW; use BasicWindow
// directly for those handles. Window is deliberately immovable because replacing either of its related handles would have to
// destroy the derived window before its parent.
class Window : public InnerWindow, public WindowBorder
{
 public:
  Window(Dimension size, Position pos, Rendition rendition, Border const& border = {});

  // Refresh the writable area, staging a newly drawn outer border first when necessary.
  void refresh();

  // Destroy the derived writable-area handle before destroying its margin-inclusive parent.
  ~Window();

  // Accessors.

  BasicWindow const& outer_window() const;
  BasicWindow& outer_window();

#ifdef CWDEBUG
  void print_on(std::ostream& os) const
  {
    os << "{";
    InnerWindow::print_members(os, "InnerWindow:{");
    os << "}, ";
    WindowBorder::print_members(os, "WindowBorder:{");
    os << "}}";
  }
#endif

  // We have a custrom print_on.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::tui::terminal
