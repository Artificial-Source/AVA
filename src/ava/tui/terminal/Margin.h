#pragma once

#include "src/ava/debug/print_members_on.h"

#include <cstdint>
#ifdef CWDEBUG
#include <iostream>
#endif

namespace ava::tui::terminal {

using columns_t = uint32_t;

// Struct Margin
//
// Aggregate containing the desired rows and columns of cell offset between a parent Window and a to be created subwindow.
//
//  Parent Window
//  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
//  ┃                top              ┃
//  ┃      ┏━━━━━━━━━━━━━━━━━━━━━━┓   ┃
//  ┃      ┃ subwindow            ┃ r ┃
//  ┃      ┃                      ┃ i ┃
//  ┃ left ┃                      ┃ g ┃
//  ┃      ┃                      ┃ h ┃
//  ┃      ┃                      ┃ t ┃
//  ┃      ┗━━━━━━━━━━━━━━━━━━━━━━┛   ┃
//  ┃               bottom            ┃
//  ┃                                 ┃
//  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
//
// Hence, the top-left of the subwindow will become (top, left) relative the to parent Window.
//
struct Margin
{
  uint8_t top = 0;
  uint8_t bottom = 0;
  uint8_t left = 0;
  uint8_t right = 0;

  uint32_t height() const { return top + bottom; }
  columns_t width() const { return left + right; }
  bool empty() const { return (top | bottom | left | right) == 0; }

  // We have a custom print_on.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

#ifdef CWDEBUG
  void print_on(std::ostream& os) const
  {
    os << "top:" << static_cast<uint32_t>(top)
       << ", bottom:" << static_cast<uint32_t>(bottom)
       << ", left:" << static_cast<uint32_t>(left)
       << ", right:" << static_cast<uint32_t>(right);
  }
#endif
};

} // namespace ava::tui::terminal
