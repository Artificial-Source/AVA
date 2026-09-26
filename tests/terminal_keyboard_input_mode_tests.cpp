#include "sys.h"
#include "support/terminal_test_support.h"
#include "support/test_harness.h"
#include "terminal/Context.h"
#include "terminal/KeyboardInputMode.h"
#include "ava/tui/config.h"
#include "ava/tui/runtime_input_internal.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace terminal = ava::tui::terminal;

namespace {

constexpr std::string_view kModifyOtherKeysLevel2 = "\x1b[>4;2m";
constexpr std::string_view kQueryModifyOtherKeys = "\x1b[?4m";
constexpr std::string_view kRequestDeviceAttributes = "\x1b[c";
constexpr std::string_view kDisableModifyOtherKeys = "\x1b[>4;0m";
constexpr std::string_view kPopKittyKeyboard = "\x1b[<u";

// Temporarily remove one environment variable and restore its exact prior state on destruction.
class ScopedUnsetEnvVar
{
 public:
  explicit ScopedUnsetEnvVar(char const* name) : name_(name)
  {
    if (char const* value = std::getenv(name))
      previous_ = value;
    static_cast<void>(unsetenv(name));
  }

  ScopedUnsetEnvVar(ScopedUnsetEnvVar const&) = delete;
  ScopedUnsetEnvVar& operator=(ScopedUnsetEnvVar const&) = delete;

  ~ScopedUnsetEnvVar()
  {
    if (previous_)
      static_cast<void>(setenv(name_.c_str(), previous_->c_str(), 1));
    else
      static_cast<void>(unsetenv(name_.c_str()));
  }

 private:
  std::string name_;
  std::optional<std::string> previous_;
};

// Verify that Context applies AVA's default only when ESCDELAY does not provide an explicit ncurses value.
void test_context_escape_delay_configuration()
{
  {
    ScopedUnsetEnvVar escdelay_guard("ESCDELAY");
    ScopedTmpFile input;
    ScopedTmpFile output;
    terminal::Context context(output.get(), input.get());
    expect(context.get_escdelay() == ava::tui::config::default_terminal_escape_delay_ms, "Context must use AVA's short Escape delay when ESCDELAY is unset");
  }

  {
    constexpr int configured_delay_ms = 2.25 * ava::tui::config::default_terminal_escape_delay_ms;
    ScopedEnvVar escdelay_guard("ESCDELAY", std::to_string(configured_delay_ms));
    ScopedTmpFile input;
    ScopedTmpFile output;
    terminal::Context context(output.get(), input.get());
    expect(context.get_escdelay() == configured_delay_ms, "an explicit ESCDELAY must take precedence over AVA's default");
  }
}

// Replace the contents of `file` with `bytes` and rewind it for Context input.
void prepare_input(FILE* file, std::string_view bytes)
{
  if (!bytes.empty())
    expect(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(), "all simulated keyboard negotiation input must be written");
  std::rewind(file);
}

// Read every emitted byte from `file`, leaving its position at end-of-file for any later writes.
std::string read_output(FILE* file)
{
  expect(std::fflush(file) == 0, "terminal test output must flush before inspection");
  std::rewind(file);
  std::string result;
  std::array<char, 1024> bytes;
  while (std::size_t const count = std::fread(bytes.data(), 1, bytes.size(), file))
    result.append(bytes.data(), count);
  return result;
}

// Count non-overlapping protocol sequence occurrences in captured terminal output.
std::size_t count_occurrences(std::string_view bytes, std::string_view sequence)
{
  std::size_t count = 0;
  for (std::size_t position = 0; (position = bytes.find(sequence, position)) != std::string_view::npos; position += sequence.size())
    ++count;
  return count;
}

// Verify that a nonzero Kitty flags reply wins and cleanup only pops the possibly successful Kitty push.
void test_kitty_reply_selects_kitty()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  // Context starts its owned KeyboardInputMode, so prepare fresh input only after construction for this separate negotiation.
  write_KeyboardInputMode_reply(input.get(), SupportedMode::KittyProtocol);
  std::rewind(input.get());
  terminal::KeyboardInputMode mode;
  mode.start(context);
  mode.stop();

  std::string const emitted = read_output(output.get());
  expect(emitted.find("\x1b[>1u\x1b[?u\x1b[c") != std::string::npos, "startup must push Kitty flag 1, query flags, and request device attributes");
  expect(count_occurrences(emitted, kPopKittyKeyboard) == 1, "Kitty cleanup must pop exactly one possibly successful push");
  expect(emitted.find(kModifyOtherKeysLevel2) == std::string::npos, "a nonzero Kitty reply must avoid the xterm fallback");
  expect(emitted.find(kDisableModifyOtherKeys) == std::string::npos, "Kitty-only cleanup must not disable modifyOtherKeys");
}

// Verify that fenced unsupported replies select the fallback and cleanup reverses both possible activations.
void test_unsupported_replies_use_modify_other_keys_fallback()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  // Context starts its owned KeyboardInputMode, so prepare fresh input only after construction for this separate negotiation.
  write_KeyboardInputMode_reply(input.get(), SupportedMode::None);
  std::rewind(input.get());
  terminal::KeyboardInputMode mode;
  mode.start(context);
  mode.stop();

  std::string const emitted = read_output(output.get());
  expect(count_occurrences(emitted, kModifyOtherKeysLevel2) == 1, "missing Kitty replies must request modifyOtherKeys level 2 once");
  expect(emitted.find(std::string(kModifyOtherKeysLevel2) + std::string(kQueryModifyOtherKeys) + std::string(kRequestDeviceAttributes)) != std::string::npos,
         "the modifyOtherKeys fallback must set level 2, query it, and request its device-attributes fence");
  expect(count_occurrences(emitted, kDisableModifyOtherKeys) == 1, "fallback cleanup must disable requested modifyOtherKeys once");
  expect(count_occurrences(emitted, kPopKittyKeyboard) == 1, "fallback cleanup must still pop a Kitty push that may have succeeded");
  expect(emitted.find(std::string(kDisableModifyOtherKeys) + std::string(kPopKittyKeyboard)) != std::string::npos,
         "cleanup must disable the later fallback before popping the earlier Kitty request");
}

std::string take_buffered_input(terminal::KeyboardInputMode& mode)
{
  std::string buffer_content;
  wint_t wch;
  while (mode.try_get_wch(&wch))
    buffer_content += static_cast<char>(wch);
  return buffer_content;
}

// Verify that one preloaded read can cross phase boundaries while each parser consumes only its own fenced replies.
void test_modify_other_keys_reply_is_consumed()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  std::string const before = "before\x1b[>4;;2m";
  std::string const after = "after\x1b[>";
  expect(std::fwrite(before.data(), 1, before.size(), input.get()) == before.size(), "all leading keyboard negotiation input must be written");
  write_KeyboardInputMode_reply(input.get(), SupportedMode::ModifyOtherKeys);
  expect(std::fwrite(after.data(), 1, after.size(), input.get()) == after.size(), "all trailing keyboard negotiation input must be written");
  std::rewind(input.get());

  terminal::KeyboardInputMode mode;
  mode.start(context);
  expect(take_buffered_input(mode) == before + after,
         "valid fallback replies must be consumed while malformed, unrelated, and incomplete bytes remain byte-for-byte");
  mode.stop();

  std::string const emitted = read_output(output.get());
  expect(emitted.find(std::string(kModifyOtherKeysLevel2) + std::string(kQueryModifyOtherKeys) + std::string(kRequestDeviceAttributes)) != std::string::npos,
         "supported modifyOtherKeys negotiation must emit one ordered set, query, and fence request");
  expect(count_occurrences(emitted, kDisableModifyOtherKeys) == 1,
         "a requested modifyOtherKeys level must be disabled even when its query response confirms support");
}

// Verify that only complete expected protocol replies are consumed and all other raw bytes remain replayable exactly once.
void test_unrelated_input_is_buffered()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  // Context starts its owned KeyboardInputMode, so these are fresh replies for mode.start below.
  std::string const unrelated1 = "text\x1b[31m\x1b[?xu\x1b[?1;;2c";
  std::string const unrelated2 = "\x1b[";
  std::string const unrelated = unrelated1 + unrelated2;
  expect(std::fwrite(unrelated1.data(), 1, unrelated1.size(), input.get()) == unrelated1.size(), "all unrelated1 keyboard negotiation input must be written");
  write_KeyboardInputMode_reply(input.get(), SupportedMode::KittyProtocol);
  expect(std::fwrite(unrelated2.data(), 1, unrelated2.size(), input.get()) == unrelated2.size(), "all unrelated2 keyboard negotiation input must be written");
  std::rewind(input.get());

  terminal::KeyboardInputMode mode;
  mode.start(context);
  expect(take_buffered_input(mode) == unrelated, "ordinary, malformed, unrelated, and incomplete escape input must survive negotiation byte-for-byte");
  expect(take_buffered_input(mode).empty(), "taking buffered negotiation input must clear it");
}

// Verify Context exposes negotiation-preserved bytes exactly once to the runtime input owner.
void test_context_replays_preserved_startup_input_once()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  std::string const preserved = std::string("typed") + static_cast<char>(0xc3) + static_cast<char>(0xa9);
  expect(std::fwrite(preserved.data(), 1, preserved.size(), input.get()) == preserved.size(), "all startup input bytes must be written");
  write_KeyboardInputMode_reply(input.get(), SupportedMode::KittyProtocol);
  std::rewind(input.get());

  terminal::Context context(output.get(), input.get());
  std::string replayed;
  for (std::size_t index = 0; index < 6; ++index)
    replayed += ava::tui::runtime_input::read_curses_input_from_terminal(context).text;

  expect(replayed == preserved, "runtime input re-encodes canonical wide startup characters to their original UTF-8 text");
  wint_t value = 0;
  expect(!context.try_get_buffered_keyboard_input(&value), "Context exposes each negotiation-preserved character exactly once");
  expect(!context.try_get_buffered_keyboard_input(nullptr), "a null Context output pointer consumes no buffered keyboard input");
}

// Verify a UTF-8 sequence split exactly at Context's bounded raw-read boundary remains one runtime character.
void test_runtime_replays_utf8_split_between_negotiation_and_descriptor()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  std::string bytes = "\x1b[?1u\x1b[?1;2c";
  constexpr std::size_t raw_read_size = 4096;
  bytes.append(raw_read_size - bytes.size() - 1, 'x');
  bytes.push_back(static_cast<char>(0xc3));
  bytes.push_back(static_cast<char>(0xa9));
  expect(std::fwrite(bytes.data(), 1, bytes.size(), input.get()) == bytes.size(), "split UTF-8 startup fixture must be written completely");
  std::rewind(input.get());

  terminal::Context context(output.get(), input.get());
  auto const padding_size = raw_read_size - std::string_view("\x1b[?1u\x1b[?1;2c").size() - 1;
  for (std::size_t index = 0; index < padding_size; ++index)
  {
    auto const input_event = ava::tui::runtime_input::read_curses_input_from_terminal(context);
    expect(input_event.text == "x", "startup padding remains ordered before the split UTF-8 character");
  }
  auto const unicode = ava::tui::runtime_input::read_curses_input_from_terminal(context);
  expect(unicode.event.key == ava::tui::Key::Character && unicode.text == "\xc3\xa9",
         "runtime input combines a UTF-8 sequence split between negotiation storage and the descriptor");
}

// Verify malformed UTF-8 can never be decoded into Escape, a surrogate, or an out-of-range scalar.
void test_invalid_utf8_replays_as_replacement_characters()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  std::string malformed;
  malformed.append("\xc0\x9b", 2);          // Overlong/control alias.
  malformed.append("\xe0\x80\x9b", 3);    // Three-byte overlong Escape alias.
  malformed.append("\xed\xa0\x80", 3);    // UTF-16 high surrogate.
  malformed.append("\xf4\xbf\xbf\xbf", 4); // Above U+10FFFF.
  malformed.append("\xf0\x9f", 2);          // Truncated four-byte sequence.
  malformed += "\x1b[?1u\x1b[?1;2c";
  prepare_input(input.get(), malformed);

  terminal::KeyboardInputMode mode;
  mode.start(context);
  wint_t value = 0;
  std::size_t replacements = 0;
  while (mode.try_get_wch(&value))
  {
    expect(value == 0xfffd, "invalid UTF-8 startup bytes replay only as replacement characters, never controls or invalid scalars");
    ++replacements;
  }
  expect(replacements == 14, "each malformed or truncated byte is consumed exactly once as U+FFFD");
  mode.stop();
}

// Verify handoff compaction drops delivered input while retaining every queued character that has not been observed.
void test_handoff_preserves_only_unconsumed_input()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  prepare_input(input.get(), "abc\x1b[?1u\x1b[?1;2c");
  terminal::KeyboardInputMode mode;
  mode.start(context);
  wint_t first = 0;
  expect(mode.try_get_wch(&first) && first == L'a', "handoff fixture consumes its first queued character");
  mode.stop();
  mode.start(context);
  expect(take_buffered_input(mode) == "bc", "handoff restart retains unconsumed characters without replaying the consumed prefix");
  mode.stop();
}

// Verify that explicit repeated shutdown emits each required restoration sequence only once.
void test_stop_is_idempotent()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  prepare_input(input.get(), {});
  terminal::KeyboardInputMode mode;
  mode.start(context);
  mode.stop();
  mode.stop();

  std::string const emitted = read_output(output.get());
  expect(count_occurrences(emitted, kDisableModifyOtherKeys) == 1, "calling stop twice must emit the modifyOtherKeys cleanup only once");
  expect(count_occurrences(emitted, kPopKittyKeyboard) == 1, "calling stop twice must emit the Kitty pop only once");
}

// Verify that destruction performs the same best-effort restoration when callers omit stop.
void test_destructor_stops_active_mode()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  {
    prepare_input(input.get(), {});
    terminal::KeyboardInputMode mode;
    mode.start(context);
  }

  std::string const emitted = read_output(output.get());
  expect(count_occurrences(emitted, kDisableModifyOtherKeys) == 1, "destruction must disable a requested modifyOtherKeys fallback");
  expect(count_occurrences(emitted, kPopKittyKeyboard) == 1, "destruction must pop a possibly successful Kitty push");
}

} // namespace

// Run deterministic keyboard protocol negotiation and lifecycle tests with a direct-color TERM to avoid palette probing.
void run_terminal_keyboard_input_mode_tests()
{
  ScopedEnvVar term_guard("TERM", "xterm-direct");
  test_context_escape_delay_configuration();
  test_kitty_reply_selects_kitty();
  test_unsupported_replies_use_modify_other_keys_fallback();
  test_modify_other_keys_reply_is_consumed();
  test_unrelated_input_is_buffered();
  test_context_replays_preserved_startup_input_once();
  test_runtime_replays_utf8_split_between_negotiation_and_descriptor();
  test_invalid_utf8_replays_as_replacement_characters();
  test_handoff_preserves_only_unconsumed_input();
  test_stop_is_idempotent();
  test_destructor_stops_active_mode();
}
