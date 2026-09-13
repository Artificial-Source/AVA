#pragma once

#include "ava/debug/print_members_on.h"

#include <cstdint>
#include <limits>
#include "debug.h"      // ASSERT

namespace ava::tui::terminal {

// class Color
//
// A wrapper for the direct-color RGB terminal values.
// Unfortunately, it depends on the terminal how such a color is displayed,
// as well as any Attributes that are in effect of course.
//
// On direct-color terminals (where COLORS is 2^24) the rgb_ value will be
// used as index, and the values 1 through 7 have a special meaning (COLOR_RED
// through COLOR_WHITE).
//
// Because of that, to avoid confusion, this class will not store such values,
// but instead replaces those with 0 (black).
//
class Color
{
 private:
  static constexpr uint32_t default_terminal_color = std::numeric_limits<std::uint32_t>::max();

  uint32_t rgb_;

 public:
  // Construct a color meaning "the default terminal color" (for either foreground or background).
  Color() : rgb_(default_terminal_color) { }

  // Construct a Color by RGB value (0x000000 (black) till 0xffffff (white)).
  constexpr Color(uint32_t rgb) : rgb_(rgb)
  {
    if (rgb_ < 8)
      rgb_ = 0;

    // The caller passed an RGB value with bits set above bit 23; construct Color only from a 24-bit RGB value
    // (0x000000 black through 0xffffff white).
    ASSERT(rgb_ < 0x1000000);
  }

  bool is_default() const { return rgb_ == default_terminal_color; }
  int as_int() const { return static_cast<int>(static_cast<int32_t>(rgb_)); }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
