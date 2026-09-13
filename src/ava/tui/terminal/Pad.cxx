#include "sys.h"
#include "GraphemeSurface.h"
#include "Pad.h"
#include "ava/tui/config.h"

#include <iterator>
#include "debug.h"

namespace ava::tui::terminal {

GraphemeSurface Pad::generate_grapheme_surface(columns_t columns)
{
  // Pass at least one terminal column so wrapping can always make progress.
  ASSERT(columns > 0);
  // Append at least one HorizontalLayout before generating this Pad.
  ASSERT(!horizontal_layouts_.empty());

  // Fit every HorizontalLayout first. GraphemeSurface records the widest resulting block row so
  // narrower rows can remain composed entirely of their real LayoutItem content.
  GraphemeSurface fitted_horizontal_layouts(horizontal_layouts_.size());
  for (HorizontalLayout const& horizontal_layout : horizontal_layouts_)
    fitted_horizontal_layouts.append(horizontal_layout.create_grapheme_block_row(columns));
  return fitted_horizontal_layouts;
}

void Pad::generate(columns_t columns, bool blank_line_between_block_rows)
{
  GraphemeSurface surface = generate_grapheme_surface(columns);

  // Initialize the number of content rows.
  content_rows_ = surface.height() + (blank_line_between_block_rows ? surface.number_of_blocks_rows() - 1 : 0);

  // Determine some initial value for the height of the pad.
  uint32_t const pad_height = std::max(config::max_composer_viewport_height, content_rows_ + growth_slack_rows);

  // (Re)create the ncurses pad; the assignment destroys the previously generated pad, if any.
  pad_ = BasicWindow::newpad({pad_height, surface.width()});

  // Existing Paragraph rows use their Paragraph default rendition, including alignment filler.
  // Standalone items, missing rows below shorter blocks, and space to the right of a narrower block
  // row use the rendition of the newly created pad.
  Rendition const pad_default_rendition = pad_->current_rendition();
  Dout(dc::notice, "pad_default_rendition = " << pad_default_rendition);
  uint32_t pad_row = 0;
  auto horizontal_layout = horizontal_layouts_.begin();
  auto const& block_rows = surface.blocks_rows();
  for (auto block_row = block_rows.begin(); block_row != block_rows.end(); ++block_row, ++horizontal_layout)
  {
    auto const& blocks = block_row->blocks();
    auto const& layout_items = horizontal_layout->layout_items();
    // Every generated block corresponds to exactly one source LayoutItem in display order.
    ASSERT(blocks.size() == layout_items.size());

    for (uint32_t row_in_block = 0; row_in_block < block_row->height(); ++row_in_block)
    {
      pad_->move(Position{pad_row, 0});
      for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index)
      {
        GraphemeBlock const& block = blocks[block_index];
        GraphemeBlockIndex const grapheme_row{row_in_block};
        if (grapheme_row < block.iend())
        {
          Paragraph const* paragraph = dynamic_cast<Paragraph const*>(layout_items[block_index].get());
          Rendition const& default_rendition = paragraph ? paragraph->default_rendition() : pad_default_rendition;
          block[grapheme_row].write_to(*pad_, default_rendition);
        }
        else
          pad_->addspaces(width_of(block), pad_default_rendition);
      }
      if (block_row->width() < surface.width())
        pad_->addspaces(surface.width() - block_row->width(), pad_default_rendition);
      ++pad_row;
    }

    if (blank_line_between_block_rows)
      ++pad_row;
  }
}

void Pad::prefresh(Position pad_pos, Position screen_pos, Dimension viewport_size)
{
  // Call `generate` before calling this function.
  ASSERT(pad_.has_value());
  pad_->prefresh(pad_pos, screen_pos, viewport_size);
}

Dimension Pad::dimension() const
{
  // Call `generate` before calling this function.
  ASSERT(pad_.has_value());
  return pad_->getmaxyx();
}

BasicWindow& Pad::basic_window()
{
  // Call `generate` before calling this function.
  ASSERT(pad_.has_value());
  return *pad_;
}

} // namespace ava::tui::terminal
