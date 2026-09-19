#include "sys.h"
#include "Context.h"
#include "KeyboardInputMode.h"

#include <chrono>
#include <string_view>
#include <thread>
#include <utility>
#include "debug.h"

namespace ava::tui::terminal {
namespace {

constexpr std::chrono::milliseconds kKeyboardNegotiationTimeout{50};
constexpr std::string_view kKittyPushQueryAndDeviceAttributes = "\x1b[>1u\x1b[?u\x1b[c";
constexpr std::string_view kModifyOtherKeysLevel2 = "\x1b[>4;2m";
constexpr std::string_view kDisableModifyOtherKeys = "\x1b[>4;0m";
constexpr std::string_view kPopKittyKeyboard = "\x1b[<u";

// Return whether `character` is a decimal digit accepted in a Kitty flags reply.
bool is_decimal_digit(char character)
{
  return character >= '0' && character <= '9';
}

// Return whether `parameters` is a non-empty semicolon-separated list of decimal integers without empty fields.
bool is_decimal_parameter_list(std::string_view parameters)
{
  bool expecting_digit = true;
  for (char const character : parameters)
  {
    if (is_decimal_digit(character))
    {
      expecting_digit = false;
      continue;
    }
    if (character != ';' || expecting_digit)
      return false;
    expecting_digit = true;
  }
  return !parameters.empty() && !expecting_digit;
}

// Remove complete expected Kitty flags and primary device-attributes replies from `bytes`, retaining every unrelated byte in place.
//
// `kitty_enabled` becomes true after any nonzero flags reply and is never reset by a later zero reply. `device_attributes_seen` records a
// syntactically valid primary DA reply. Incomplete and malformed candidates remain buffered for later input or timeout replay.
void consume_expected_replies(std::string& bytes, bool& kitty_enabled, bool& device_attributes_seen)
{
  std::size_t position = 0;
  while ((position = bytes.find("\x1b[?", position)) != std::string::npos)
  {
    std::size_t cursor = position + 3;
    std::size_t const parameter_begin = cursor;
    while (cursor < bytes.size() && (is_decimal_digit(bytes[cursor]) || bytes[cursor] == ';'))
      ++cursor;
    if (cursor == bytes.size())
      return;

    std::string_view const parameters{bytes.data() + parameter_begin, cursor - parameter_begin};
    bool const valid_parameters = is_decimal_parameter_list(parameters);
    bool const kitty_reply = valid_parameters && parameters.find(';') == std::string_view::npos && bytes[cursor] == 'u';
    bool const device_attributes_reply = valid_parameters && bytes[cursor] == 'c';
    if (!kitty_reply && !device_attributes_reply)
    {
      ++position;
      continue;
    }

    if (kitty_reply)
    {
      bool nonzero = false;
      for (char const digit : parameters)
        nonzero = nonzero || digit != '0';
      kitty_enabled = kitty_enabled || nonzero;
    }
    else if (device_attributes_reply)
      device_attributes_seen = true;

    bytes.erase(position, cursor - position + 1);
  }
}

} // namespace

// Restore terminal keyboard modes on scope exit.
KeyboardInputMode::~KeyboardInputMode() noexcept
{
  stop();
}

// Negotiate Kitty keyboard reporting, preserving all bytes outside recognized protocol replies.
void KeyboardInputMode::start(Context& context)
{
  if (context_)
  {
    // Call stop() before reusing a KeyboardInputMode with another start() invocation.
    ASSERT(context_ == nullptr);
    return;
  }

  context_ = &context;
  kitty_push_requested_ = true;
  static_cast<void>(context.write_raw_sequence(kKittyPushQueryAndDeviceAttributes));

  using Clock = std::chrono::steady_clock;
  auto const deadline = Clock::now() + kKeyboardNegotiationTimeout;
  bool kitty_enabled = false;
  bool device_attributes_seen = false;
  while (Clock::now() < deadline)
  {
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    std::string bytes = context.read_raw_input_for(remaining);
    if (bytes.empty())
    {
      // Regular files report EOF immediately; preserve the same bounded negotiation deadline used for terminal descriptors.
      std::this_thread::sleep_until(deadline);
      break;
    }
    buffered_input_ += bytes;
    consume_expected_replies(buffered_input_, kitty_enabled, device_attributes_seen);
    if (kitty_enabled && device_attributes_seen)
      break;
  }

  if (!kitty_enabled)
  {
    // Record the request before writing because a short or failed write may still have changed the terminal mode.
    modify_other_keys_requested_ = true;
    static_cast<void>(context.write_raw_sequence(kModifyOtherKeysLevel2));
  }

  remaining_buffered_input_ = buffered_input_;
}

// Best-effort reverse mode requests in the opposite order from activation.
void KeyboardInputMode::stop() noexcept
{
  Context* const context = std::exchange(context_, nullptr);
  if (!context)
    return;

  bool const disable_modify_other_keys = std::exchange(modify_other_keys_requested_, false);
  bool const pop_kitty = std::exchange(kitty_push_requested_, false);
  if (disable_modify_other_keys)
    static_cast<void>(context->write_raw_sequence(kDisableModifyOtherKeys));
  if (pop_kitty)
    static_cast<void>(context->write_raw_sequence(kPopKittyKeyboard));
}

} // namespace ava::tui::terminal
