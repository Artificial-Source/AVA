#pragma once

#include <concepts>
#include <limits>

namespace ava::core {

// Clamp unsigned addition to the type's maximum instead of wrapping.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T saturating_add(T left, T right) noexcept
{
  constexpr auto maximum = std::numeric_limits<T>::max();
  return right > maximum - left ? maximum : static_cast<T>(left + right);
}

}  // namespace ava::core
