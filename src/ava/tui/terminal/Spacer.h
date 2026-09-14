#pragma once

#include "LayoutItem.h"

namespace ava::tui::terminal {

// class Spacer
//
// A LayoutItem that, like any LayItem, can be appended to a HorizontalLayout representing
// a number of spaces. The rendition used is ...
//
class Spacer final : public LayoutItem
{
 private:
  // Construct a Spacer.
  Spacer(LayoutItem::Properties const& layout_properties) : LayoutItem(layout_properties)
  {
    // Use an unbounded natural width, making it greedy. Such an item must have shrink priority zero.
    initialize_cached_natural_width(greedy);
    // Use a priority of 0 for a Spacer.
    ASSERT(shrink_priority() == 0);
  }

 public:
  static std::unique_ptr<Spacer> create(LayoutItem::Properties layout_properties = {.minimum_width = 1})
  {
    return std::unique_ptr<Spacer>(new Spacer(layout_properties));
  }

  // Convert this Spacer to one empty GraphemeSpan having its assigned width.
  GraphemeBlock create_grapheme_block() const override;

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(LayoutItem)
};

} // namespace ava::tui::terminal
