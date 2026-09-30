#include "sys.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/runtime_input_internal.h"
#include "terminal/Context.h"
#include "ava/core/Application.h"

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

// Return ncurses-compatible status while reading through the canonical Context first.
// `value` is a required non-null output parameter, matching wget_wch's caller contract.
int read_terminal_wch(terminal::Context& terminal_context, wint_t* value_ptr)
{
  // Canonical Context replay has first authority over bytes consumed by bounded startup negotiation. It returns logical wide characters
  // with ncurses' ordinary OK status; only after that queue is empty may wget_wch read the terminal and report OK, KEY_CODE_YES, or ERR.
  if (terminal_context.try_get_buffered_keyboard_input(value_ptr))
    return OK;
  return wget_wch(stdscr, value_ptr);
}

std::optional<wchar_t> read_plain_wide_character(terminal::Context& terminal_context)
{
  wint_t value = 0;
  auto const result = read_terminal_wch(terminal_context, &value);
  if (result == ERR || result == KEY_CODE_YES)
    return std::nullopt;
  return static_cast<wchar_t>(value);
}

// Read the body of an escape/control sequence after ESC was already consumed.
// Bounded by kMaxEscapeSequenceBytes; stops on complete sequence, timeout, or
// non-backspace KEY_CODE. Caller owns the surrounding wtimeout budget.
std::string read_escape_sequence_body(terminal::Context& terminal_context)
{
  std::string consumed;
  consumed.reserve(32);
  while (consumed.size() < kMaxEscapeSequenceBytes)
  {
    wint_t value = 0;
    auto const result = read_terminal_wch(terminal_context, &value);
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

RuntimeInput read_bracketed_paste(terminal::Context& terminal_context)
{
  std::string pasted;
  static_cast<void>(wtimeout(stdscr, 1000));
  while (!Signals::received(terminal_signals) && pasted.size() < kMaxBracketedPasteBytes)
  {
    auto const character = read_plain_wide_character(terminal_context);
    if (!character)
      break;
    if (*character == L'\x1b')
    {
      // Protocol ownership: assemble a complete bounded escape/control sequence
      // after ESC. Paste-end ends the paste; an armed OSC 11 reply is handled and
      // discarded without joining the paste payload; all other escape content is
      // preserved under ordinary paste normalization and the byte cap.
      auto const consumed = read_escape_sequence_body(terminal_context);
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

std::optional<RuntimeInput> read_escape_sequence_input(terminal::Context& terminal_context)
{
  static_cast<void>(wtimeout(stdscr, 50));
  auto const consumed = read_escape_sequence_body(terminal_context);
  static_cast<void>(wtimeout(stdscr, -1));

  if (consumed.empty())
    return std::nullopt;
  if (terminal_background_response_handle(consumed))
    return unknown_input();
  if (consumed == "[200~")
    return read_bracketed_paste(terminal_context);
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
  return read_curses_input_from_terminal(core::Application::instance().terminal_context());
}

struct Entry
{
  std::string_view name;
  Key key;
};

Key read_curses_key_from_terminal(terminal::Context& terminal_context, wint_t& value)
{
  int const result = read_terminal_wch(terminal_context, &value);
  if (result == ERR)
    return Key::Unknown;
  if (result == KEY_CODE_YES)
  {
    static constexpr std::array<Entry, 35> table = {{
      {}, {}, {},
      {"kri", Key::ShiftArrowUp},
      {"kind", Key::ShiftArrowDown},
      {"kEND6", Key::ShiftCtrlEnd},
      {}, {},
      {"kEND5", Key::CtrlEnd},
      {"kHOM6", Key::ShiftCtrlHome},
      {"kRIT6", Key::ShiftCtrlArrowRight},
      {"kRIT4", Key::ShiftAltArrowRight},
      {"kHOM5", Key::CtrlHome},
      {"kRIT5", Key::CtrlArrowRight},
      {},
      {"kLFT6", Key::ShiftCtrlArrowLeft},
      {"kLFT4", Key::ShiftAltArrowLeft},
      {"kDN3", Key::AltArrowDown},
      {"kLFT5", Key::CtrlArrowLeft},
      {"kDN2", Key::ShiftArrowDown},
      {"kEND2", Key::ShiftEnd},
      {}, {},
      {"kRIT3", Key::AltArrowRight},
      {"kHOM2", Key::ShiftHome},
      {"kRIT2", Key::ShiftArrowRight},
      {},
      {"kUP3", Key::AltArrowUp},
      {"kLFT3", Key::AltArrowLeft},
      {"kUP2", Key::ShiftArrowUp},
      {"kLFT2", Key::ShiftArrowLeft},
      {},
      {"kDC3", Key::AltDelete},
      {},
      {"kDC2", Key::ShiftDelete},
    }};

    std::string_view const keyname = terminal_context.keyname(value);

    if (3 <= keyname.size() && keyname.size() <= 5)
    {
      static std::array<unsigned char, 256> asso_values = {
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        15, 13,  1,  3,  0, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 15, 35, 35,
        10, 35, 35,  5, 35, 35, 35, 35,  0,  4,
        10, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
         0, 35, 35, 35, 35,  0, 35, 35, 35, 35,
         0, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35
      };

      unsigned int const key =
        keyname.size() + asso_values[static_cast<unsigned char>(keyname[2])] + asso_values[static_cast<unsigned char>(keyname.back())];

      if (key < table.size())
      {
        Entry const& entry = table[key];
        if (keyname == entry.name)
          return entry.key;
      }
    }

    switch (static_cast<int>(value))
    {
      case KEY_ENTER:
        return Key::Enter;
      case KEY_BACKSPACE:
        return Key::Backspace;
#ifdef KEY_BTAB
      case KEY_BTAB:
        return Key::ShiftTab;
#endif
#if defined(KEY_SDC) && (!defined(KEY_DC) || KEY_SDC != KEY_DC)
      case KEY_SDC:
        return Key::ShiftDelete;
#endif
#ifdef KEY_DC
      case KEY_DC:
        return Key::Delete;
#endif
#ifdef KEY_IC
      case KEY_IC:
        return Key::Insert;
#endif
#ifdef KEY_CLEAR
      case KEY_CLEAR:
        return Key::Clear;
#endif
      case KEY_UP:
        return Key::ArrowUp;
      case KEY_DOWN:
        return Key::ArrowDown;
      case KEY_LEFT:
        return Key::ArrowLeft;
      case KEY_RIGHT:
        return Key::ArrowRight;
#ifdef KEY_SLEFT
      case KEY_SLEFT:
        return Key::ShiftArrowLeft;
#endif
#ifdef KEY_SRIGHT
      case KEY_SRIGHT:
        return Key::ShiftArrowRight;
#endif
#ifdef KEY_SUP
      case KEY_SUP:
        return Key::ShiftArrowUp;
#endif
#ifdef KEY_SDOWN
      case KEY_SDOWN:
        return Key::ShiftArrowDown;
#endif
#ifdef KEY_SR
      case KEY_SR:
        return Key::ShiftArrowUp;
#endif
#ifdef KEY_SF
      case KEY_SF:
        return Key::ShiftArrowDown;
#endif
#ifdef KEY_SHOME
      case KEY_SHOME:
        return Key::ShiftHome;
#endif
#ifdef KEY_SEND
      case KEY_SEND:
        return Key::ShiftEnd;
#endif
      case KEY_PPAGE:
        return Key::PageUp;
      case KEY_NPAGE:
        return Key::PageDown;
#ifdef KEY_HOME
      case KEY_HOME:
        return Key::Home;
#endif
#ifdef KEY_END
      case KEY_END:
        return Key::End;
#endif
#ifdef KEY_F
      case KEY_F(1):
        return Key::F1;
      case KEY_F(2):
        return Key::F2;
      case KEY_F(3):
        return Key::F3;
      case KEY_F(4):
        return Key::F4;
      case KEY_F(5):
        return Key::F5;
      case KEY_F(6):
        return Key::F6;
      case KEY_F(7):
        return Key::F7;
      case KEY_F(8):
        return Key::F8;
      case KEY_F(9):
        return Key::F9;
      case KEY_F(10):
        return Key::F10;
      case KEY_F(11):
        return Key::F11;
      case KEY_F(12):
        return Key::F12;
#endif
#ifdef KEY_RESIZE
      case KEY_RESIZE:
        return Key::Resize;
#endif
#ifdef KEY_MOUSE
      case KEY_MOUSE:
        return Key::Mouse;
#endif
      default:
        return Key::Unknown;
    }
  }

  wchar_t const character = static_cast<wchar_t>(value);
  if (character == L'\r')
    return Key::Enter;
  if (character == L'\n')
    return Key::ShiftEnter;
  if (character == L'\t')
    return Key::Tab;
  if (character == L' ')
    return Key::Space;
  if (character == 0x00)
    return Key::CtrlSpace;
  if (character == 0x1B)
    return Key::Escape;
  if (character == 0x01)
    return Key::CtrlA;
  if (character == 0x02)
    return Key::CtrlB;
  if (character == 0x03)
    return Key::CtrlC;
  if (character == 0x04)
    return Key::CtrlD;
  if (character == 0x05)
    return Key::CtrlE;
  if (character == 0x06)
    return Key::CtrlF;
  if (character == 0x07)
    return Key::CtrlG;
  if (character == 0x08)
    return Key::CtrlH;
  if (character == 0x0B)
    return Key::CtrlK;
  if (character == 0x0C)
    return Key::CtrlL;
  if (character == 0x1F)
    return Key::CtrlMinus;
  if (character == 0x0E)
    return Key::CtrlN;
  if (character == 0x0F)
    return Key::CtrlO;
  if (character == 0x10)
    return Key::CtrlP;
  if (character == 0x11)
    return Key::CtrlQ;
  if (character == 0x12)
    return Key::CtrlR;
  if (character == 0x13)
    return Key::CtrlS;
  if (character == 0x14)
    return Key::CtrlT;
  if (character == 0x15)
    return Key::CtrlU;
  if (character == 0x16)
    return Key::CtrlV;
  if (character == 0x17)
    return Key::CtrlW;
  if (character == 0x18)
    return Key::CtrlX;
  if (character == 0x19)
    return Key::CtrlY;
  if (character == 0x1A)
    return Key::CtrlZ;
  if (character == 0x1C)
    return Key::Unknown;                // Not handled.
  if (character == 0x1D)
    return Key::CtrlRightBracket;
  if (character == 0x1E)
    return Key::Unknown;                // Not handled.
  if (character == 0x7F)
    return Key::Backspace;
  // Paranoia check: all control characters should be handled.
  ASSERT(character >= 0x20);
  return Key::WideCharacter;
}

RuntimeInput read_curses_input_from_terminal(terminal::Context& terminal_context)
{
  wint_t value = 0;
  Key key = read_curses_key_from_terminal(terminal_context, value);
  if (Signals::received(terminal_signals))
    return key_input(Key::CtrlC);
  if (key == Key::WideCharacter)
  {
    wchar_t const character = static_cast<wchar_t>(value);
    auto encoded = encode_wide_character(character);
    if (encoded)
      return character_input(std::move(*encoded));
    return unknown_input();
  }
  if (key == Key::Escape)
  {
    if (auto escape_input = read_escape_sequence_input(terminal_context))
      return *escape_input;
    return key_input(Key::Escape);
  }
  if (key == Key::Space)
    return space_input();
#ifdef KEY_RESIZE
  if (key == Key::Resize)
  {
    return RuntimeInput{.event = InputEvent{.key = Key::Unknown, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0},
                        .text = {},
                        .bracketed_paste = false,
                        .resize = true};
  }
#endif
#ifdef KEY_MOUSE
  if (key == Key::Mouse)
  {
    MEVENT mouse{};
    if (getmouse(&mouse) != OK)
      return unknown_input();
    return event_input(terminal_ncurses_mouse_event(static_cast<std::uint64_t>(mouse.bstate), static_cast<std::size_t>(mouse.x + 1),
                                                    static_cast<std::size_t>(mouse.y + 1)));
  }
#endif

  return key_input(key);
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
