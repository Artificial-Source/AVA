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
constexpr std::string_view kKittyPushQueryAndDeviceAttributes = "\x1b[>7u\x1b[?u\x1b[c";
constexpr std::string_view kModifyOtherKeysSetQueryAndDeviceAttributes = "\x1b[>4;2m\x1b[?4m\x1b[c";
constexpr std::string_view kDisableModifyOtherKeys = "\x1b[>4;0m";
constexpr std::string_view kPopKittyKeyboard = "\x1b[<u";

// Return whether `character` is a decimal digit accepted in a keyboard protocol parameter.
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

// Remove complete expected Kitty flags replies through the first primary device-attributes fence from `bytes`.
//
// `kitty_enabled` becomes true after any nonzero flags reply and is never reset by a later zero reply. The return value reports whether
// the fence was consumed. Bytes after that fence, plus incomplete, malformed, and unrelated bytes, remain in place for a later phase or input.
bool consume_kitty_phase_replies(std::string& bytes, bool& kitty_enabled)
{
  std::size_t position = 0;
  while ((position = bytes.find("\x1b[?", position)) != std::string::npos)
  {
    std::size_t cursor = position + 3;
    std::size_t const parameter_begin = cursor;
    while (cursor < bytes.size() && (is_decimal_digit(bytes[cursor]) || bytes[cursor] == ';'))
      ++cursor;
    if (cursor == bytes.size())
      return false;

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
    bytes.erase(position, cursor - position + 1);
    if (device_attributes_reply)
      return true;
  }
  return false;
}

// Return whether `parameters` identifies an XTMODKEYS query reply for modifyOtherKeys with one decimal level.
bool is_modify_other_keys_reply(std::string_view parameters)
{
  if (!parameters.starts_with("4;") || parameters.size() == 2)
    return false;
  for (char const character : parameters.substr(2))
  {
    if (!is_decimal_digit(character))
      return false;
  }
  return true;
}

// Remove complete XTMODKEYS query replies through the first primary device-attributes fence from `bytes`.
//
// The return value reports whether the fence was consumed. Bytes after that fence, plus incomplete, malformed, and unrelated bytes,
// remain in place for normal input replay.
bool consume_modify_other_keys_phase_replies(std::string& bytes)
{
  std::size_t position = 0;
  while ((position = bytes.find("\x1b[", position)) != std::string::npos)
  {
    if (position + 2 >= bytes.size())
      return false;

    char const selector = bytes[position + 2];
    if (selector != '?' && selector != '>')
    {
      ++position;
      continue;
    }

    std::size_t cursor = position + 3;
    std::size_t const parameter_begin = cursor;
    while (cursor < bytes.size() && (is_decimal_digit(bytes[cursor]) || bytes[cursor] == ';'))
      ++cursor;
    if (cursor == bytes.size())
      return false;

    std::string_view const parameters{bytes.data() + parameter_begin, cursor - parameter_begin};
    bool const device_attributes_reply = selector == '?' && is_decimal_parameter_list(parameters) && bytes[cursor] == 'c';
    bool const modify_other_keys_reply = selector == '>' && is_modify_other_keys_reply(parameters) && bytes[cursor] == 'm';
    if (!device_attributes_reply && !modify_other_keys_reply)
    {
      ++position;
      continue;
    }

    bytes.erase(position, cursor - position + 1);
    if (device_attributes_reply)
      return true;
  }
  return false;
}

} // namespace

// Restore terminal keyboard modes on scope exit.
KeyboardInputMode::~KeyboardInputMode() noexcept
{
  stop();
}

// Negotiate fenced Kitty or modifyOtherKeys keyboard reporting, preserving all bytes outside recognized phase replies.
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
  bool kitty_fence_seen = consume_kitty_phase_replies(buffered_input_, kitty_enabled);
  while (!kitty_fence_seen && Clock::now() < deadline)
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
    kitty_fence_seen = consume_kitty_phase_replies(buffered_input_, kitty_enabled);
  }

  if (!kitty_enabled)
  {
    // Record the request before writing because a short or failed write may still have changed the terminal mode.
    modify_other_keys_requested_ = true;
    static_cast<void>(context.write_raw_sequence(kModifyOtherKeysSetQueryAndDeviceAttributes));

    auto const modify_other_keys_deadline = Clock::now() + kKeyboardNegotiationTimeout;
    bool modify_other_keys_fence_seen = consume_modify_other_keys_phase_replies(buffered_input_);
    while (!modify_other_keys_fence_seen && Clock::now() < modify_other_keys_deadline)
    {
      auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(modify_other_keys_deadline - Clock::now());
      std::string bytes = context.read_raw_input_for(remaining);
      if (bytes.empty())
      {
        // Regular files report EOF immediately; preserve the same bounded negotiation deadline used for terminal descriptors.
        std::this_thread::sleep_until(modify_other_keys_deadline);
        break;
      }
      buffered_input_ += bytes;
      modify_other_keys_fence_seen = consume_modify_other_keys_phase_replies(buffered_input_);
    }
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
