#include "sys.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/runtime_input_internal.h"

#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <curses.h>

namespace ava::tui::runtime_input {
using Signals = core::Signals;

namespace {

constexpr std::size_t kMaxBracketedPasteBytes = 1024 * 1024;
constexpr std::size_t kMaxEscapeSequenceBytes = 16 * 1024;
constexpr std::size_t kStartupInputQueueCap = 64;

std::deque<RuntimeInput>& startup_input_queue_storage()
{
  static std::deque<RuntimeInput> queue;
  return queue;
}

RuntimeInput key_input(Key key)
{
  return RuntimeInput{
      .event = InputEvent{.key = key, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0}, .text = {}, .bracketed_paste = false, .resize = false};
}

RuntimeInput event_input(InputEvent event)
{
  return RuntimeInput{.event = std::move(event), .text = {}, .bracketed_paste = false, .resize = false};
}

RuntimeInput unknown_input()
{
  return key_input(Key::Unknown);
}

bool curses_key_name_equals(int value, std::string_view expected)
{
  char const* name = keyname(value);
  return name != nullptr && std::string_view(name) == expected;
}

std::optional<std::string> encode_wide_character(wchar_t character)
{
  std::mbstate_t state{};
  char buffer[MB_LEN_MAX]{};
  auto const length = std::wcrtomb(buffer, character, &state);
  if (length == static_cast<std::size_t>(-1))
    return std::nullopt;
  return std::string(buffer, length);
}

RuntimeInput character_input(std::string text, bool bracketed_paste = false)
{
  auto const first_byte = text.empty() ? '\0' : text[0];
  auto event_text = text;
  return RuntimeInput{.event = InputEvent{.key = Key::Character, .character = first_byte, .text = std::move(event_text), .mouse_column = 0, .mouse_row = 0},
                      .text = std::move(text),
                      .bracketed_paste = bracketed_paste,
                      .resize = false};
}

RuntimeInput space_input()
{
  return RuntimeInput{.event = InputEvent{.key = Key::Space, .character = ' ', .text = " ", .mouse_column = 0, .mouse_row = 0},
                      .text = " ",
                      .bracketed_paste = false,
                      .resize = false};
}

std::optional<wchar_t> read_plain_wide_character()
{
  wint_t value = 0;
  auto const result = wget_wch(stdscr, &value);
  if (result == ERR || result == KEY_CODE_YES)
    return std::nullopt;
  return static_cast<wchar_t>(value);
}

// Read the body of an escape/control sequence after ESC was already consumed.
// Bounded by kMaxEscapeSequenceBytes; stops on complete sequence, timeout, or
// non-backspace KEY_CODE. Caller owns the surrounding wtimeout budget.
std::string read_escape_sequence_body()
{
  std::string consumed;
  consumed.reserve(32);
  while (consumed.size() < kMaxEscapeSequenceBytes)
  {
    wint_t value = 0;
    auto const result = wget_wch(stdscr, &value);
    if (result == ERR)
      break;
    if (result == KEY_CODE_YES)
    {
      if (static_cast<int>(value) == KEY_BACKSPACE)
      {
        consumed.push_back('\x7f');
      }
      else
      {
        break;
      }
    }
    else if (auto encoded = encode_wide_character(static_cast<wchar_t>(value)))
    {
      consumed += *encoded;
    }
    if (terminal_escape_sequence_complete(consumed))
      break;
  }
  return consumed;
}

RuntimeInput read_bracketed_paste()
{
  std::string pasted;
  static_cast<void>(wtimeout(stdscr, 1000));
  while (!Signals::signal_received(terminal_signals) && pasted.size() < kMaxBracketedPasteBytes)
  {
    auto const character = read_plain_wide_character();
    if (!character)
      break;
    if (*character == L'\x1b')
    {
      // Protocol ownership: assemble a complete bounded escape/control sequence
      // after ESC. Paste-end ends the paste; an armed OSC 11 reply is handled and
      // discarded without joining the paste payload; all other escape content is
      // preserved under ordinary paste normalization and the byte cap.
      auto const consumed = read_escape_sequence_body();
      if (consumed == "[201~")
        break;
      if (terminal_background_response_handle(consumed))
        continue;
      pasted.push_back('\x1b');
      if (pasted.size() + consumed.size() > kMaxBracketedPasteBytes)
        break;
      pasted += consumed;
      continue;
    }
    if (auto encoded = encode_wide_character(*character))
    {
      if (pasted.size() + encoded->size() > kMaxBracketedPasteBytes)
      {
        break;
      }
      pasted += *encoded;
    }
  }
  static_cast<void>(wtimeout(stdscr, -1));
  return character_input(normalize_composer_paste_text(pasted), true);
}

std::optional<RuntimeInput> read_escape_sequence_input()
{
  static_cast<void>(wtimeout(stdscr, 50));
  auto const consumed = read_escape_sequence_body();
  static_cast<void>(wtimeout(stdscr, -1));

  if (consumed.empty())
    return std::nullopt;
  if (terminal_keyboard_protocol_handle_response(consumed))
    return unknown_input();
  if (terminal_background_response_handle(consumed))
    return unknown_input();
  if (consumed == "[200~")
    return read_bracketed_paste();
  auto event = terminal_escape_sequence_event(consumed);
  if (event.key != Key::Unknown)
    return event_input(std::move(event));
  if (terminal_escape_sequence_should_discard(consumed) || !terminal_escape_sequence_complete(consumed))
  {
    return unknown_input();
  }
  return unknown_input();
}

std::optional<RuntimeInput> take_startup_input()
{
  auto& queue = startup_input_queue_storage();
  if (queue.empty())
    return std::nullopt;
  auto input = std::move(queue.front());
  queue.pop_front();
  return input;
}

}  // namespace

std::optional<std::string> printable_jump_target(RuntimeInput const& input)
{
  if (input.bracketed_paste)
    return std::nullopt;
  if (input.event.key == Key::Space)
    return std::string(" ");
  if (input.event.key != Key::Character)
    return std::nullopt;

  std::string text = input.text.empty() ? std::string(1, input.event.character) : input.text;
  if (text.empty())
    return std::nullopt;

  auto const first = static_cast<unsigned char>(text.front());
  if (first < 0x20U || first == 0x7FU)
    return std::nullopt;
  if ((first & 0x80U) == 0)
    return text.substr(0, 1);

  auto length = std::size_t{0};
  if (first >= 0xC2U && first <= 0xDFU)
    length = 2;
  else if ((first & 0xF0U) == 0xE0U)
    length = 3;
  else if (first >= 0xF0U && first <= 0xF4U)
    length = 4;
  if (length == 0 || text.size() < length)
    return std::nullopt;
  for (std::size_t index = 1; index < length; ++index)
  {
    if ((static_cast<unsigned char>(text[index]) & 0xC0U) != 0x80U)
      return std::nullopt;
  }
  return text.substr(0, length);
}

void clear_startup_input_queue()
{
  startup_input_queue_storage().clear();
}

std::size_t startup_input_queue_size()
{
  return startup_input_queue_storage().size();
}

bool enqueue_startup_input(RuntimeInput input)
{
  auto& queue = startup_input_queue_storage();
  if (queue.size() >= kStartupInputQueueCap)
    return false;
  queue.push_back(std::move(input));
  return true;
}

RuntimeInput read_curses_input_from_terminal()
{
  wint_t value = 0;
  auto const result = wget_wch(stdscr, &value);
  if (Signals::signal_received(terminal_signals))
    return key_input(Key::CtrlC);
  if (result == ERR)
    return unknown_input();

  if (result == KEY_CODE_YES)
  {
    if (curses_key_name_equals(static_cast<int>(value), "kDC2"))
    {
      return key_input(Key::ShiftDelete);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kDC3"))
    {
      return key_input(Key::AltDelete);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kLFT6"))
    {
      return key_input(Key::ShiftCtrlArrowLeft);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kRIT6"))
    {
      return key_input(Key::ShiftCtrlArrowRight);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kLFT4"))
    {
      return key_input(Key::ShiftAltArrowLeft);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kRIT4"))
    {
      return key_input(Key::ShiftAltArrowRight);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kLFT3"))
    {
      return key_input(Key::AltArrowLeft);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kRIT3"))
    {
      return key_input(Key::AltArrowRight);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kUP3"))
    {
      return key_input(Key::AltArrowUp);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kDN3"))
    {
      return key_input(Key::AltArrowDown);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kUP2"))
    {
      return key_input(Key::ShiftArrowUp);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kDN2"))
    {
      return key_input(Key::ShiftArrowDown);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kri"))
    {
      return key_input(Key::ShiftArrowUp);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kind"))
    {
      return key_input(Key::ShiftArrowDown);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kLFT2"))
    {
      return key_input(Key::ShiftArrowLeft);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kRIT2"))
    {
      return key_input(Key::ShiftArrowRight);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kLFT5"))
    {
      return key_input(Key::CtrlArrowLeft);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kRIT5"))
    {
      return key_input(Key::CtrlArrowRight);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kHOM6"))
    {
      return key_input(Key::ShiftCtrlHome);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kEND6"))
    {
      return key_input(Key::ShiftCtrlEnd);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kHOM5"))
    {
      return key_input(Key::CtrlHome);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kEND5"))
    {
      return key_input(Key::CtrlEnd);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kHOM2"))
    {
      return key_input(Key::ShiftHome);
    }
    if (curses_key_name_equals(static_cast<int>(value), "kEND2"))
    {
      return key_input(Key::ShiftEnd);
    }
    switch (static_cast<int>(value))
    {
      case KEY_ENTER:
        return key_input(Key::Enter);
      case KEY_BACKSPACE:
        return key_input(Key::Backspace);
#ifdef KEY_BTAB
      case KEY_BTAB:
        return key_input(Key::ShiftTab);
#endif
#if defined(KEY_SDC) && (!defined(KEY_DC) || KEY_SDC != KEY_DC)
      case KEY_SDC:
        return key_input(Key::ShiftDelete);
#endif
#ifdef KEY_DC
      case KEY_DC:
        return key_input(Key::Delete);
#endif
#ifdef KEY_IC
      case KEY_IC:
        return key_input(Key::Insert);
#endif
#ifdef KEY_CLEAR
      case KEY_CLEAR:
        return key_input(Key::Clear);
#endif
      case KEY_UP:
        return key_input(Key::ArrowUp);
      case KEY_DOWN:
        return key_input(Key::ArrowDown);
      case KEY_LEFT:
        return key_input(Key::ArrowLeft);
      case KEY_RIGHT:
        return key_input(Key::ArrowRight);
#ifdef KEY_SLEFT
      case KEY_SLEFT:
        return key_input(Key::ShiftArrowLeft);
#endif
#ifdef KEY_SRIGHT
      case KEY_SRIGHT:
        return key_input(Key::ShiftArrowRight);
#endif
#ifdef KEY_SUP
      case KEY_SUP:
        return key_input(Key::ShiftArrowUp);
#endif
#ifdef KEY_SDOWN
      case KEY_SDOWN:
        return key_input(Key::ShiftArrowDown);
#endif
#ifdef KEY_SR
      case KEY_SR:
        return key_input(Key::ShiftArrowUp);
#endif
#ifdef KEY_SF
      case KEY_SF:
        return key_input(Key::ShiftArrowDown);
#endif
#ifdef KEY_SHOME
      case KEY_SHOME:
        return key_input(Key::ShiftHome);
#endif
#ifdef KEY_SEND
      case KEY_SEND:
        return key_input(Key::ShiftEnd);
#endif
      case KEY_PPAGE:
        return key_input(Key::PageUp);
      case KEY_NPAGE:
        return key_input(Key::PageDown);
#ifdef KEY_HOME
      case KEY_HOME:
        return key_input(Key::Home);
#endif
#ifdef KEY_END
      case KEY_END:
        return key_input(Key::End);
#endif
#ifdef KEY_F
      case KEY_F(1):
        return key_input(Key::F1);
      case KEY_F(2):
        return key_input(Key::F2);
      case KEY_F(3):
        return key_input(Key::F3);
      case KEY_F(4):
        return key_input(Key::F4);
      case KEY_F(5):
        return key_input(Key::F5);
      case KEY_F(6):
        return key_input(Key::F6);
      case KEY_F(7):
        return key_input(Key::F7);
      case KEY_F(8):
        return key_input(Key::F8);
      case KEY_F(9):
        return key_input(Key::F9);
      case KEY_F(10):
        return key_input(Key::F10);
      case KEY_F(11):
        return key_input(Key::F11);
      case KEY_F(12):
        return key_input(Key::F12);
#endif
#ifdef KEY_RESIZE
      case KEY_RESIZE:
        return RuntimeInput{.event = InputEvent{.key = Key::Unknown, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0},
                            .text = {},
                            .bracketed_paste = false,
                            .resize = true};
#endif
#ifdef KEY_MOUSE
      case KEY_MOUSE: {
        MEVENT mouse{};
        if (getmouse(&mouse) != OK)
          return unknown_input();
        return event_input(terminal_ncurses_mouse_event(static_cast<std::uint64_t>(mouse.bstate), static_cast<std::size_t>(mouse.x + 1),
                                                        static_cast<std::size_t>(mouse.y + 1)));
      }
#endif
      default:
        return unknown_input();
    }
  }

  auto const character = static_cast<wchar_t>(value);
  if (character == L'\r')
    return key_input(Key::Enter);
  if (character == L'\n')
    return key_input(Key::ShiftEnter);
  if (character == L'\t')
    return key_input(Key::Tab);
  if (character == L' ')
    return space_input();
  if (character == 0x00)
    return key_input(Key::CtrlSpace);
  if (character == 0x1B)
  {
    if (auto escape_input = read_escape_sequence_input())
      return *escape_input;
    return key_input(Key::Escape);
  }
  if (character == 0x01)
    return key_input(Key::CtrlA);
  if (character == 0x02)
    return key_input(Key::CtrlB);
  if (character == 0x03)
    return key_input(Key::CtrlC);
  if (character == 0x04)
    return key_input(Key::CtrlD);
  if (character == 0x05)
    return key_input(Key::CtrlE);
  if (character == 0x06)
    return key_input(Key::CtrlF);
  if (character == 0x07)
    return key_input(Key::CtrlG);
  if (character == 0x08)
    return key_input(Key::CtrlH);
  if (character == 0x0B)
    return key_input(Key::CtrlK);
  if (character == 0x0C)
    return key_input(Key::CtrlL);
  if (character == 0x1F)
    return key_input(Key::CtrlMinus);
  if (character == 0x0E)
    return key_input(Key::CtrlN);
  if (character == 0x0F)
    return key_input(Key::CtrlO);
  if (character == 0x10)
    return key_input(Key::CtrlP);
  if (character == 0x12)
    return key_input(Key::CtrlR);
  if (character == 0x13)
    return key_input(Key::CtrlS);
  if (character == 0x14)
    return key_input(Key::CtrlT);
  if (character == 0x15)
    return key_input(Key::CtrlU);
  if (character == 0x16)
    return key_input(Key::CtrlV);
  if (character == 0x17)
    return key_input(Key::CtrlW);
  if (character == 0x18)
    return key_input(Key::CtrlX);
  if (character == 0x19)
    return key_input(Key::CtrlY);
  if (character == 0x1A)
    return key_input(Key::CtrlZ);
  if (character == 0x1D)
    return key_input(Key::CtrlRightBracket);
  if (character == 0x7F)
    return key_input(Key::Backspace);
  if (character >= 0x20)
  {
    auto encoded = encode_wide_character(character);
    if (!encoded)
      return unknown_input();
    return character_input(std::move(*encoded));
  }
  return unknown_input();
}

RuntimeInput read_curses_input()
{
  if (auto queued = take_startup_input())
    return *queued;
  return read_curses_input_from_terminal();
}

bool empty_curses_input(RuntimeInput const& input)
{
  return !input.resize && input.event.key == Key::Unknown && input.text.empty() && !input.bracketed_paste;
}

std::optional<RuntimeInput> poll_curses_input()
{
  if (auto queued = take_startup_input())
    return queued;
  static_cast<void>(wtimeout(stdscr, 0));
  auto input = read_curses_input_from_terminal();
  static_cast<void>(wtimeout(stdscr, -1));
  if (empty_curses_input(input))
  {
    return std::nullopt;
  }
  return input;
}

std::optional<RuntimeInput> read_curses_input_with_timeout(std::chrono::milliseconds timeout)
{
  if (auto queued = take_startup_input())
    return queued;
  static_cast<void>(wtimeout(stdscr, static_cast<int>(timeout.count())));
  auto input = read_curses_input_from_terminal();
  static_cast<void>(wtimeout(stdscr, -1));
  if (empty_curses_input(input))
    return std::nullopt;
  return input;
}

void drain_startup_probe_input(std::chrono::milliseconds deadline)
{
  using clock = std::chrono::steady_clock;
  auto const started = clock::now();
  while (startup_input_queue_storage().size() < kStartupInputQueueCap)
  {
    auto const elapsed = clock::now() - started;
    if (elapsed >= deadline)
      break;
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - elapsed);
    if (remaining.count() <= 0)
      break;

    static_cast<void>(wtimeout(stdscr, static_cast<int>(remaining.count())));
    auto input = read_curses_input_from_terminal();
    static_cast<void>(wtimeout(stdscr, -1));
    if (empty_curses_input(input))
      continue;
    if (!enqueue_startup_input(std::move(input)))
      break;
  }
}

}  // namespace ava::tui::runtime_input
