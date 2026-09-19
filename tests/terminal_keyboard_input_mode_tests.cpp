#include "sys.h"
#include "support/test_harness.h"
#include "support/terminal_test_support.h"
#include "terminal/Context.h"
#include "terminal/KeyboardInputMode.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace terminal = ava::tui::terminal;

namespace {

constexpr std::string_view kModifyOtherKeysLevel2 = "\x1b[>4;2m";
constexpr std::string_view kDisableModifyOtherKeys = "\x1b[>4;0m";
constexpr std::string_view kPopKittyKeyboard = "\x1b[<u";

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

  // This will be read by the terminal::KeyboardInputMode::start.
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

// Verify that an absent reply reaches the bounded fallback and cleanup reverses both possible activations.
void test_timeout_uses_modify_other_keys_fallback()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  prepare_input(input.get(), {});
  terminal::KeyboardInputMode mode;
  mode.start(context);
  mode.stop();

  std::string const emitted = read_output(output.get());
  expect(count_occurrences(emitted, kModifyOtherKeysLevel2) == 1, "missing Kitty replies must request modifyOtherKeys level 2 once");
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

// Verify that only complete expected protocol replies are consumed and all other raw bytes remain replayable exactly once.
void test_unrelated_input_is_buffered()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  terminal::Context context(output.get(), input.get());
  reset_output_file(output.get());

  // Prepare input for mode.start.
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
  test_kitty_reply_selects_kitty();
  test_timeout_uses_modify_other_keys_fallback();
  test_unrelated_input_is_buffered();
  test_stop_is_idempotent();
  test_destructor_stops_active_mode();
}
