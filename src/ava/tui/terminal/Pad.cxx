#include "sys.h"
#include "GraphemeSurface.h"
#include "Pad.h"
#include "ava/tui/config.h"

#include <iterator>
#include "debug.h"

namespace ava::tui::terminal {

void Pad::generate_grapheme_surface(columns_t columns, bool blank_line_between_block_rows)
{
  DoutEntering(dc::notice, "Pad::generate_grapheme_surface(" << columns << ", " << blank_line_between_block_rows << ")");

  // Pass at least one terminal column so wrapping can always make progress.
  ASSERT(columns > 0);

  // Fit every HorizontalLayout first. GraphemeSurface records the widest resulting block row so
  // narrower rows can remain composed entirely of their real LayoutItem content.
  fitted_horizontal_layouts_.reset(horizontal_layouts_.size(), columns);
  for (HorizontalLayout const& horizontal_layout : horizontal_layouts_)
    fitted_horizontal_layouts_.append(horizontal_layout.create_grapheme_block_row(columns));

  // Initialize the number of content rows.
  content_rows_ = fitted_horizontal_layouts_.height() + (blank_line_between_block_rows ? std::max(1U, fitted_horizontal_layouts_.number_of_blocks_rows()) - 1 : 0);
  Dout(dc::notice, "content_rows_ set to " << content_rows_);

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
  GraphemeSurface::blocks_rows_type const& block_rows = fitted_horizontal_layouts_.blocks_rows();
  // Run over all GraphemeBlockRow's of the fitted_horizontal_layouts_ GraphemeSurface.
  for (auto block_row = block_rows.begin(); block_row != block_rows.end(); ++block_row, ++horizontal_layout)
  {
    GraphemeBlockRow::blocks_type const& blocks = block_row->blocks();
    HorizontalLayout::layout_items_type const& layout_items = horizontal_layout->layout_items();
    // Every generated block corresponds to exactly one source LayoutItem in display order.
    ASSERT(blocks.size() == layout_items.size());

    // Run vertically over all terminal rows in this block_row.
    for (uint32_t row_in_block = 0; row_in_block < block_row->height(); ++row_in_block)
    {
      bool const last_row = row_in_block == block_row->height() - 1;
      columns_t last_pad_col = -1;
      columns_t pad_col = 0;
      // Run horizontally over all GraphemeBlock's in the block_row.
      for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index)
      {
        if (pad_col != last_pad_col)
        {
          pad_.move(Position{pad_row, pad_col});
          last_pad_col = pad_col;
        }
        GraphemeBlock const& block = blocks[block_index];
        GraphemeBlockIndex const grapheme_row{row_in_block};
        // Is the height of this block large enough to have a terminal row at this grapheme_row?
        pad_col += width_of(block);
        if (grapheme_row < block.iend())
        {
          Paragraph const* paragraph = dynamic_cast<Paragraph const*>(layout_items[block_index].get());
          Rendition const& default_rendition = paragraph ? paragraph->default_rendition() : pad_default_rendition;
          bool const write_trailing_filler_spaces = !last_row || block_index != blocks.size() - 1;
          block[grapheme_row].write_to(pad_, default_rendition, write_trailing_filler_spaces);
          // The cursor is now at pad_col, unless write_trailing_filler_spaces is false, but then
          // we'll leave both loops anyway and won't be using last_pad_col anymore.
          last_pad_col = pad_col;
        }
      }
      //if (block_row->width() < fitted_horizontal_layouts_.width())
      //  pad_.addspaces(fitted_horizontal_layouts_.width() - block_row->width(), pad_default_rendition);
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
