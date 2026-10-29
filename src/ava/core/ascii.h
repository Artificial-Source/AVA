#pragma once

#include <string>
#include <string_view>

namespace ava::core {

[[nodiscard]] constexpr bool is_c0_or_del(unsigned char byte) noexcept
{
  return byte < 0x20 || byte == 0x7f;
}

[[nodiscard]] constexpr bool has_c0_or_del(std::string_view value) noexcept
{
  for (unsigned char byte : value)
    if (is_c0_or_del(byte))
      return true;
  return false;
}

[[nodiscard]] constexpr bool is_ascii_space(unsigned char ch) noexcept
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

// Unlike trim(), this policy is independent of the current C locale.
[[nodiscard]] inline std::string ascii_trim(std::string_view value)
{
  while (!value.empty() && is_ascii_space(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && is_ascii_space(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return std::string(value);
}

[[nodiscard]] constexpr char ascii_lower(char ch) noexcept
{
  return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

[[nodiscard]] inline std::string ascii_lower_copy(std::string_view value)
{
  std::string lowered(value);
  for (char& ch : lowered)
    ch = ascii_lower(ch);
  return lowered;
}

}  // namespace ava::core
