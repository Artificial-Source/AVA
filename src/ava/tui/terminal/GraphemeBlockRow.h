#pragma once

#include "GraphemeBlock.h"

namespace ava::tui::terminal {

// class GraphemeBlockRow
//
// A horizontal band in a GraphemeSurface. Its width is the sum of its horizontally stacked
// GraphemeBlock widths, and its height is the largest height of any block.
//
//                                   width_
// ┊◄----------------------------------------------------------------------►┊
// ┏━━━━━━━━━━━━━━━━━┯━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┯━━━━━━━━━━━━━━━━━━━━┓ ┄
// ┃                 │                                 │   GraphemeBlock 2  ┃ ▲
// ┃ GraphemeBlock 0 │                                 ├────────────────────┨ ┆
// ┃                 │         GraphemeBlock 1         │╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲┃ ┆ height_
// ┠─────────────────┤                                 │╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲┃ ┆
// ┃╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲│                                 │╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲╲┃ ▼
// ┗━━━━━━━━━━━━━━━━━┷━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┷━━━━━━━━━━━━━━━━━━━━┛ ┄
//
// This row of blocks usually fills the full width of the GraphemeSurface that it is a part of,
// but it can be narrower in which case the terminal cells on the right are left untouched.
//
class GraphemeBlockRow
{
 public:
  using blocks_type = std::vector<GraphemeBlock, core::Application::Vec8Alloc::rebind<GraphemeBlock>::other>;

 private:
  blocks_type blocks_;          // Horizontally stacked GraphemeBlock's spanning width_ terminal columns.
  uint32_t height_{};           // The height of the block row; the largest height of any block.
  columns_t width_{};           // The width of the block row, in terminal columns.

 public:
  // Construct an empty GraphemeBlockRow with a pre-allocated capacity for `reserved_blocks` GraphemeBlock's.
  GraphemeBlockRow(std::size_t reserved_blocks) : blocks_(core::Application::instance().vec8alloc())
  {
    blocks_.reserve(core::Application::Vec8Alloc::optimal_capacity(reserved_blocks));
  }

  // Construct a GraphemeBlockRow from one or more horizontally stacked GraphemeBlock's.
  GraphemeBlockRow(blocks_type&& blocks) : blocks_(std::move(blocks))
  {
    // Pass at least one GraphemeBlock so this GraphemeBlockRow has a known width.
    ASSERT(!blocks_.empty());
    for (GraphemeBlock const& block : blocks_)
    {
      height_ = std::max(height_, height_of(block));
      width_ += width_of(block);
    }
  }

  // Construct a GraphemeBlockRow that exists of a single GraphemeBlock.
  GraphemeBlockRow(GraphemeBlock&& block) : blocks_(core::Application::instance().vec8alloc())
  {
    append(std::move(block));
  }

  void append(GraphemeBlock&& block)
  {
    height_ = std::max(height_, height_of(block));
    width_ += width_of(block);
    blocks_.emplace_back(std::move(block));
  }

  // Accessors.

  blocks_type const& blocks() const { return blocks_; }
  uint32_t height() const { return height_; }
  columns_t width() const { return width_; }

  columns_t width_last_line() const
  {
    columns_t total_width = 0;
    columns_t width = 0;
    for (GraphemeBlock const& block : blocks_)
    {
      if (height_of(block) == height_)
        width = total_width + block.back().columns_excluding_trailing_whitespace();
      total_width += width_of(block);
    }
    return width;
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
