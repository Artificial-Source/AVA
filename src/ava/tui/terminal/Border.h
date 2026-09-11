#pragma once

#include "Margin.h"
#include "Rendition.h"
#include "Box.h"

namespace ava::tui::terminal {

// class Border
//
// Data required to draw a border.
//
class Border
{
 private:
  Margin margin_;                       // The margin to be used for this border; a size of 0 means that there is no border on that side!
  Box box_characters_;                  // The eight characters used for the border.
  std::array<ColorPair, 8> colors_;     // The eight colors to use for the border. Uses the same indexes as Box::index_to_pos.

 public:
  // Construct a non-existent Border.
  Border() : margin_{}, box_characters_{}, colors_{} { }

  // Construct a Border using `margin`, `border_color` and `box_characters`.
  Border(Margin margin,  ColorPair border_color, Box const& box_characters = {Box::default_box})
    : margin_(margin), box_characters_(box_characters),
      colors_{border_color, border_color, border_color, border_color, border_color, border_color, border_color, border_color} { }

  // Construct a Border using `margin`, `border_color`, `left_hand_color` and `box_characters`.
  Border(Margin margin,  ColorPair border_color, ColorPair left_hand_color, Box const& box_characters = {Box::default_box})
    : margin_(margin), box_characters_(box_characters),
      colors_{left_hand_color, border_color, border_color, border_color, left_hand_color, border_color, left_hand_color, border_color} { }

  // Accessors.
  Margin const& margin() const { return margin_; }
  Box const& box_characters() const { return box_characters_; }
  ColorPair const colors(int index) const { return colors_[index]; }

  // Convenience accessor.
  bool empty() const { return margin_.empty(); }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
