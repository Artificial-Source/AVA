#include "sys.h"
#include "terminal/ColorPair.h"
#include "terminal/Context.h"
#include "terminal/Window.h"
#include "tests/support/test_harness.h"

#include <cstdio>

namespace terminal = ava::tui::terminal;

namespace {

// Verify that Window exposes writable-area geometry for a non-empty margin and avoids creating a subwindow for an empty one.
//
// Runs ncurses against temporary files and repeatedly destroys margin-aware windows so parent/child handle ordering is exercised
// without writing to a real terminal.
void test_margin_aware_window_geometry_and_lifetime()
{
  FILE* input = std::tmpfile();
  FILE* output = std::tmpfile();
  if (!input || !output)
  {
    if (input)
      static_cast<void>(std::fclose(input));
    if (output)
      static_cast<void>(std::fclose(output));
    expect(false, "tmpfile must be available for terminal::Window tests");
    return;
  }

  ScopedEnvVar term_guard("TERM", "xterm-256color");
  terminal::Context terminal_context(output, input);

  for (int iteration = 0; iteration != 3; ++iteration)
  {
    terminal::Margin const margin{.top = 1, .bottom = 2, .left = 3, .right = 1};
    terminal::Window window({8, 12}, {2, 4}, {margin, terminal::ColorPair{}});

    terminal::Dimension const inner_size = window.getmaxyx();
    terminal::Position const inner_origin = window.getbegyx();
    expect(inner_size.height() == 5 && inner_size.width() == 8, "a margin-aware Window must expose only its writable dimensions");
    expect(inner_origin.row() == 3 && inner_origin.col() == 7, "a margin-aware Window must expose the writable area's screen origin");
    expect(window.is_subwin(), "a Window with a margin must expose a derived writable window");

    terminal::Dimension const outer_size = window.outer_window().getmaxyx();
    terminal::Position const outer_origin = window.outer_window().getbegyx();
    expect(outer_size.height() == 8 && outer_size.width() == 12, "outer_window must retain the margin-inclusive dimensions");
    expect(outer_origin.row() == 2 && outer_origin.col() == 4, "outer_window must retain the margin-inclusive screen origin");
    expect(!window.outer_window().is_subwin(), "outer_window must be the parent of the writable area");
  }

  {
    terminal::Window window({4, 7}, {1, 2});
    expect(!window.is_subwin(), "an empty-margin Window must not create an ncurses subwindow");
    expect(&window.outer_window() == static_cast<terminal::BasicWindow*>(&window),
           "an empty-margin Window must use one BasicWindow wrapper for inner and outer access");
  }

  static_cast<void>(std::fclose(input));
  static_cast<void>(std::fclose(output));
}

} // namespace

void run_terminal_window_tests()
{
  test_margin_aware_window_geometry_and_lifetime();
}
