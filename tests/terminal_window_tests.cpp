#include "sys.h"
#include "support/test_harness.h"
#include "terminal/ColorPair.h"
#include "terminal/Context.h"
#include "terminal/Window.h"

#include <cstdio>
#include <string_view>

namespace terminal = ava::tui::terminal;

namespace {

// Verify that Context is the sole cursor-style owner and emits every DECSCUSR mapping while suppressing duplicate settings.
void test_context_cursor_settings()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  terminal::Context terminal_context(output.get(), input.get());

  long const untouched_default_begin = std::ftell(output.get());
  terminal_context.reapply_cursor_settings();
  expect(untouched_default_begin >= 0 && std::ftell(output.get()) == untouched_default_begin,
         "Context leaves the terminal's inherited default cursor unperturbed until AVA explicitly applies a setting");

  long const begin = std::ftell(output.get());
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Block, true});
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Block, false});
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Underline, true});
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Underline, false});
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Bar, true});
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Bar, false});
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Bar, false});
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Default});
  long const end = std::ftell(output.get());

  constexpr std::string_view expected = "\x1b[1 q\x1b[2 q\x1b[3 q\x1b[4 q\x1b[5 q\x1b[6 q\x1b[0 q";
  std::string actual(expected.size(), '\0');
  bool const positions_valid = begin >= 0 && end - begin == static_cast<long>(expected.size());
  bool const read_succeeded =
      positions_valid && std::fseek(output.get(), begin, SEEK_SET) == 0 && std::fread(actual.data(), 1, actual.size(), output.get()) == actual.size();
  expect(read_succeeded && actual == expected && terminal_context.cursor_settings() == terminal::CursorSettings{terminal::CursorStyle::Default},
         "Context maps cursor styles and blink settings, suppresses duplicates, and retains the applied default");
}

// Verify that Window exposes writable-area geometry for a non-empty margin and avoids creating a subwindow for an empty one.
//
// Runs ncurses against temporary files and repeatedly destroys margin-aware windows so parent/child handle ordering is exercised
// without writing to a real terminal.
void test_margin_aware_window_geometry_and_lifetime()
{
  ScopedTmpFile input;
  ScopedTmpFile output;

  ScopedEnvVar term_guard("TERM", "xterm-256color");
  terminal::Context terminal_context(output.get(), input.get());

  // A visibility change can reset DECSCUSR state without changing the retained settings. Verify that Context's repair path
  // emits the retained sequence on every call instead of letting CursorState's ordinary duplicate suppression hide it.
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Bar, false});
  long const reapply_begin = std::ftell(output.get());
  terminal_context.reapply_cursor_settings();
  terminal_context.reapply_cursor_settings();
  long const reapply_end = std::ftell(output.get());
  char reapplied_sequences[10]{};
  bool const positions_valid = reapply_begin >= 0 && reapply_end >= reapply_begin;
  bool const seek_succeeded = positions_valid && std::fseek(output.get(), reapply_begin, SEEK_SET) == 0;
  std::size_t const bytes_read = seek_succeeded ? std::fread(reapplied_sequences, 1, sizeof(reapplied_sequences), output.get()) : 0;
  expect(positions_valid && reapply_end - reapply_begin == static_cast<long>(sizeof(reapplied_sequences)) && bytes_read == sizeof(reapplied_sequences) &&
             std::string_view{reapplied_sequences, sizeof(reapplied_sequences)} == "\x1b[6 q\x1b[6 q",
         "reapplying cursor settings must always emit the retained shape and blink sequence");
  static_cast<void>(std::fseek(output.get(), 0, SEEK_END));

  terminal::Rendition const background_rendition{{}};

  for (int iteration = 0; iteration != 3; ++iteration)
  {
    terminal::Margin const margin{.top = 1, .bottom = 2, .left = 3, .right = 1};
    terminal::Window window({8, 12}, {2, 4}, background_rendition, {margin, terminal::ColorPair{}});

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
    terminal::Window window({4, 7}, {1, 2}, background_rendition);
    expect(!window.is_subwin(), "an empty-margin Window must not create an ncurses subwindow");
    expect(&window.outer_window() == static_cast<terminal::BasicWindow*>(&window),
           "an empty-margin Window must use one BasicWindow wrapper for inner and outer access");
  }

  terminal_context.apply_cursor_settings({terminal::CursorStyle::Default});
}

} // namespace

void run_terminal_window_tests()
{
  test_context_cursor_settings();
  test_margin_aware_window_geometry_and_lifetime();
}
