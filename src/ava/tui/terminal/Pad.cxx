#include "sys.h"
#include "GraphemeSurface.h"
#include "Pad.h"
#include "ava/tui/config.h"

#include <iterator>
#include "debug.h"

namespace ava::tui::terminal {

void Pad::generate_grapheme_surface(columns_t columns, bool blank_line_between_block_rows)
{
  // Pass at least one terminal column so wrapping can always make progress.
  ASSERT(columns > 0);

  // Fit every HorizontalLayout first. GraphemeSurface records the widest resulting block row so
  // narrower rows can remain composed entirely of their real LayoutItem content.
  fitted_horizontal_layouts_.reset(horizontal_layouts_.size(), columns);
  for (HorizontalLayout const& horizontal_layout : horizontal_layouts_)
    fitted_horizontal_layouts_.append(horizontal_layout.create_grapheme_block_row(columns));

  // Initialize the number of content rows.
  content_rows_ = fitted_horizontal_layouts_.height() + (blank_line_between_block_rows ? std::max(1U, fitted_horizontal_layouts_.number_of_blocks_rows()) - 1 : 0);

  // Remember if content_rows_ includes blank lines between the block rows.
  blank_line_between_block_rows_ = blank_line_between_block_rows;

  // Mark up-to-date.
  fitted_horizontal_layouts_up_to_date_ = true;
}

void Pad::build()
{
  // Call generate_grapheme_surface before calling this function.
  ASSERT(fitted_horizontal_layouts_up_to_date_);

  // Determine some initial value for the height of the pad.
  uint32_t const pad_height = std::max(config::max_composer_viewport_height, content_rows_ + growth_slack_rows);

  // (Re)create the ncurses pad; the assignment destroys the previously generated pad, if any.
  pad_ = BasicWindow::newpad({pad_height, fitted_horizontal_layouts_.width()});

  // Existing Paragraph rows use their Paragraph default rendition, including alignment filler.
  // Standalone items, missing rows below shorter blocks, and space to the right of a narrower block
  // row use the rendition of the newly created pad.
  Rendition const pad_default_rendition = pad_.current_rendition();
  Dout(dc::notice, "pad_default_rendition = " << pad_default_rendition);
  uint32_t pad_row = 0;
  auto horizontal_layout = horizontal_layouts_.begin();
  auto const& block_rows = fitted_horizontal_layouts_.blocks_rows();
  for (auto block_row = block_rows.begin(); block_row != block_rows.end(); ++block_row, ++horizontal_layout)
  {
    auto const& blocks = block_row->blocks();
    auto const& layout_items = horizontal_layout->layout_items();
    // Every generated block corresponds to exactly one source LayoutItem in display order.
    ASSERT(blocks.size() == layout_items.size());

    for (uint32_t row_in_block = 0; row_in_block < block_row->height(); ++row_in_block)
    {
      pad_.move(Position{pad_row, 0});
      for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index)
      {
        GraphemeBlock const& block = blocks[block_index];
        GraphemeBlockIndex const grapheme_row{row_in_block};
        if (grapheme_row < block.iend())
        {
          Paragraph const* paragraph = dynamic_cast<Paragraph const*>(layout_items[block_index].get());
          Rendition const& default_rendition = paragraph ? paragraph->default_rendition() : pad_default_rendition;
          block[grapheme_row].write_to(pad_, default_rendition);
        }
        else
          pad_.addspaces(width_of(block), pad_default_rendition);
      }
      if (block_row->width() < fitted_horizontal_layouts_.width())
        pad_.addspaces(fitted_horizontal_layouts_.width() - block_row->width(), pad_default_rendition);
      ++pad_row;
    }

    if (blank_line_between_block_rows_)
      ++pad_row;
  }
}

void Pad::prefresh(Position pad_pos, Position viewport_pos, Dimension viewport_size)
{
  // Call `generate` before calling this function.
  ASSERT(pad_.is_initialized());
  pad_.prefresh(pad_pos, viewport_pos, viewport_size);
}

Dimension Pad::dimension() const
{
  // Call `generate` before calling this function.
  ASSERT(pad_.is_initialized());
  return pad_.getmaxyx();
}

BasicWindow& Pad::basic_window()
{
  // Call `generate` before calling this function.
  ASSERT(pad_.is_initialized());
  return pad_;
}

} // namespace ava::tui::terminal
