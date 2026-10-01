#include "sys.h"
#include "composer_editor.h"
#include "runtime_input_internal.h"
#include "encode_wide_character.h"
#include "terminal/Context.h"
#include "ava/core/Application.h"

#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <string_view>
#include <utility>

namespace ava::tui::runtime_input {
using Signals = core::Signals;
using Key = terminal::Key;

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

// Read the body of an escape/control sequence after ESC was already consumed.
// Bounded by kMaxEscapeSequenceBytes; stops on complete sequence, timeout, or
// non-backspace KEY_CODE. Caller owns the surrounding wtimeout budget.
std::string read_escape_sequence_body(terminal::Context& terminal_context)
{
  std::string consumed;
  consumed.reserve(32);
  while (consumed.size() < kMaxEscapeSequenceBytes)
  {
    if (!terminal_context.append_escape_sequence_character(consumed))
      break;
    if (terminal_escape_sequence_complete(consumed))
      break;
  }
  return consumed;
}

RuntimeInput read_bracketed_paste(terminal::Context& terminal_context)
{
  std::string pasted;
  {
    auto scoped_timeout = terminal_context.timeout(std::chrono::milliseconds(1000));
    while (!Signals::received(terminal_signals) && pasted.size() < kMaxBracketedPasteBytes)
    {
      auto const character = terminal_context.read_plain_wide_character();
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
          break;
        pasted += *encoded;
      }
    }
  }
  return character_input(normalize_composer_paste_text(pasted), true);
}

std::optional<RuntimeInput> read_escape_sequence_input(terminal::Context& terminal_context)
{
  std::string consumed;
  {
    auto scoped_timeout = terminal_context.timeout(std::chrono::milliseconds(config::default_terminal_escape_delay_ms));
    consumed = read_escape_sequence_body(terminal_context);
  }

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

RuntimeInput read_curses_input_from_terminal(terminal::Context& terminal_context)
{
  wint_t value = 0;
  Key key = terminal_context.read_curses_key(value);
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
  RuntimeInput input;
  {
    auto& terminal_context = core::Application::instance().terminal_context();
    auto scoped_timeout = terminal_context.timeout(std::chrono::milliseconds(0));       // Immediately return if there is no input.
    input = read_curses_input_from_terminal();
  }
  if (empty_curses_input(input))
    return std::nullopt;
  return input;
}

std::optional<RuntimeInput> read_curses_input_with_timeout(std::chrono::milliseconds timeout)
{
  if (auto queued = take_startup_input())
    return queued;
  RuntimeInput input;
  {
    auto& terminal_context = core::Application::instance().terminal_context();
    auto scoped_timeout = terminal_context.timeout(timeout);
    input = read_curses_input_from_terminal();
  }
  if (empty_curses_input(input))
    return std::nullopt;
  return input;
}

void drain_startup_probe_input(std::chrono::milliseconds deadline)
{
  using clock = std::chrono::steady_clock;
  auto& terminal_context = core::Application::instance().terminal_context();
  auto const started = clock::now();
  while (startup_input_queue_storage().size() < kStartupInputQueueCap)
  {
    auto const elapsed = clock::now() - started;
    if (elapsed >= deadline)
      break;
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - elapsed);
    if (remaining.count() <= 0)
      break;

    RuntimeInput input;
    {
      auto scoped_timeout = terminal_context.timeout(remaining);
      input = read_curses_input_from_terminal();
    }
    if (empty_curses_input(input))
      continue;
    if (!enqueue_startup_input(std::move(input)))
      break;
  }
}

}  // namespace ava::tui::runtime_input
