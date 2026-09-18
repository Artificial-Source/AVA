#include <cctype>
#include <clocale>
#include <cstring>
#include <iostream>
#include <sstream>
#include <curses.h>

// Test resizing of terminal windows and being notified.

int main()
{
  // See linux00-initialization.cpp
  setlocale(LC_ALL, "");
  // From https://man7.org/linux/man-pages/man3/resizeterm.3x.html:
  //
  // If the application has not set up a handler for SIGWINCH when it
  // initializes ncurses (by calling initscr(3X) or newterm(3X)), then
  // ncurses establishes a SIGWINCH handler that notifies the library
  // when a window-resizing event has occurred.  The library checks for
  // this notification.
  initscr();

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

  cbreak();
  noecho();
  addstr("Testing with cbreak/noecho - type q - to stop\n");

  for (int cbreak_or_raw = 0; cbreak_or_raw != 2; ++cbreak_or_raw)
  {
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
    } while (ch != 'q');
    // Note that KEY_RESIZE is delivered despite the nocbreak() because it doesn't originate from the terminal.

    //---------------------------------------------------------------------------
    raw();
    addstr("Testing with raw/noecho - type q - to stop\n");
  }

  //---------------------------------------------------------------------------

  // See linux00-initialization.cpp
  endwin();
}
