#include "terminal/Context.h"
#include "terminal/Window.h"

#include <iostream>

namespace terminal = ava::tui::terminal;

int main()
{
  int wch;
  {
    terminal::Context terminal_context;

    terminal::Dimension const size{15, 20};
    terminal::Position const top_left{(terminal_context.rows() - size.height()) / 2, 10};
    terminal::Margin const margin{.top = 1, .bottom = 1, .left = 1, .right = 1};
    terminal::ColorPair const dark_red_colorpair(terminal_context.create_color_pair({}, {0x880000}));
    terminal::Rendition const dark_blue_rendition(terminal_context.create_color_pair({}, {0x000044}));
    auto long_line = u8"Dark red window: αβγ this line is longer than the width of the window.";

    terminal::Window window1(size, top_left, {margin, dark_red_colorpair});
    window1.set_background(dark_blue_rendition);
    window1.addstr(long_line);

    {
      terminal::Window window2(size, top_left + terminal::Margin{.top = 5, .left = 5}, {margin, dark_red_colorpair});
      window2.addstr(long_line);

      window1.refresh();
      window2.refresh();
      wch = terminal_context.get_wch();
    }
    window1.refresh();
    wch = terminal_context.get_wch();
  }
  std::cout << "Program terminated with " << wch << std::endl;
}
