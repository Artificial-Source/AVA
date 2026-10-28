#pragma once

#include <chrono>

namespace ava::process {

// A nonpositive budget leaves the deadline unchanged. Positive budgets saturate
// rather than overflowing the steady clock's representation.
[[nodiscard]] constexpr std::chrono::steady_clock::time_point saturating_deadline_add(std::chrono::steady_clock::time_point value,
                                                                                      std::chrono::steady_clock::duration budget) noexcept
{
  if (budget <= std::chrono::steady_clock::duration::zero())
    return value;
  auto const maximum = std::chrono::steady_clock::time_point::max().time_since_epoch();
  if (value.time_since_epoch() > maximum - budget)
    return std::chrono::steady_clock::time_point::max();
  return value + budget;
}

}  // namespace ava::process
