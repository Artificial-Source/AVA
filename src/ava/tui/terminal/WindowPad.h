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
 public:
  WindowPad(columns_t window_width, Position window_bottom_left, Rendition rendition, Border border);

  void generate(bool blank_line_between_block_rows = true)
  {
    generate_grapheme_surface(data_.window_width_, blank_line_between_block_rows);
    uint32_t window_height = std::min(std::max(1U, content_rows_), config::max_composer_viewport_height) + border_.margin().height();
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
