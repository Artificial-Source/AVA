#pragma once

#include "Dimension.h"
#include "GraphemeBlockRow.h"

namespace ava::tui::terminal {

// class GraphemeSurface
//
// A complete two-dimensional result ready to be written to a BasicWindow.
// The (required) size of window or pad can be obtained with `dimension`.
//
class GraphemeSurface
{
 public:
  using blocks_rows_type = std::vector<GraphemeBlockRow, core::Application::Vec8Alloc::rebind<GraphemeBlockRow>::other>;

 private:
  blocks_rows_type blocks_rows_;
  uint32_t height_{};                           // The height of the surface, in terminal rows.
  columns_t width_{};                           // The width of the widest block row, in terminal columns.

 public:
  // Construct an empty GraphemeSurface pre-allocating a capacity of `reserve_blocks` GraphemeBlockRow's.
  GraphemeSurface(std::size_t reserve_blocks = 0) : blocks_rows_(core::Application::instance().vec8alloc())
  {
    if (reserve_blocks != 0)
      blocks_rows_.reserve(core::Application::Vec8Alloc::optimal_capacity(reserve_blocks));
  }

  void reset(std::size_t reserve_blocks, columns_t width)
  {
    blocks_rows_.clear();
    std::size_t optimal_capacity = core::Application::Vec8Alloc::optimal_capacity(reserve_blocks);
    if (optimal_capacity > blocks_rows_.capacity())
      blocks_rows_.reserve(optimal_capacity);
    height_ = 0;
    width_ = width;
  }

  void append(GraphemeBlockRow&& block_row)
  {
    height_ += block_row.height();
    width_ = std::max(width_, block_row.width());
    blocks_rows_.emplace_back(std::move(block_row));
  }

  // Accessors

  blocks_rows_type const& blocks_rows() const { return blocks_rows_; }
  uint32_t height() const { return height_; }
  columns_t width() const { return width_; }

  // Convenience accessors.
  Dimension dimension() const { return {height_, width_}; }
  uint32_t number_of_blocks_rows() const { return static_cast<uint32_t>(blocks_rows_.size()); }

  columns_t width_last_line() const
  {
    columns_t width = 0;
    if (!blocks_rows_.empty())
    {
      GraphemeBlockRow const& last_block_row = blocks_rows_.back();
      width = last_block_row.width_last_line();
    }
    return width;
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
