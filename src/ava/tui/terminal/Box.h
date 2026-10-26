#pragma once

#include "ComplexChar.h"

#include <array>

namespace ava::tui::terminal {

// class Box
//
// While a real ncursesw border exists of eight cchar_t complex characters,
// we only store a single wchar_t here: combining characters are not possible.
// The Rendition has to be supplied separatedly.
//
// clang-format off
class Box
{
 public:
  // right      ⎞
  // |left      ⎟_ margin
  // ||top      ⎟
  // |||bottom  ⎠
  // ||||   .----- pos (index into box_characters_)
  // vvvv   v
  // 0000   0           none (space)
  // 0001   1   ━       bottom
  // 0010   2   ━       top
  // 0100   4   ┃       left
  // 1000   8   ┃       right
  // 0101   5   ┗       bottom-left
  // 0110   6   ┏       top-left
  // 1001   9   ┛       bottom-right
  // 1010  10   ┓       top-right
  static constexpr int no = 0;   // Index into box_characters_ for a nothing or space.
  static constexpr int bs = 1;   // Index into box_characters_ for the bottom side character of the border.
  static constexpr int ts = 2;   //               ,,                      top side              ,,
  static constexpr int ls = 4;   //               ,,                     left hand side         ,,
  static constexpr int rs = 8;   //               ,,                    right hand side         ,,
  static constexpr int bl = 5;   //               ,,                   bottom left              ,,
  static constexpr int tl = 6;   //               ,,                      top left              ,,
  static constexpr int br = 9;   //               ,,                   bottom right             ,,
  static constexpr int tr = 10;  //               ,,                      top right             ,,

  static constexpr std::array<int, 8> index_to_pos = { ls, rs, ts, bs, tl, tr, bl, br };

  // The inverse of index_to_pos.
  static consteval int pos_to_index(int pos)
  {
    for (int i = 0; i < index_to_pos.size(); ++i)
      if (index_to_pos[i] == pos)
        return i;
    throw "pos must be one of the eight Box border positions";
  }

  // Default string.
  static constexpr wchar_t const* default_box =
    L"┏━┓"    // Corresponds with indices into box_characters_: 012
     "┃ ┃"    //                                                3 5
     "┗━┛";   //                                                678

 private:
  std::array<wchar_t, 11> const box_characters_{};

 public:
  // Construct an empty Box whose character slots are all null.
  constexpr Box() = default;

  // Construct a Box from box, which must point to at least nine readable characters
  // arranged as a three-by-three border sample, including its center character.
  constexpr Box(wchar_t const* box) : box_characters_{box[4], box[7], box[1], wchar_t{}, box[3], box[6], box[0], wchar_t{}, box[5], box[8], box[2]} { }

  // Return the i-th border character as a ComplexChar using Rendition rendition,
  // where i = 0 corresponds to the left hand side, i = 1 corresponds to right hand side, etc.
  // (the same order as the elements of index_to_pos).
  ComplexChar get_complex_character(int pos, Rendition rendition) const
  {
    GraphemeCluster::Storage zero_terminated_character = { box_characters_[pos], L'\0' };
    return {GraphemeCluster{zero_terminated_character}, rendition};
  }

#ifdef CWDEBUG
  // Custom print_members.
  AVA_PRINT_ON_MEMBERS
#endif
};
// clang-format on

} // namespace ava::tui::terminal
