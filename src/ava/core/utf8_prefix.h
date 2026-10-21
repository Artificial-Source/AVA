#pragma once

#include "ava/core/utf8.h"

#include <cstddef>
#include <string_view>

namespace ava::core {

// Candidate to move into utf8.h.
// Return the longest prefix within max_bytes without splitting a valid scalar.
// Each invalid byte consumes one byte, matching the decoder's failure boundary.
[[nodiscard]] constexpr std::size_t utf8_prefix_size(std::string_view text, std::size_t max_bytes) noexcept
{
  std::size_t index = 0;
  while (index < text.size() && index < max_bytes)
  {
    auto const decoded = decode_utf8_scalar(text, index);
    auto const width = decoded ? decoded->length : 1;
    if (width > max_bytes - index)
      break;
    index += width;
  }
  return index;
}

}  // namespace ava::core
