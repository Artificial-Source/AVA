#pragma once

#include "Margin.h"
#include "ava/debug/print_members_on.h"

#include <cstdint>

namespace ava::tui::terminal {

struct PositionOffset
{
  uint32_t row_offset_;
  uint32_t col_offset_;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// class Position
//
// The coordinates of the top-left cell of a block of terminal cell-characters in (row, col).
// The top-left of the screen is (0, 0).
//
class Position
{
 private:
  uint32_t row_;        // The y-coordinate of the top-left cell.
  uint32_t col_;        // The x-coordinate of the top-left cell.

 public:
  // Construct an uninitialized Postion.
  Position() = default;

  // Construct a Position from a row and col.
  // Note that rows always come before colums; this is also how ncurses treats
  // coordinates although in that case they use (y, x) instead of (row, col).
  Position(uint32_t row, uint32_t col) : row_(row), col_(col) { }

  void advance_row() { ++row_; }

  // Accessors.
  uint32_t row() const { return row_; }
  uint32_t col() const { return col_; }

  Position operator+(Margin margin) const
  {
    return {row_ + margin.top, col_ + margin.left};
  }

  Position& operator+=(PositionOffset delta)
  {
    row_ += delta.row_offset_;
    col_ += delta.col_offset_;
    return *this;
  }

  Position& operator-=(PositionOffset delta)
  {
    row_ -= delta.row_offset_;
    col_ -= delta.col_offset_;
    return *this;
  }

  Position operator+(PositionOffset delta)
  {
    Position result(*this);
    result += delta;
    return result;
  }

  Position operator-(PositionOffset delta)
  {
    Position result(*this);
    result -= delta;
    return result;
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
