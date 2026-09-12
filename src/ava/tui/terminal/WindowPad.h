#pragma once

#include "WindowBorder.h"
#include "Pad.h"

namespace ava::tui::terminal {

enum class ScrollPosition
{
  begin,
  end
};

  // Window
class WindowPad : public WindowBorder, public Pad
{
 public:
  WindowPad(Dimension window_size, Position window_pos, Rendition rendition, Border border);

  // Show a portion of pad with its first line in the top row of this window (ScrollPosition::begin),
  // or with its bottom line in the bottom row of this window (ScrollPosition::end), unless the height
  // of the pad is less than the height of the window; in that case ScrollPosition::begin is used.
  void prefresh(ScrollPosition scroll_position = ScrollPosition::begin);

 private:
  // Show `n` rows of this pad, starting with `pad_row`. Called by prefresh.
  void do_pnoutrefresh(uint32_t pad_row, uint32_t n);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
