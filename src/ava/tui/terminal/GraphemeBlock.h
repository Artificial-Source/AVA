#pragma once

#include "GraphemeSpan.h"
#include "utils/Vector.h"

namespace ava::tui::terminal {

// A GraphemeBlock is a non-empty vertical sequence of GraphemeSpan rows
// sharing the same max_columns_ (allocated layout width).
//
//                       max_columns_
//   ┊◄-----------------------------------------------►┊
//   ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┯━━━━┯━━┓
//   ┃             ⋯ GraphemeRun's ⋯           ╎ WS │🯟🯝┃                Each GraphemeSpan::columns_ can be less than max_columns_.
//   ┣━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┷━━┯━┷━━╉───────┐        Each GraphemeSpan may end on whitespace.
//   ┃             ⋯ GraphemeRun's ⋯              ╎ whitespace │        The whitespace might extend beyond max_columns_.
//   ┣━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┷━━━━╉───────┘
//   ┇                     ┊                           ┇
//   ┃               GraphemeSpan's                    ┃
//   ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
//
// The block retains views into its source TextSpan objects, so those objects must outlive
// the block and any GraphemeSurface containing it.

struct GraphemeBlockCategory
{
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
using GraphemeBlockIndex = utils::VectorIndex<GraphemeBlockCategory>;
using GraphemeBlock = utils::Vector<GraphemeSpan, GraphemeBlockIndex, core::Application::Vec8Alloc::rebind<GraphemeSpan>::other>;

inline uint32_t height_of(GraphemeBlock const& block)
{
  return static_cast<uint32_t>(block.size());
}
inline columns_t width_of(GraphemeBlock const& block)
{
  return block.front().max_columns();
}

} // namespace ava::tui::terminal
