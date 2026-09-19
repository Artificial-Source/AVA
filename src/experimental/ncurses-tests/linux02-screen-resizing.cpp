#include "sys.h"
#include "Application.h"
#include <cctype>
#include <clocale>
#include <cstring>
#include <iostream>
#include <sstream>
#include <curses.h>

// Test resizing of terminal windows and being notified.

#undef NO_KITTY_PROTOCOL

int main()
{
#ifdef NO_KITTY_PROTOCOL
  setlocale(LC_ALL, "");
  // From https://man7.org/linux/man-pages/man3/resizeterm.3x.html:
  //
  // If the application has not set up a handler for SIGWINCH when it
  // initializes ncurses (by calling initscr(3X) or newterm(3X)), then
  // ncurses establishes a SIGWINCH handler that notifies the library
  // when a window-resizing event has occurred.  The library checks for
  // this notification.

  // From https://invisible-island.net/ncurses/man/curs_util.3x.html
  // use_env   use_tioctl   Summary
  // TRUE      TRUE         ncurses updates LINES and COLUMNS based on operating system calls.
  ::use_env(TRUE);
  ::use_tioctl(TRUE);

  initscr();
#else
  Application application("linux04-attributes");
  ava::tui::terminal::Context& terminal_context = application.terminal_context();
  terminal_context.initialize();
  noraw();
  curs_set(TRUE);
#endif

  // IMPORTANT: if `nocbreak()` is used then *instead* of returning KEY_RESIZE,
  // the input buffer in flushed with a synthesized newline added.
  nocbreak();
  echo();
  addstr("Testing with nocbreak/echo - type \"quit<Enter>\" - to switch to cbreak/noecho\n");

  int rows;
  int cols;
  int last_rows = -1;
  int last_cols = -1;

  // This limits the number of characters that can be *typed* to 7 (what is passed to wgetnstr below).
  char input_buf[8];
  do
  {
    std::ostringstream text;
    // Get current terminal size and print it.
    getmaxyx(stdscr, rows, cols);
    if (rows != last_rows || cols != last_cols)
    {
      text << "cols = " << cols << ", rows = " << rows << '\n';
      addstr(text.str().c_str());
      refresh();
    }
    wgetnstr(stdscr, input_buf, sizeof(input_buf) - 1);
    addstr("Received: [");
    addstr(input_buf);
    addstr("]\n");
  } while (std::strcmp(input_buf, "quit") != 0);

  noecho();
  int mode = 0;

  for (;;)
  {
    addstr("Testing with noecho/");
    switch (mode)
    {
      case 0:
      {
        noraw();
        cbreak();
        addstr("cbreak/");
        break;
      }
      case 1:
      {
        nocbreak();
        raw();
        addstr("raw/");
        break;
      }
    }
    addstr(is_nl() ? "nl" : "nonl");
    addstr(" - type, t:toggle cbreak/raw, n:toggle nl/nonl, q:stop\n");

    //---------------------------------------------------------------------------
    // From https://man7.org/linux/man-pages/man3/curs_getch.3x.html:
    //
    // wgetch returns KEY_RESIZE, even if the window's keypad mode is disabled,
    // if ncurses has handled a SIGWINCH signal since wgetch was called;
    // see initscr(3X) and resizeterm(3X).
    // [...]
    // Except for the special case KEY_RESIZE, it is necessary to enable keypad for getch to return these codes.

    // keypad(stdscr, TRUE);       // Not necessary for KEY_RESIZE.

    // In cbreak mode we receive one character at a time, including KEY_RESIZE.
    int ch = 0;
    do
    {
      std::ostringstream text;
      if (ch != 0)
      {
        text << "ch = ";
        if (ch == KEY_RESIZE)
          text << "KEY_RESIZE";
        else if (std::isprint(ch))
          text << '\'' << static_cast<char>(ch) << '\'';
        else
          text << ch;
        text << ", ";
      }
      if (ch == 0 || ch == KEY_RESIZE)
      {
        // Get current terminal size and print it.
        getmaxyx(stdscr, rows, cols);
        text << "cols = " << cols << ", rows = " << rows << '\n';
      }
      else
        text << '\n';
      // Show new values on the screen.
      addstr(text.str().c_str());
      refresh();
      // getch() is the same as wgetch(stdscr).
      ch = getch();
    } while (ch != 't' && ch != 'n' && ch != 'q' && ch != 3);
    // Note that KEY_RESIZE is delivered despite the nocbreak() because it doesn't originate from the terminal.
    if (ch == 'q' || ch == 3)
      break;
    if (ch == 't')
      mode = (mode + 1) % 2;
    else if (ch == 'n')
    {
      if (is_nl())
        nonl();
      else
        nl();
    }
  }

#ifdef NO_KITTY_PROTOCOL
  endwin();
#endif
}
