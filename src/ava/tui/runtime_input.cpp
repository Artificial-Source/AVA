// Decodes native/replayed terminal input into owned events and preserves the
// Empty/Discarded distinction required by the session's bounded input stage.
#include "sys.h"
#include "composer_editor.h"
#include "encode_wide_character.h"
#include "runtime_input_internal.h"
#include "terminal/Context.h"
#include "terminal/MouseEvent.h"
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

// Bound retained untrusted paste bytes; the TUI decoder may still spend time waiting
// between characters, so this limits memory rather than whole-operation duration.
constexpr std::size_t kMaxBracketedPasteBytes = 1024 * 1024;
// Limit escape/control accumulation before malformed terminal input grows without bound.
constexpr std::size_t kMaxEscapeSequenceBytes = 16 * 1024;
// Preserve only this many startup events before leaving later input for normal polling.
constexpr std::size_t kStartupInputQueueCap = 64;

// The decoder's session state is shared by startup probing and normal polling.
// Only the registered TUI thread uses it; session entry/exit resets it. It owns
// replay values and a bounded paste-marker cursor, never terminal storage.
class InputReplay final
{
 public:
  // TUI startup: preserve an owned event in arrival order, without exceeding
  // the replay capacity. False leaves a full queue unchanged.
  [[nodiscard]] bool enqueue(RuntimeInput input)
  {
    if (startup_.size() >= kStartupInputQueueCap)
      return false;
    startup_.push_back(std::move(input));
    return true;
  }
  // TUI polling: transfer the oldest saved event before reading newer native
  // input. No value means the caller may continue with the terminal decoder.
  [[nodiscard]] std::optional<RuntimeInput> take_startup()
  {
    if (startup_.empty())
      return std::nullopt;
    auto input = std::move(startup_.front());
    startup_.pop_front();
    return input;
  }
  // TUI startup checks capacity before reading another event it must preserve.
  [[nodiscard]] std::size_t startup_size() const noexcept { return startup_.size(); }
  // TUI prompt cleanup: discard replay without releasing an unfinished paste's
  // ownership of future bytes. Those bytes must never become prompt shortcuts.
  void clear_startup() { startup_.clear(); }
  // TUI session entry/exit only: retire both replay and paste ownership before
  // another terminal session begins decoding. Prompt flushes must not call this.
  void reset_session()
  {
    clear_startup();
    discard_paste_tail_ = false;
    paste_end_prefix_ = 0;
  }
  // TUI decoder after a paste-start marker: begin a new terminator match while
  // retaining ownership if collection later hits its size limit or times out.
  void begin_paste()
  {
    discard_paste_tail_ = true;
    paste_end_prefix_ = 0;
  }
  // TUI decoder: track one literal character, including escape-parser consumption.
  // True means the full marker ended paste ownership; no text is retained here.
  bool observe_paste_character(wchar_t character)
  {
    constexpr std::wstring_view end = L"\x1b[201~";
    paste_end_prefix_ = character == end[paste_end_prefix_] ? paste_end_prefix_ + 1 : (character == end.front() ? 1 : 0);
    if (paste_end_prefix_ != end.size())
      return false;
    paste_end_prefix_ = 0;
    discard_paste_tail_ = false;
    return true;
  }
  // TUI polling: consume one decoded native item while an unfinished paste owns
  // its tail. Special keys break a partial marker; only a literal full marker
  // ends ownership. True charges this discarded item to the normal input budget.
  [[nodiscard]] bool discard_tail(Key key, wint_t value)
  {
    if (!discard_paste_tail_)
      return false;
    if (key == Key::WideCharacter || key == Key::Escape)
      static_cast<void>(observe_paste_character(static_cast<wchar_t>(value)));
    else
      paste_end_prefix_ = 0;
    return true;
  }

  // Internal-linkage decoder state contains user input and cannot be debug-printed.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Startup probing appends at most 64 owned events; ordinary polling removes them.
  std::deque<RuntimeInput> startup_;
  // TUI-only paste ownership, set at its start and cleared by its full closing
  // marker or a terminal-session boundary. Prompt cleanup preserves it.
  bool discard_paste_tail_ = false;
  // TUI decoder's matched prefix of ESC[201~; always below the marker length.
  // Literal characters advance it; special keys, completion and session reset clear it.
  std::size_t paste_end_prefix_ = 0;
};

// TUI thread only: one state owner spans startup and interactive decoding. The
// local static keeps the existing replay lifetime; no worker may access it.
InputReplay& input_replay()
{
  static InputReplay state;
  return state;
}

// TUI decoder: create an owned nontext event from a semantic key; callers use
// character_input/space_input when insertion text must accompany the key.
RuntimeInput key_input(Key key)
{
  return RuntimeInput{
      .event = InputEvent{.key = key, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0}, .text = {}, .bracketed_paste = false, .resize = false};
}

// TUI decoder: transfer already decoded escape/mouse metadata into an owned event
// without inventing composer insertion text or retaining terminal buffers.
RuntimeInput event_input(InputEvent event)
{
  return RuntimeInput{.event = std::move(event), .text = {}, .bracketed_paste = false, .resize = false};
}

// TUI decoder: represent a consumed unsupported/protocol event. Legacy read APIs
// also use this for no input; poll_curses_input preserves native emptiness separately.
RuntimeInput unknown_input()
{
  return key_input(Key::Unknown);
}

// TUI decoder: own UTF-8 text in both the full input and legacy InputEvent view.
// Paste stays one event even when empty; the caller supplies already encoded or
// normalized text and the returned value retains no reference to that argument.
RuntimeInput character_input(std::string text, bool bracketed_paste = false)
{
  auto const first_byte = text.empty() ? '\0' : text[0];
  auto event_text = text;
  return RuntimeInput{.event = InputEvent{.key = Key::Character, .character = first_byte, .text = std::move(event_text), .mouse_column = 0, .mouse_row = 0},
                      .text = std::move(text),
                      .bracketed_paste = bracketed_paste,
                      .resize = false};
}

// TUI decoder: preserve Space as both a bindable key and insertion text so command
// routing and composer insertion can share the same owned event.
RuntimeInput space_input()
{
  return RuntimeInput{.event = InputEvent{.key = Key::Space, .character = ' ', .text = " ", .mouse_column = 0, .mouse_row = 0},
                      .text = " ",
                      .bracketed_paste = false,
                      .resize = false};
}

// TUI thread with a live Context, after consuming ESC: accumulate and encode the
// sequence body up to the byte cap. The caller scopes the per-read timeout; this
// function owns returned bytes and stops on completion, timeout/read failure,
// unencodable input or a special key other than Backspace. It does not replay a
// character already consumed when a size/encoding check ends assembly.
std::string read_escape_sequence_body(terminal::Context& terminal_context)
{
  std::string consumed;
  consumed.reserve(32);
  // Stop as soon as the protocol recognizes a complete body so a following key
  // stays available for the next input-budget item rather than joining this one.
  while (consumed.size() < kMaxEscapeSequenceBytes)
  {
    auto const character = terminal_context.read_escape_sequence_character();
    if (!character)
      break;
    auto const encoded = encode_wide_character(*character);
    if (!encoded || encoded->size() > kMaxEscapeSequenceBytes - consumed.size())
      break;
    consumed += *encoded;
    if (terminal_escape_sequence_complete(consumed))
      break;
  }
  return consumed;
}

// TUI thread after consuming paste-start, with Context alive: assemble one owned
// normalized paste prefix until end marker, signal, read failure/timeout or byte cap.
// The temporary 1 s timeout applies to each read, not to the complete paste; restore
// the previous timeout before returning. If the marker has not arrived, later polls
// discard the tail instead of dispatching its characters as ordinary keys.
RuntimeInput read_bracketed_paste(terminal::Context& terminal_context)
{
  auto& replay = input_replay();
  replay.begin_paste();
  std::string pasted;
  {
    auto scoped_timeout = terminal_context.timeout(std::chrono::milliseconds(1000));
    while (!Signals::received(terminal_signals) && pasted.size() < kMaxBracketedPasteBytes)
    {
      auto const character = terminal_context.read_plain_wide_character();
      if (!character)
        break;
      if (replay.observe_paste_character(*character))
        break;
      if (*character == L'\x1b')
      {
        // Assemble a complete bounded escape/control sequence
        // after ESC. Paste-end ends the paste; an armed OSC 11 reply is handled and
        // discarded without joining the paste payload; all other escape content is
        // preserved under ordinary paste normalization and the byte cap.
        auto const consumed = read_escape_sequence_body(terminal_context);
        bool ended = false;
        for (unsigned char byte : consumed)
          ended = replay.observe_paste_character(byte) || ended;
        if (ended)
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

// TUI thread after ESC, with Context alive: read continuation under the configured
// per-read escape timeout and restore the caller's setting. No continuation means
// a standalone Escape; a consumed unrecognized/protocol sequence yields Unknown,
// while paste-start assembles one paste event. Returned text owns its bytes.
std::optional<RuntimeInput> read_escape_sequence_input(terminal::Context& terminal_context)
{
  std::string consumed;
  {
    auto scoped_timeout = terminal_context.timeout(std::chrono::milliseconds(config::default_terminal_escape_delay_ms));
    consumed = read_escape_sequence_body(terminal_context);
  }

  if (consumed.empty())
    return std::nullopt;
  // A query reply consumes input even though no key should reach a feature. Keep
  // that distinction from a lone Escape so polling continues through buffered keys.
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

}  // namespace

// TUI input handlers borrow input only for this call; return owned bytes for one
// jump target without terminal access. Paste cannot accidentally trigger a jump.
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

  // Use the first UTF-8 sequence's byte shape rather than one byte of a multibyte
  // character; this checks width/continuations, not full Unicode scalar validity.
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

// TUI thread: discard saved startup events, including during prompt input cleanup.
// Keep incomplete-paste tracking so a delayed tail cannot become ordinary keys.
void clear_startup_input_queue()
{
  input_replay().clear_startup();
}

// TUI session entry/exit: clear replay and paste tracking before another session decodes input.
void reset_input_decoder()
{
  input_replay().reset_session();
}

// TUI startup code uses this nonconsuming count to avoid reading beyond replay capacity.
std::size_t startup_input_queue_size()
{
  return input_replay().startup_size();
}

// TUI startup decoder: save an actionable owned event in arrival order. Full means
// the passed value is not saved; callers must check capacity before reading more input.
bool enqueue_startup_input(RuntimeInput input)
{
  return input_replay().enqueue(std::move(input));
}

// TUI thread with a live Context: decode one native/replayed item using the current
// timeout, bypassing this module's startup queue. Only native ERR gives nullopt;
// unsupported input retains a value so polling cannot mistake it for drained input.
// Escape/paste may consume additional reads; the returned event owns all of its text.
static std::optional<RuntimeInput> decode_terminal_input(terminal::Context& terminal_context)
{
  wint_t value = 0;
  auto key = terminal_context.read_curses_key(value);
  if (!key)
    return std::nullopt;
  // Signal handling remains TuiSession's job. When a read did consume a key,
  // preserve the existing CtrlC routing for a terminal signal already observed here.
  if (Signals::received(terminal_signals))
    return key_input(Key::CtrlC);
  // A cap or timeout ends the retained prefix, not the terminal's paste. Spend
  // one input-budget item per tail character; never turn it into prompt keys.
  if (input_replay().discard_tail(*key, value))
    return unknown_input();
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
  if (key == Key::Resize)
  {
    // Resize is actionable without a normal key or text payload; its separate flag
    // prevents empty_curses_input from discarding the required layout update.
    return RuntimeInput{.event = InputEvent{.key = Key::Unknown, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0},
                        .text = {},
                        .bracketed_paste = false,
                        .resize = true};
  }
  if (key == Key::Mouse)
  {
    auto mouse_event = terminal::get_mouse_event();
    if (!mouse_event)
      return unknown_input();
    return event_input(terminal_ncurses_mouse_event(*mouse_event));
  }

  return key_input(*key);
}

// TUI startup path with Application initialized: use its live Context while leaving
// this module's startup FIFO untouched, so the probe cannot read and requeue saved events.
RuntimeInput read_curses_input_from_terminal()
{
  return read_curses_input_from_terminal(core::Application::instance().terminal_context());
}

// TUI thread: borrow Context for one decode and return owned input. This legacy
// interface maps both native emptiness and unsupported input to Unknown.
RuntimeInput read_curses_input_from_terminal(terminal::Context& terminal_context)
{
  return decode_terminal_input(terminal_context).value_or(unknown_input());
}

// TUI decoding paths use this pure borrowed-value check to suppress unsupported
// events. It cannot prove the native queue is empty; resize and empty paste still
// count as real events because their flags convey work independently of text.
bool empty_curses_input(RuntimeInput const& input)
{
  return !input.resize && input.event.key == Key::Unknown && input.text.empty() && !input.bracketed_paste;
}

// TUI input stage: consume replay or try one initial native read without sleeping.
// Keep discarded input distinct from ERR, and restore the prior timeout before
// returning. Callers charge both Input and Discarded against the turn's input budget.
InputPollResult poll_curses_input()
{
  if (auto queued = input_replay().take_startup())
    return {.kind = InputPollKind::Input, .input = std::move(*queued)};
  // Only the initial native read is nonblocking. The decoder temporarily applies
  // its existing escape/paste continuation timeouts once a sequence has started.
  auto& terminal_context = core::Application::instance().terminal_context();
  auto scoped_timeout = terminal_context.timeout(std::chrono::milliseconds(0));
  auto input = decode_terminal_input(terminal_context);
  if (!input)
    return {.kind = InputPollKind::Empty};
  if (empty_curses_input(*input))
    return {.kind = InputPollKind::Discarded};
  return {.kind = InputPollKind::Input, .input = std::move(*input)};
}

// TUI startup stage with Application initialized: preserve user input while query
// replies are consumed. deadline is an outer duration checked between decodes;
// a started escape/paste decode may exceed it. Stop before reading past FIFO capacity.
void drain_startup_probe_input(std::chrono::milliseconds deadline)
{
  using clock = std::chrono::steady_clock;
  auto& terminal_context = core::Application::instance().terminal_context();
  auto const started = clock::now();
  // Bypass the startup queue below; polling replay would take the oldest saved
  // event and enqueue it again instead of draining the native terminal.
  while (input_replay().startup_size() < kStartupInputQueueCap)
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
