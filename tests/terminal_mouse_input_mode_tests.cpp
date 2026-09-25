#include "sys.h"
#include "support/test_harness.h"
#include "terminal/Context.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace terminal = ava::tui::terminal;

namespace {

constexpr std::string_view kMouseEnableSequence = "\x1b[?1003l\x1b[?1000h\x1b[?1002h\x1b[?1006h";
constexpr std::string_view kMouseDisableSequence = "\x1b[?1003l\x1b[?1006l\x1b[?1002l\x1b[?1000l";
constexpr std::string_view kBracketedPasteEnableSequence = "\x1b[?2004h";
constexpr std::string_view kBracketedPasteDisableSequence = "\x1b[?2004l";

// Read every emitted byte from `file` after flushing and rewinding it.
std::string read_output(FILE* file)
{
  expect(std::fflush(file) == 0, "terminal mouse-mode output must flush before inspection");
  std::rewind(file);
  std::string result;
  std::array<char, 1024> bytes;
  while (std::size_t const count = std::fread(bytes.data(), 1, bytes.size(), file))
    result.append(bytes.data(), count);
  return result;
}

// Return the number of non-overlapping occurrences of `sequence` in captured terminal output.
std::size_t count_occurrences(std::string_view bytes, std::string_view sequence)
{
  std::size_t count = 0;
  for (std::size_t position = 0; (position = bytes.find(sequence, position)) != std::string_view::npos; position += sequence.size())
    ++count;
  return count;
}

// Verify that Context owns one balanced mouse-reporting and bracketed-paste lifecycle without probing for support.
void test_context_balances_mouse_input_modes()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  {
    terminal::Context context(output.get(), input.get());
  }

  std::string const emitted = read_output(output.get());
  std::size_t const mouse_enable = emitted.find(kMouseEnableSequence);
  std::size_t const paste_enable = emitted.find(kBracketedPasteEnableSequence);
  std::size_t const paste_disable = emitted.find(kBracketedPasteDisableSequence);
  std::size_t const mouse_disable = emitted.find(kMouseDisableSequence);
  expect(mouse_enable != std::string::npos && paste_enable != std::string::npos && paste_disable != std::string::npos && mouse_disable != std::string::npos,
         "Context must emit complete mouse and bracketed-paste activation and restoration sequences");
  expect(mouse_enable < paste_enable && paste_enable < paste_disable && paste_disable < mouse_disable,
         "Context must restore mouse and bracketed-paste modes in reverse activation order");
  expect(count_occurrences(emitted, kMouseEnableSequence) == 1 && count_occurrences(emitted, kBracketedPasteEnableSequence) == 1 &&
             count_occurrences(emitted, kBracketedPasteDisableSequence) == 1 && count_occurrences(emitted, kMouseDisableSequence) == 1,
         "Context must activate and restore each mouse input mode exactly once");
}

// Verify that Context alone releases and rearms mouse, paste, keyboard, and retained cursor modes across a temporary terminal handoff.
void test_context_balances_handoff_modes()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  {
    terminal::Context context(output.get(), input.get());
    context.apply_cursor_settings({terminal::CursorStyle::Bar, false});
    context.release_input_modes_for_handoff();
    context.rearm_input_modes_after_handoff();
  }

  std::string const emitted = read_output(output.get());
  expect(count_occurrences(emitted, kMouseEnableSequence) == 2 && count_occurrences(emitted, kMouseDisableSequence) == 2 &&
             count_occurrences(emitted, kBracketedPasteEnableSequence) == 2 && count_occurrences(emitted, kBracketedPasteDisableSequence) == 2,
         "Context balances mouse and bracketed-paste modes across handoff, rearm, and final destruction");
  expect(count_occurrences(emitted, "\x1b[6 q") == 2 && count_occurrences(emitted, "\x1b[0 q") == 2,
         "Context releases a forced cursor for handoff, reapplies it on resume, and resets it during destruction");
}

} // namespace

// Run deterministic terminal mouse-mode lifecycle tests with a direct-color TERM to avoid palette probing.
void run_terminal_mouse_input_mode_tests()
{
  ScopedEnvVar term_guard("TERM", "xterm-direct");
  test_context_balances_mouse_input_modes();
  test_context_balances_handoff_modes();
}
