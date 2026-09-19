#include "sys.h"
#include "Application.h"
#include "terminal/Context.h"

#include <array>
#include <sstream>

namespace terminal = ava::tui::terminal;

int main()
{
  Application application("linux05-colors");
  terminal::Context& terminal_context = application.terminal_context();
  terminal_context.initialize();

  terminal::BasicWindow& window = terminal_context.stdscr();
  window.move({10, 0});

  using enum terminal::ColorIndex;
  std::array<terminal::ColorPair, 7> color_pairs{
      terminal_context.create_color_pair(red, black),   terminal_context.create_color_pair(green, black),   terminal_context.create_color_pair(yellow, black),
      terminal_context.create_color_pair(blue, black),  terminal_context.create_color_pair(magenta, black), terminal_context.create_color_pair(cyan, black),
      terminal_context.create_color_pair(white, black),
  };

  for (terminal::ColorPair color_pair : color_pairs)
  {
    window.color_set(color_pair);
    std::ostringstream ss;
    if (auto const pair_content = terminal_context.color_pair_content(color_pair))
    {
      if (auto const foreground = terminal_context.color_content(pair_content->foreground_index))
        ss << pair_content->foreground_index << ": " << foreground->red << ", " << foreground->green << ", " << foreground->blue << "; ";
      if (auto const background = terminal_context.color_content(pair_content->background_index))
        ss << pair_content->background_index << ": " << background->red << ", " << background->green << ", " << background->blue;
    }
    ss << '\n';
    window.addstr(ss.str().c_str());
  }

  terminal::ColorPair const reddish_pair = terminal_context.create_color_pair(terminal::Color{0xfe0000}, black);
  window.color_set(reddish_pair);
  window.addstr("Hello world\n");

  if (auto const pair_content = terminal_context.color_pair_content(reddish_pair))
  {
    if (auto const reddish = terminal_context.color_content(pair_content->foreground_index))
      window.printw("%d: %d, %d, %d\n", pair_content->foreground_index, reddish->red, reddish->green, reddish->blue);
  }

  window.refresh();
  static_cast<void>(terminal_context.get_wch());
}
