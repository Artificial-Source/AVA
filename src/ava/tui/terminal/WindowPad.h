#pragma once

#include "Context.h"
#include "Pad.h"
#include "WindowBorder.h"
#include "ava/tui/config.h"

namespace ava::tui::terminal {

enum class ScrollPosition
{
  begin,
  end
};

class WindowPad : public WindowBorder, public Pad
{
 private:
  // The following, as well as Pad::has_cursor_, is only valid after calling generate.
  bool last_line_is_full_;      // Set to true iff `has_cursor` was true and the bottom line has a width equal to the grapheme surface.

 public:
  WindowPad(columns_t window_width, Position window_bottom_left, Rendition rendition, Border border);

  void generate(bool has_cursor, bool blank_line_between_block_rows)
  {
    has_cursor_ = has_cursor;
    columns_t const viewport_width = data_.window_width_ - border_.margin().width();
    generate_grapheme_surface(viewport_width, blank_line_between_block_rows);

    uint32_t window_height = std::min(std::max(1U, content_rows_), config::max_composer_viewport_height) + border_.margin().height();
    last_line_is_full_ = fitted_horizontal_layouts_.width_last_line() == viewport_width;

    // Make room for the cursor if the last line spans the full width and the window can grow.
    if (has_cursor && last_line_is_full_ && window_height < config::max_composer_viewport_height)
      ++window_height;

    set_height(window_height);
  }

  // Show a portion of pad with its first line in the top row of this window (ScrollPosition::begin),
  // or with its bottom line in the bottom row of this window (ScrollPosition::end), unless the height
  // of the pad is less than the height of the window; in that case ScrollPosition::begin is used.
  void pnoutrefresh(ScrollPosition scroll_position = ScrollPosition::begin);

  void prefresh(ScrollPosition scroll_position = ScrollPosition::begin)
  {
    pnoutrefresh(scroll_position);
    Context::doupdate();
  }

 private:
  // Publish the pad starting with `pad_row`. Called by prefresh.
  void do_pnoutrefresh(uint32_t pad_row);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
