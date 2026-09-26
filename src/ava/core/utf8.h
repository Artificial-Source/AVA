#pragma once

#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ava::core {

struct DecodedUtf8Scalar
{
  std::uint32_t scalar = 0;
  std::size_t length = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Return the byte width indicated by an ASCII byte or a permitted UTF-8 lead
// byte. Invalid leads, including continuation bytes, C0/C1, and F5-FF, return
// zero. This examines only the lead and does not imply that following bytes exist.
[[nodiscard]] inline constexpr std::size_t utf8_expected_width(unsigned char lead) noexcept
{
  if (lead <= 0x7fU)
    return 1;
  if (lead >= 0xc2U && lead <= 0xdfU)
    return 2;
  if (lead >= 0xe0U && lead <= 0xefU)
    return 3;
  if (lead >= 0xf0U && lead <= 0xf4U)
    return 4;
  return 0;
}

// Decode exactly one Unicode scalar beginning at index. An out-of-bounds index,
// truncated sequence, malformed continuation, overlong encoding, surrogate, or
// value above U+10FFFF returns nullopt and consumes nothing.
[[nodiscard]] inline constexpr std::optional<DecodedUtf8Scalar> decode_utf8_scalar(std::string_view text, std::size_t index) noexcept
{
  if (index >= text.size())
    return std::nullopt;

  auto const lead = static_cast<unsigned char>(text[index]);
  auto const length = utf8_expected_width(lead);
  if (length == 0 || length > text.size() - index)
    return std::nullopt;
  if (length == 1)
    return DecodedUtf8Scalar{.scalar = lead, .length = length};

  std::uint32_t scalar = lead & ((1U << (7U - static_cast<unsigned int>(length))) - 1U);
  for (std::size_t offset = 1; offset < length; ++offset)
  {
    auto const byte = static_cast<unsigned char>(text[index + offset]);
    if ((byte & 0xc0U) != 0x80U)
      return std::nullopt;
    scalar = (scalar << 6U) | (byte & 0x3fU);
  }

  constexpr std::uint32_t minimum_by_width[] = {0, 0, 0x80U, 0x800U, 0x10000U};
  if (scalar < minimum_by_width[length] || (scalar >= 0xd800U && scalar <= 0xdfffU) || scalar > 0x10ffffU)
    return std::nullopt;
  return DecodedUtf8Scalar{.scalar = scalar, .length = length};
}

}  // namespace ava::core
