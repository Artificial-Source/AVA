#include "sys.h"
#include "Context.h"
#include "KeyboardInputMode.h"
#include "ava/core/utf8.h"

#include <chrono>
#include <string_view>
#include <thread>
#include <utility>
#include "debug.h"

namespace ava::tui::terminal {
namespace {

constexpr std::chrono::milliseconds kKeyboardNegotiationTimeout{50};
constexpr std::chrono::milliseconds kBufferedUtf8CompletionTimeout{50};
constexpr int kKittyKeyboardHealthyFlags = 7;
constexpr int kKittyKeyboardDisambiguationOnlyFlags = 1;
constexpr int kKittyKeyboardDesiredFlags = kKittyKeyboardDisambiguationOnlyFlags;
constexpr std::string_view kKittyHealthyPushQueryAndDeviceAttributes = "\x1b[>7u\x1b[?u\x1b[c";
constexpr std::string_view kKittyDisambiguationOnlyPushQueryAndDeviceAttributes = "\x1b[>1u\x1b[?u\x1b[c";
constexpr std::string_view kModifyOtherKeysSetQueryAndDeviceAttributes = "\x1b[>4;2m\x1b[?4m\x1b[c";
constexpr std::string_view kDisableModifyOtherKeys = "\x1b[>4;0m";
constexpr std::string_view kPopKittyKeyboard = "\x1b[<u";

// Return the Kitty push/query sequence for the configured desired flags.
// Both disambiguation-only and the previous full healthy flag set remain supported.
constexpr std::string_view kitty_push_query_and_device_attributes()
{
  static_assert(kKittyKeyboardDesiredFlags == kKittyKeyboardDisambiguationOnlyFlags || kKittyKeyboardDesiredFlags == kKittyKeyboardHealthyFlags,
                "unsupported desired Kitty keyboard flags");
  return kKittyKeyboardDesiredFlags == kKittyKeyboardDisambiguationOnlyFlags ? kKittyDisambiguationOnlyPushQueryAndDeviceAttributes
                                                                             : kKittyHealthyPushQueryAndDeviceAttributes;
}

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
  // Compact only bytes already delivered. Unconsumed input predates this handoff
  // and must remain ahead of any bytes observed by the new negotiation.
  buffered_input_.erase(0, buffered_input_offset_);
  buffered_input_offset_ = 0;
  std::string negotiation_input;
  kitty_push_requested_ = true;
  static_cast<void>(context.write_raw_sequence(kitty_push_query_and_device_attributes()));

  using Clock = std::chrono::steady_clock;
  auto const deadline = Clock::now() + kKeyboardNegotiationTimeout;
  bool kitty_enabled = false;
  bool kitty_fence_seen = consume_kitty_phase_replies(negotiation_input, kitty_enabled);
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
    negotiation_input += bytes;
    kitty_fence_seen = consume_kitty_phase_replies(negotiation_input, kitty_enabled);
  }

  if (!kitty_enabled)
  {
    // Record the request before writing because a short or failed write may still have changed the terminal mode.
    modify_other_keys_requested_ = true;
    static_cast<void>(context.write_raw_sequence(kModifyOtherKeysSetQueryAndDeviceAttributes));

    auto const modify_other_keys_deadline = Clock::now() + kKeyboardNegotiationTimeout;
    bool modify_other_keys_fence_seen = consume_modify_other_keys_phase_replies(negotiation_input);
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
      negotiation_input += bytes;
      modify_other_keys_fence_seen = consume_modify_other_keys_phase_replies(negotiation_input);
    }
  }

  buffered_input_ += negotiation_input;
}

// Replay one logical character while retaining byte order across the raw-negotiation/ncurses boundary.
bool KeyboardInputMode::try_get_wch(wint_t* wch)
{
  if (wch == nullptr || buffered_input_offset_ >= buffered_input_.size())
    return false;

  auto const lead = static_cast<unsigned char>(buffered_input_[buffered_input_offset_]);
  auto const width = ava::core::utf8_expected_width(lead);
  if (width == 0)
  {
    *wch = 0xfffd;
    ++buffered_input_offset_;
    return true;
  }
  if (width > buffered_input_.size() - buffered_input_offset_ && context_ != nullptr)
  {
    using Clock = std::chrono::steady_clock;
    auto const deadline = Clock::now() + kBufferedUtf8CompletionTimeout;
    while (width > buffered_input_.size() - buffered_input_offset_ && Clock::now() < deadline)
    {
      auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
      auto bytes = context_->read_raw_input_for(remaining);
      if (bytes.empty())
        break;
      buffered_input_ += bytes;
    }
  }

  auto const available = buffered_input_.size() - buffered_input_offset_;
  if (width > available)
  {
    *wch = 0xfffd;
    ++buffered_input_offset_;
    return true;
  }

  auto const decoded = ava::core::decode_utf8_scalar(buffered_input_, buffered_input_offset_);
  if (!decoded)
  {
    *wch = 0xfffd;
    ++buffered_input_offset_;
    return true;
  }
  *wch = static_cast<wint_t>(decoded->scalar);
  buffered_input_offset_ += decoded->length;
  return true;
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
