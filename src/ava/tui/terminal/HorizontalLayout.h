#pragma once

#include "LayoutItem.h"

#include <vector>

namespace ava::tui::terminal {

class GraphemeBlockRow;

// class HorizontalLayout
//
// A container for adjacent LayoutItem's.
//
class HorizontalLayout
{
 public:
  using layout_items_type = std::vector<std::unique_ptr<LayoutItem>, core::Application::Vec8Alloc::rebind<std::unique_ptr<LayoutItem>>::other>;

 private:
  layout_items_type layout_items_;

 public:
  HorizontalLayout() : layout_items_(core::Application::instance().vec8alloc()) { }

  void append(std::unique_ptr<LayoutItem>&& layout_item) { layout_items_.push_back(std::move(layout_item)); }

  // Assign widths to all child items according to their sizing properties, so that the total width becomes `columns` terminal columns.
  // The assigned total can exceed `columns` when the sum of the child minimum widths is larger.
  void set_width(columns_t columns) const;

  // Fit this HorizontalLayout into `columns` terminal columns, returning the GraphemeBlockRow corresponding to that width.
  GraphemeBlockRow create_grapheme_block_row(columns_t columns) const;

  // Return the LayoutItem objects in their horizontal display order.
  layout_items_type const& layout_items() const { return layout_items_; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
