#include "sys.h"
#include "support/terminal_test_support.h"
#include "support/test_harness.h"
#include "terminal/Context.h"
#include "terminal/MouseEvent.h"
#include "ava/core/Application.h"

#include <array>
#include <cstdio>
#include <cstdlib>
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
void test_context_balances_mouse_input_modes(FILE* output)
{
  std::string const emitted = read_output(output);
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
  terminal::Context& context = ava::core::Application::instance().terminal_context();
  context.apply_cursor_settings({terminal::CursorStyle::Bar, false});
  context.leave_terminal_for_handoff();
  context.restore_terminal_after_handoff();
}

// Decode real SGR mouse bytes through ncurses, checking coordinates, wheels, modifiers, motion, and empty-queue failure.
void test_mouse_event_decoding()
{
  terminal::Context& context = ava::core::Application::instance().terminal_context();
  FILE* input = process_terminal_test_input();
  constexpr std::string_view reports = "\x1b[<28;13;7M\x1b[<0;13;7m\x1b[<64;3;2M\x1b[<65;3;2M\x1b[<35;3;2M";
  reset_output_file(input);
  expect(std::fwrite(reports.data(), 1, reports.size(), input) == reports.size(), "mouse reports must be written completely");
  std::rewind(input);
  constexpr std::array<uint16_t, 5> buttons{1, 1, 4, 5, 0};
  constexpr std::array actions{terminal::MouseButtonEvent::pressed, terminal::MouseButtonEvent::released, terminal::MouseButtonEvent::pressed,
                               terminal::MouseButtonEvent::pressed, terminal::MouseButtonEvent::moved};
  for (std::size_t index = 0; index < buttons.size(); ++index)
  {
    // Ncurses can batch reports; later reads may reach EOF while decoded mouse data remains queued.
    wint_t value = 0;
    auto const key = context.read_curses_key(value);
    if (index == 0)
      expect(key == terminal::Key::Mouse, "ncurses must recognize SGR mouse input");
    auto event = terminal::get_mouse_event();
    expect(event.has_value(), "get_mouse_event must decode the queued report");
    if (!event)
      return;
    expect(event->button() == buttons[index], "mouse button numbers must preserve wheel direction");
    expect(event->position().row() == (index < 2 ? 6U : 1U) && event->position().col() == (index < 2 ? 12U : 2U),
           "mouse coordinates must be zero-based in row, column order");
    expect(event->event() == actions[index], "mouse action must distinguish press, release, and motion");
    expect(static_cast<uint8_t>(event->modifiers()) == (index == 0 ? 7 : 0), "Shift, Alt, and Ctrl modifiers must combine independently");
  }
  expect(!terminal::get_mouse_event(), "an empty mouse queue must return nullopt rather than an invented event");
}

// Verify handoff and final Application teardown emitted balanced protocol transitions.
void verify_context_balances_handoff_modes(FILE* output)
{
  std::string const emitted = read_output(output);
  expect(count_occurrences(emitted, kMouseEnableSequence) == 2 && count_occurrences(emitted, kMouseDisableSequence) == 2 &&
             count_occurrences(emitted, kBracketedPasteEnableSequence) == 2 && count_occurrences(emitted, kBracketedPasteDisableSequence) == 2,
         "Context balances mouse and bracketed-paste modes across handoff, rearm, and final destruction");
  expect(count_occurrences(emitted, "\x1b[6 q") == 2 && count_occurrences(emitted, "\x1b[0 q") == 2,
         "Context releases a forced cursor for handoff, reapplies it on resume, and resets it during destruction");
}

} // namespace

// Configure the direct-color terminal before the Application initializes its Context.
void prepare_terminal_mouse_input_mode_test_case(std::string_view, FILE*, FILE*)
{
  static_cast<void>(setenv("TERM", "xterm-direct", 1));
}

// Exercise the live portion of one isolated mouse lifecycle case.
void run_terminal_mouse_input_mode_test_case(std::string_view test_case)
{
  if (test_case == "context_lifecycle")
  {
    test_mouse_event_decoding();
    return;
  }
  if (test_case == "handoff_lifecycle")
    test_context_balances_handoff_modes();
  else
    expect(false, "unknown terminal mouse input mode test case: " + std::string(test_case));
}

// Check output that is complete only after TestRunnerApplication has destroyed its Context.
void verify_terminal_mouse_input_mode_test_case(std::string_view test_case, FILE* output)
{
  if (test_case == "context_lifecycle")
    test_context_balances_mouse_input_modes(output);
  else if (test_case == "handoff_lifecycle")
    verify_context_balances_handoff_modes(output);
}
