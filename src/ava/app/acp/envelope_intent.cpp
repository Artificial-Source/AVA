#include "sys.h"
#include "ava/app/acp/envelope_intent.h"
#include "ava/core/hex.h"

#include <limits>

namespace ava::app::acp {
namespace {

constexpr std::uint8_t kIdCandidate = 1U;
constexpr std::uint8_t kMethodCandidate = 2U;

bool is_json_whitespace(char ch) noexcept
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

bool is_value_delimiter(char ch) noexcept
{
  return is_json_whitespace(ch) || ch == ',' || ch == '}';
}

}  // namespace

void EnvelopeIntentScanner::mark_malformed() noexcept
{
  malformed_ = true;
}

void EnvelopeIntentScanner::open_container(char ch) noexcept
{
  if (depth_ < containers_.size())
    containers_[depth_] = ch;
  else
    untracked_nesting_ = true;
  if (depth_ == std::numeric_limits<std::size_t>::max())
    mark_malformed();
  else
    ++depth_;
}

bool EnvelopeIntentScanner::close_container(char ch) noexcept
{
  if (depth_ == 0)
  {
    mark_malformed();
    return false;
  }
  if (depth_ <= containers_.size())
  {
    char const expected = ch == '}' ? '{' : '[';
    if (containers_[depth_ - 1] != expected)
    {
      mark_malformed();
      state_ = State::Invalid;
      return false;
    }
  }
  --depth_;
  return true;
}

void EnvelopeIntentScanner::begin_string(StringRole role) noexcept
{
  in_string_ = true;
  escaped_ = false;
  unicode_digits_remaining_ = 0;
  unicode_value_ = 0;
  string_role_ = role;
  if (role == StringRole::Key)
  {
    key_candidates_ = kIdCandidate | kMethodCandidate;
    key_length_ = 0;
    key_is_id_ = false;
    key_is_method_ = false;
  }
}

void EnvelopeIntentScanner::consume_key_character(unsigned value) noexcept
{
  constexpr std::string_view id = "id";
  constexpr std::string_view method = "method";
  if (value > 0x7FU || key_length_ >= id.size() || static_cast<unsigned char>(id[key_length_]) != value)
    key_candidates_ &= static_cast<std::uint8_t>(~kIdCandidate);
  if (value > 0x7FU || key_length_ >= method.size() || static_cast<unsigned char>(method[key_length_]) != value)
    key_candidates_ &= static_cast<std::uint8_t>(~kMethodCandidate);
  if (key_length_ != std::numeric_limits<std::size_t>::max())
    ++key_length_;
}

void EnvelopeIntentScanner::finish_string() noexcept
{
  in_string_ = false;
  if (string_role_ == StringRole::Key)
  {
    key_is_id_ = (key_candidates_ & kIdCandidate) != 0 && key_length_ == 2;
    key_is_method_ = (key_candidates_ & kMethodCandidate) != 0 && key_length_ == 6;
    if (key_is_id_)
    {
      duplicate_envelope_key_ = duplicate_envelope_key_ || saw_id_;
      saw_id_ = true;
    }
    if (key_is_method_)
    {
      duplicate_envelope_key_ = duplicate_envelope_key_ || saw_method_;
      saw_method_ = true;
      saw_reliable_method_ = saw_reliable_method_ || (!untracked_nesting_ && !malformed_);
    }
    state_ = State::Colon;
  }
  else if (string_role_ == StringRole::Value && depth_ == 1)
    state_ = State::CommaOrEnd;
  string_role_ = StringRole::None;
}

void EnvelopeIntentScanner::consume_string(char ch) noexcept
{
  if (unicode_digits_remaining_ != 0)
  {
    int const digit = ava::core::hex_digit_value(ch);
    if (digit < 0)
    {
      mark_malformed();
      unicode_digits_remaining_ = 0;
      if (string_role_ == StringRole::Key)
        consume_key_character(0x100U);
      return;
    }
    unicode_value_ = (unicode_value_ << 4U) | static_cast<unsigned>(digit);
    --unicode_digits_remaining_;
    if (unicode_digits_remaining_ == 0 && string_role_ == StringRole::Key)
      consume_key_character(unicode_value_);
    return;
  }

  if (escaped_)
  {
    escaped_ = false;
    unsigned decoded = 0x100U;
    switch (ch)
    {
      case '"':
        decoded = '"';
        break;
      case '\\':
        decoded = '\\';
        break;
      case '/':
        decoded = '/';
        break;
      case 'b':
        decoded = '\b';
        break;
      case 'f':
        decoded = '\f';
        break;
      case 'n':
        decoded = '\n';
        break;
      case 'r':
        decoded = '\r';
        break;
      case 't':
        decoded = '\t';
        break;
      case 'u':
        unicode_value_ = 0;
        unicode_digits_remaining_ = 4;
        return;
      default:
        mark_malformed();
        break;
    }
    if (string_role_ == StringRole::Key)
      consume_key_character(decoded);
    return;
  }

  if (ch == '\\')
  {
    escaped_ = true;
    return;
  }
  if (ch == '"')
  {
    finish_string();
    return;
  }
  if (static_cast<unsigned char>(ch) < 0x20U)
    mark_malformed();
  if (string_role_ == StringRole::Key)
    consume_key_character(static_cast<unsigned char>(ch));
}

void EnvelopeIntentScanner::begin_primitive(char ch) noexcept
{
  literal_offset_ = 1;
  if (ch == 't')
    primitive_ = Primitive::True;
  else if (ch == 'f')
    primitive_ = Primitive::False;
  else if (ch == 'n')
    primitive_ = Primitive::Null;
  else
  {
    primitive_ = Primitive::Number;
    if (ch == '-')
      number_state_ = NumberState::Minus;
    else if (ch == '0')
      number_state_ = NumberState::Zero;
    else if (ch >= '1' && ch <= '9')
      number_state_ = NumberState::Integer;
    else
      mark_malformed();
  }
  state_ = State::Primitive;
}

void EnvelopeIntentScanner::consume_primitive(char ch) noexcept
{
  if (primitive_ != Primitive::Number)
  {
    std::string_view expected;
    if (primitive_ == Primitive::True)
      expected = "true";
    else if (primitive_ == Primitive::False)
      expected = "false";
    else
      expected = "null";
    if (literal_offset_ >= expected.size() || expected[literal_offset_] != ch)
      mark_malformed();
    if (literal_offset_ != std::numeric_limits<std::size_t>::max())
      ++literal_offset_;
    return;
  }

  switch (number_state_)
  {
    case NumberState::Minus:
      if (ch == '0')
        number_state_ = NumberState::Zero;
      else if (ch >= '1' && ch <= '9')
        number_state_ = NumberState::Integer;
      else
        mark_malformed();
      break;
    case NumberState::Zero:
      if (ch == '.')
        number_state_ = NumberState::Dot;
      else if (ch == 'e' || ch == 'E')
        number_state_ = NumberState::Exponent;
      else
        mark_malformed();
      break;
    case NumberState::Integer:
      if (ch >= '0' && ch <= '9')
        break;
      if (ch == '.')
        number_state_ = NumberState::Dot;
      else if (ch == 'e' || ch == 'E')
        number_state_ = NumberState::Exponent;
      else
        mark_malformed();
      break;
    case NumberState::Dot:
      if (ch >= '0' && ch <= '9')
        number_state_ = NumberState::Fraction;
      else
        mark_malformed();
      break;
    case NumberState::Fraction:
      if (ch >= '0' && ch <= '9')
        break;
      if (ch == 'e' || ch == 'E')
        number_state_ = NumberState::Exponent;
      else
        mark_malformed();
      break;
    case NumberState::Exponent:
      if (ch == '+' || ch == '-')
        number_state_ = NumberState::ExponentSign;
      else if (ch >= '0' && ch <= '9')
        number_state_ = NumberState::ExponentDigits;
      else
        mark_malformed();
      break;
    case NumberState::ExponentSign:
      if (ch >= '0' && ch <= '9')
        number_state_ = NumberState::ExponentDigits;
      else
        mark_malformed();
      break;
    case NumberState::ExponentDigits:
      if (ch < '0' || ch > '9')
        mark_malformed();
      break;
  }
}

bool EnvelopeIntentScanner::primitive_complete() const noexcept
{
  if (primitive_ == Primitive::True)
    return literal_offset_ == 4;
  if (primitive_ == Primitive::False)
    return literal_offset_ == 5;
  if (primitive_ == Primitive::Null)
    return literal_offset_ == 4;
  return number_state_ == NumberState::Zero || number_state_ == NumberState::Integer || number_state_ == NumberState::Fraction ||
         number_state_ == NumberState::ExponentDigits;
}

void EnvelopeIntentScanner::consume(char ch) noexcept
{
  if (in_string_)
  {
    consume_string(ch);
    return;
  }

  if (depth_ > 1)
  {
    if (ch == '"')
      begin_string(StringRole::Nested);
    else if (ch == '{' || ch == '[')
      open_container(ch);
    else if (ch == '}' || ch == ']')
    {
      if (close_container(ch) && depth_ == 1)
        state_ = State::CommaOrEnd;
    }
    return;
  }

  if (state_ == State::Primitive)
  {
    if (!is_value_delimiter(ch))
    {
      consume_primitive(ch);
      return;
    }
    if (!primitive_complete())
      mark_malformed();
    state_ = State::CommaOrEnd;
  }

  switch (state_)
  {
    case State::Leading:
      if (is_json_whitespace(ch))
        return;
      if (ch == '{')
      {
        root_object_ = true;
        open_container(ch);
        state_ = State::KeyOrEnd;
      }
      else
      {
        mark_malformed();
        state_ = State::Invalid;
      }
      return;
    case State::KeyOrEnd:
      if (is_json_whitespace(ch))
        return;
      if (ch == '}' && close_container(ch))
        state_ = State::Complete;
      else if (ch == '"')
        begin_string(StringRole::Key);
      else
      {
        mark_malformed();
        state_ = State::Invalid;
      }
      return;
    case State::Key:
      if (is_json_whitespace(ch))
        return;
      if (ch == '"')
        begin_string(StringRole::Key);
      else
      {
        mark_malformed();
        state_ = State::Invalid;
      }
      return;
    case State::Colon:
      if (is_json_whitespace(ch))
        return;
      if (ch == ':')
        state_ = State::Value;
      else
      {
        mark_malformed();
        state_ = State::Invalid;
      }
      return;
    case State::Value:
      if (is_json_whitespace(ch))
        return;
      if (ch == '"')
        begin_string(StringRole::Value);
      else if (ch == '{' || ch == '[')
        open_container(ch);
      else if (ch == ',' || ch == '}')
      {
        mark_malformed();
        state_ = State::Invalid;
      }
      else
        begin_primitive(ch);
      return;
    case State::CommaOrEnd:
      if (is_json_whitespace(ch))
        return;
      if (ch == ',')
        state_ = State::Key;
      else if (ch == '}' && close_container(ch))
        state_ = State::Complete;
      else
      {
        mark_malformed();
        state_ = State::Invalid;
      }
      return;
    case State::Complete:
      if (!is_json_whitespace(ch))
      {
        mark_malformed();
        state_ = State::Invalid;
      }
      return;
    case State::Primitive:
    case State::Invalid:
      return;
  }
}

EnvelopeScanResult EnvelopeIntentScanner::finish() noexcept
{
  if (state_ != State::Complete || in_string_ || escaped_ || unicode_digits_remaining_ != 0)
    mark_malformed();

  EnvelopeIntent intent = EnvelopeIntent::Unknown;
  if (root_object_)
  {
    if (saw_method_ && (!saw_id_ || saw_reliable_method_))
      intent = saw_id_ ? EnvelopeIntent::Request : EnvelopeIntent::Notification;
    else if (saw_id_)
      intent = EnvelopeIntent::Response;
  }
  return EnvelopeScanResult{.intent = intent, .top_level_object = root_object_, .ambiguous = malformed_ || untracked_nesting_ || duplicate_envelope_key_};
}

EnvelopeScanResult scan_envelope_intent(std::string_view record) noexcept
{
  EnvelopeIntentScanner scanner;
  for (char ch : record) scanner.consume(ch);
  return scanner.finish();
}

EnvelopeIntent loop_safe_oversized_intent(EnvelopeScanResult const& scan) noexcept
{
  if (scan.intent != EnvelopeIntent::Unknown)
    return scan.intent;
  if (scan.top_level_object && scan.ambiguous)
    return EnvelopeIntent::Response;
  return EnvelopeIntent::Unknown;
}

}  // namespace ava::app::acp
