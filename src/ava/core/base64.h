#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ava::core {

enum class Base64Alphabet
{
  Standard,
  Url
};
enum class Base64Padding
{
  Required,
  Omitted
};
enum class Base64TailBits
{
  Canonical,
  Unchecked
};

[[nodiscard]] constexpr int base64_digit_value(char ch, Base64Alphabet alphabet = Base64Alphabet::Standard) noexcept
{
  if (ch >= 'A' && ch <= 'Z')
    return ch - 'A';
  if (ch >= 'a' && ch <= 'z')
    return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9')
    return ch - '0' + 52;
  if (ch == (alphabet == Base64Alphabet::Standard ? '+' : '-'))
    return 62;
  if (ch == (alphabet == Base64Alphabet::Standard ? '/' : '_'))
    return 63;
  return -1;
}

[[nodiscard]] inline std::string base64_encode(std::span<std::byte const> bytes, Base64Alphabet alphabet = Base64Alphabet::Standard,
                                               Base64Padding padding = Base64Padding::Required)
{
  auto const digits = alphabet == Base64Alphabet::Standard ? std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")
                                                           : std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
  std::string result;
  result.reserve((bytes.size() / 3U) * 4U + (bytes.size() % 3U == 0 ? 0U : 4U));
  while (!bytes.empty())
  {
    auto const count = bytes.size() < 3U ? bytes.size() : 3U;
    auto block = std::to_integer<std::uint32_t>(bytes[0]) << 16U;
    if (count > 1U)
      block |= std::to_integer<std::uint32_t>(bytes[1]) << 8U;
    if (count > 2U)
      block |= std::to_integer<std::uint32_t>(bytes[2]);
    result.push_back(digits[(block >> 18U) & 0x3fU]);
    result.push_back(digits[(block >> 12U) & 0x3fU]);
    if (count > 1U)
      result.push_back(digits[(block >> 6U) & 0x3fU]);
    else if (padding == Base64Padding::Required)
      result.push_back('=');
    if (count > 2U)
      result.push_back(digits[block & 0x3fU]);
    else if (padding == Base64Padding::Required)
      result.push_back('=');
    bytes = bytes.subspan(count);
  }
  return result;
}

[[nodiscard]] inline std::string base64_encode(std::string_view bytes, Base64Alphabet alphabet = Base64Alphabet::Standard,
                                               Base64Padding padding = Base64Padding::Required)
{
  return base64_encode(std::as_bytes(std::span(bytes.data(), bytes.size())), alphabet, padding);
}

// Empty input is valid at the codec layer. Protocol callers own nonempty and size
// requirements. Padding must exactly match the selected policy; whitespace and
// mixed alphabets are rejected. Unchecked preserves decoders accepting nonzero tail bits.
[[nodiscard]] inline std::optional<std::string> base64_decode(std::string_view encoded, Base64Alphabet alphabet = Base64Alphabet::Standard,
                                                              Base64Padding padding = Base64Padding::Required,
                                                              Base64TailBits tail_bits = Base64TailBits::Canonical,
                                                              std::size_t max_bytes = std::numeric_limits<std::size_t>::max())
{
  auto digits = encoded;
  if (padding == Base64Padding::Required)
  {
    if (encoded.size() % 4U != 0)
      return std::nullopt;
    if (digits.ends_with('='))
      digits.remove_suffix(1);
    if (digits.ends_with('='))
      digits.remove_suffix(1);
  }
  auto const remainder = digits.size() % 4U;
  if (remainder == 1U)
    return std::nullopt;
  auto const decoded_size = (digits.size() / 4U) * 3U + (remainder == 0U ? 0U : remainder - 1U);
  if (decoded_size > max_bytes)
    return std::nullopt;
  std::string decoded;
  decoded.reserve(decoded_size);
  std::uint32_t buffer = 0;
  unsigned bits = 0;
  for (char const ch : digits)
  {
    int const value = base64_digit_value(ch, alphabet);
    if (value < 0)
      return std::nullopt;
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
    bits += 6U;
    if (bits >= 8U)
    {
      bits -= 8U;
      decoded.push_back(static_cast<char>((buffer >> bits) & 0xffU));
    }
  }
  if (tail_bits == Base64TailBits::Canonical && (buffer & ((1U << bits) - 1U)) != 0)
    return std::nullopt;
  return decoded;
}

}  // namespace ava::core
