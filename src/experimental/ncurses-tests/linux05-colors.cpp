#include "sys.h"
#include "terminal/Context.h"
#include "Application.h"

#include <array>
#include <iostream>
#include <sstream>
#include <curses.h>

int main()
{
  Application application("linux05-colors");
  ava::tui::terminal::Context& terminal_context = application.terminal_context();
  terminal_context.initialize();

  move(10, 0);

  std::array<int, 7> color_pair_index = {1, 2, 3, 4, 5, 6, 7};
  init_extended_pair(color_pair_index[0], COLOR_RED, COLOR_BLACK);
  init_extended_pair(color_pair_index[1], COLOR_GREEN, COLOR_BLACK);
  init_extended_pair(color_pair_index[2], COLOR_YELLOW, COLOR_BLACK);
  init_extended_pair(color_pair_index[3], COLOR_BLUE, COLOR_BLACK);
  init_extended_pair(color_pair_index[4], COLOR_MAGENTA, COLOR_BLACK);
  init_extended_pair(color_pair_index[5], COLOR_CYAN, COLOR_BLACK);
  init_extended_pair(color_pair_index[6], COLOR_WHITE, COLOR_BLACK);

  for (int i = 0; i != color_pair_index.size(); ++i)
  {
    wattr_set(stdscr, A_NORMAL, 0, &color_pair_index[i]);
    std::ostringstream ss;
    int foreground, background;
    extended_pair_content(color_pair_index[i], &foreground, &background);         // Returns COLOR_RED, COLOR_BLACK etc.
    int R, G, B;
    extended_color_content(foreground, &R, &G, &B);
    ss << foreground << ": " << R << ", " << G << ", " << B << "; ";
    extended_color_content(background, &R, &G, &B);
    ss << background << ": " << R << ", " << G << ", " << B << '\n';
    addstr(ss.str().c_str());
  }

  int color_redish = 0xfe0000;
  int pair_redish = 2;
  init_extended_pair(pair_redish, color_redish, COLOR_BLACK);
  wattr_set(stdscr, A_NORMAL, 0, &pair_redish);
  waddstr(stdscr, "Hello world\n");

  int r, g, b;
  if (extended_color_content(color_redish, &r, &g, &b) == OK)
    printw("%d: %d, %d, %d\n", color_redish, r, g, b);

  refresh();
  wint_t wch;
  get_wch(&wch);
}
