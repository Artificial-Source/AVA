#pragma once

#include "BasicWindow.h"

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
class Window : public InnerWindow
{
 private:
  BasicWindow outer_window_;            // The margin-inclusive parent, or an empty wrapper when the margin is empty.
  Border const border_;                 // The immutable border and margin configuration.
  bool need_border_refresh_ = false;    // True iff the outer border must be staged before refreshing the writable area.

 public:
  // Construct a Window of `size` at screen position `pos`, using the immutable margin and styling in `border`.
  // `rendition` is used to clear the whole window, including the margin, before drawing the one-character border around
  // the window.

  // The inherited BasicWindow coordinates and dimensions describe the writable interior. The border is drawn on the
  // margin-inclusive outer window. `border.margin()` must leave at least one interior row and column.
  Window(Dimension size, Position pos, Rendition rendition, Border const& border = {});

  Window(Window const&) = delete;
  Window& operator=(Window const&) = delete;
  Window(Window&&) = delete;
  Window& operator=(Window&&) = delete;

  // Destroy the derived writable-area handle before destroying its margin-inclusive parent.
  ~Window();

  // Accessors.

  BasicWindow const& outer_window() const;
  BasicWindow& outer_window();

  Border const& border() const { return border_; }

  // Return true if this Window has a margin (and border).
  bool has_margin() const { return !border_.empty(); }

  // Redraw the configured border on the margin-inclusive outer window.
  //
  // Calling this on a Window with an empty margin is a programming error because no separate outer window exists.
  void draw_border();

  // Refresh the writable area, staging a newly drawn outer border first when necessary.
  void refresh();

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
