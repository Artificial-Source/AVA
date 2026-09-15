#pragma once

#include "BasicWindow.h"
#include "Border.h"
#include "Dimension.h"
#include "Position.h"

namespace ava::tui::terminal {

class WindowBorder
{
 public:
  struct DelayedInitializationData
  {
    columns_t window_width_;
    Position window_bottom_left_;
    Rendition rendition_;

    // Construct a DelayedInitializationData object that won't be used.
    DelayedInitializationData() : rendition_{{}} {}
    DelayedInitializationData(columns_t window_width, Position window_bottom_left, Rendition rendition) :
      window_width_(window_width), window_bottom_left_(window_bottom_left), rendition_(rendition) { }

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

 protected:
  DelayedInitializationData data_;      // Data used to construct outer_window_ once set_height is being called.
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

  // Construct a WindowBorder for Window with width `window_width` and its bottom-left corner at `window_bottom_left`.
  // `rendition` and `border` as above. The outer_window_ is not created yet. Call set_height to (re)create outer_window_.
  WindowBorder(uint32_t window_width, Position window_bottom_left, Rendition rendition, Border const& border);

  WindowBorder(WindowBorder const&) = delete;
  WindowBorder& operator=(WindowBorder const&) = delete;
  WindowBorder(WindowBorder&&) = delete;
  WindowBorder& operator=(WindowBorder&&) = delete;

  // Redraw the configured border on the margin-inclusive outer window.
  //
  // Calling this on a Window with an empty margin is a programming error because no separate outer window exists.
  void draw_border();

  // Change, or set the height of the window. This might create the window, or move/resize it, keeping its bottom-left
  // corner in the same place.
  void set_height(uint32_t window_height);

  // Accessors.

  Border const& border() const { return border_; }

  // Return true if this Window has a margin (and border).
  bool has_margin() const { return !border_.empty(); }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
