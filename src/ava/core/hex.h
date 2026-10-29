#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ava::core {

// Protocol digits are ASCII, independent of the active locale. Invalid bytes return -1.
[[nodiscard]] constexpr int hex_digit_value(char ch) noexcept
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

[[nodiscard]] constexpr bool is_hex_digit(char ch) noexcept
{
  return hex_digit_value(ch) >= 0;
}

// Fixed-width lowercase base16, including leading zeroes; empty input stays empty.
[[nodiscard]] inline std::string hex_encode(std::span<std::byte const> bytes)
{
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (auto const byte : bytes)
  {
    auto const value = std::to_integer<unsigned char>(byte);
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

[[nodiscard]] inline std::string hex_encode(std::span<std::uint8_t const> bytes)
{
  return hex_encode(std::as_bytes(bytes));
}

}  // namespace ava::core
