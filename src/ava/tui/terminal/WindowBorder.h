#pragma once

#include "BasicWindow.h"
#include "Border.h"
#include "Dimension.h"
#include "Position.h"

namespace ava::tui::terminal {

class WindowBorder
{
 protected:
  BasicWindow outer_window_;            // The margin-inclusive parent, or an empty wrapper when the margin is empty.
  Border const border_;                 // The immutable border and margin configuration.
  bool need_border_refresh_ = false;    // True iff the outer border must be staged before refreshing the writable area.

 public:
  // Construct a Window of `size` at screen position `pos`, using the immutable margin and styling in `border`.
  // `rendition` is used to clear the whole window, including the margin, before drawing the one-character border around
  // the window.

  // The inherited BasicWindow coordinates and dimensions describe the writable interior. The border is drawn on the
  // margin-inclusive outer window. `border.margin()` must leave at least one interior row and column.
  WindowBorder(Dimension size, Position pos, Rendition rendition, Border const& border);

  WindowBorder(WindowBorder const&) = delete;
  WindowBorder& operator=(WindowBorder const&) = delete;
  WindowBorder(WindowBorder&&) = delete;
  WindowBorder& operator=(WindowBorder&&) = delete;

  // Accessors.

  Border const& border() const { return border_; }

  // Return true if this Window has a margin (and border).
  bool has_margin() const { return !border_.empty(); }

  // Redraw the configured border on the margin-inclusive outer window.
  //
  // Calling this on a Window with an empty margin is a programming error because no separate outer window exists.
  void draw_border();

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
