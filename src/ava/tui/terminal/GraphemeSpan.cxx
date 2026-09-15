#include "sys.h"
#include "BasicWindow.h"
#include "GraphemeRun.h"
#include "GraphemeSpan.h"
#include "Rendition.h"
#include "TextSpan.h"
#include "utils/macros.h"

#include <iterator>
#include <stdexcept>

namespace ava::tui::terminal {

// Determine how much of `run` still fits on this GraphemeSpan.
//
// If everything fits, move-append `run` to grapheme_runs_ and return a default constructed GraphemeRun.
//
// For example if GraphemeSpan currently contains the GraphemeRun's "current ", "CONTENT" and " ":
//
//           <--------max_columns_------->
//  this:   |current CONTENT #            |
//           ''''''''^^^^^^^'                 <--
//  run:    |hello world|
//
//  result: |current CONTENT hello world# |
//           ''''''''^^^^^^^'^^^^^^^^^^^      <-- this line shows which characters belong to which GraphemeRun element (alternating ' and ~ characters).
//
// If the first word of `run` is longer than max_columns_, use it to fill the current GraphemeSpan by splitting
// that word only between compact grapheme clusters and return the remainder. A cluster wider than an otherwise
// empty row is appended whole, exceeding max_columns_, so wrapping can make progress without splitting it.
//
// For example,
//             <--------max_columns_------->
//  this:     |foo#                         |
//  run:      |AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA BBB|
//
//  result:   |fooAAAAAAAAAAAAAAAAAAAAAAAAAA|
//  returned: |AAAAAAAAAAA BBB|
//
// otherwise if nothing fits, return `run` itself.
//
// For example,
//             <--------max_columns_------->
//  this:     |foo#                         |
//  run:      |AAAAAAAAAAAAAAAAAAAAAAAAAAA BBB|
//
//  result:   |foo#                         |
//  returned: |AAAAAAAAAAAAAAAAAAAAAAAAAAA BBB|
//
// Otherwise, append all grapheme clusters up till the first white-space of `run` that still fit,
// plus any subsequent white-space even if they don't fit, as a single GraphemeRun to to this GraphemeSpan
// and return the remaining grapheme clusters as a GraphemeRun.
//
// For example,
//             <--------max_columns_------->
//  this:     |aaaaaaa bbbbbbb #            |
//  run:      |ccccc ddddd eeeeeee fffff ggggg hhhhh|
//
//  result:   |aaaaaaa bbbbbbb ccccc ddddd #|
//             ''''''''''''''''^^^^^^^^^^^^
//  returned: |eeeeeee fffff ggggg hhhhh|
//
//  or
//             <--------max_columns_------->
//  this:     |aaaaaaa bbbbbbb #            |
//  run:      |ccccc ddddddd    eeeeeee fffff ggggg hhhhh|
//
//  result:   |aaaaaaa bbbbbbb ccccc ddddddd|    #
//             ''''''''''''''''^^^^^^^^^^^^^^^^^^
//  returned: |eeeeeee fffff ggggg hhhhh|
//
GraphemeRun GraphemeSpan::append(GraphemeRun&& run)
{
  if (run.empty())
    // Pretend we succesfully appended `run`.
    return {};

  auto const run_begin = run.metadata().begin();
  auto const run_end = run.metadata().end();
  auto appended_end = run_begin;
  auto scan = run_begin;
  columns_t appended_columns = 0;

  // Leading whitespace remains trailing whitespace on this row until another
  // word is appended, so preserve all of it even when it crosses the limit.
  while (scan != run_end && scan->whitespace)
  {
    appended_columns += scan->columns;
    appended_end = ++scan;
  }

  bool first_word = true;
  while (scan != run_end)
  {
    auto const word_begin = scan;
    columns_t word_columns = 0;
    while (scan != run_end && !scan->whitespace)
    {
      word_columns += scan->columns;
      ++scan;
    }

    // Can we append this word too?
    if (columns_ + appended_columns + word_columns > max_columns_)
    {
      // Only an overlong first word may be split. A normally sized word that
      // does not fit in the remaining cells must start on the next row.
      if (first_word && word_columns > max_columns_)
      {
        scan = word_begin;
        while (scan != run_end && !scan->whitespace && columns_ + appended_columns <= max_columns_)
        {
          auto cluster_end = std::next(scan);
          columns_t cluster_width = scan->columns;
          while (cluster_end != run_end && cluster_end->combining)
          {
            cluster_width += cluster_end->columns;
            ++cluster_end;
          }

          if (columns_ + appended_columns + cluster_width > max_columns_)
          {
            // An indivisible cluster wider than an empty row must be kept whole so wrapping makes progress.
            if (columns_ == 0 && appended_columns == 0)
            {
              appended_columns = cluster_width;
              appended_end = cluster_end;
              scan = cluster_end;
            }
            break;
          }

          appended_columns += cluster_width;
          appended_end = cluster_end;
          scan = cluster_end;
        }
      }
      break;
    }

    appended_columns += word_columns;
    appended_end = scan;
    first_word = false;

    // Whitespace following an appended word stays on this row, including the
    // portion beyond max_columns_, because it is ignored for wrapping.
    while (scan != run_end && scan->whitespace)
    {
      appended_columns += scan->columns;
      appended_end = ++scan;
    }
  }

  if (appended_end == run_begin)
    return std::move(run);

  // Update columns_excluding_trailing_whitespace_ by adding all appended columns through the last non-whitespace.
  columns_t column_count = 0;
  for (auto iter = run_begin; iter != appended_end; ++iter)
  {
    column_count += iter->columns;
    if (!iter->whitespace)
      columns_excluding_trailing_whitespace_ = columns_ + column_count;
  }

  if (appended_end == run_end)
  {
    columns_ += appended_columns;
    grapheme_runs_.push_back(std::move(run));
    return {};
  }

  // The appended run takes the first `appended_character_count` entries of both parallel containers;
  // wide_[N] corresponds to metadata_[N].
  std::size_t const appended_character_count = static_cast<std::size_t>(appended_end - run_begin);

  GraphemeRun appended_run;
  appended_run.text_span_ = run.text_span_;
  appended_run.copy_prefix(run, appended_character_count);
  run.remove_prefix(appended_character_count);

  columns_ += appended_columns;
  grapheme_runs_.push_back(std::move(appended_run));
  return std::move(run);
}

GraphemeSpan::GraphemeSpan(TextSpan const& source, columns_t max_columns, HorizontalAlignment alignment)
    : max_columns_(max_columns),
      alignment_(alignment),
      right_align_excluding_trailing_whitespace_(false),
      columns_(0),
      columns_excluding_trailing_whitespace_(0),
      grapheme_runs_(core::Application::instance().vec8alloc())
{
  try
  {
    // The GraphemeSpan exists of a single (possibly clipped) GraphemeRun.
    grapheme_runs_.emplace_back(source, max_columns);
  }
  catch (std::runtime_error const& exception)
  {
    Dout(dc::warning, "Unable to render a TextSpan as a GraphemeRun; treating it as empty: " << exception.what());
    return;
  }
  auto [columns, columns_excluding_trailing_whitespace] = grapheme_runs_.back().get_columns();
  columns_ = columns;
  columns_excluding_trailing_whitespace_ = columns_excluding_trailing_whitespace;
}

// Write one GraphemeSpan into the ncurses handle `basic_window` at the current cursor position.
//
// Every addstr call continues right after where the previous one ended; the GraphemeRun's are simply concatenated.
// Each GraphemeRun is written with the rendition of its parent TextSpan, or with `default_rendition` when that
// TextSpan was created without a rendition of its own. The rendition is only passed to ncurses when it changes,
// and is restored at the end.
//
// Wrapping (for example of a Paragraph) may have kept trailing white-space past the end of the GraphemeSpan.
// Trailing white-space is still written - it carries the background color of its rendition - but is clipped
// at `max_columns_` terminal columns. A GraphemeSpan can only go beyond its `max_columns_` with spaces,
// therefore only spaces might get clipped. Retained trailing white-space is written with its source rendition
// when it falls inside `max_columns_`, while extra filler space is written using the default rendition,
// unless write_trailing_filler_spaces is false, which should only be the case for the last GraphemeSpan of
// a Paragraph.
//
// In the case of right alignment the width through the last non-white-space grapheme is used, so no trailing
// white-space is visible.
//
// Rows are extended to `max_columns_` terminal columns using spaces with `default_rendition`, so that the
// background of the whole row equals the paragraph background. A row that is written to the very last row
// of the basic_window (i.e. an ncurses window, subwindow or pad) can end exactly at the bottom-right corner.
// That is the benign ncurses corner case tolerated by BasicWindow::addstr (even though ncurses `addstr` returns
// ERR because it can't advance the cursor).
//
void GraphemeSpan::write_to(BasicWindow& basic_window, Rendition const& default_rendition, bool write_trailing_filler_spaces) const
{
  DoutEntering(dc::terminal, "GraphemeSpan::write_to(" << basic_window << ", " << default_rendition << ", " << write_trailing_filler_spaces << ")");
  Dout(dc::terminal, "Contents of this GraphemeSpan: " << *this);

  // Track the rendition that ncurses would still use, starting from the current one,
  // so that an unchanged rendition never needs an attr_set call.
  Rendition original_rendition = basic_window.current_rendition();

  std::vector<std::size_t> per_grapheme_run_wide_character_count;
  per_grapheme_run_wide_character_count.reserve(grapheme_runs_.size());

  std::size_t leading_spaces = 0;
  if (alignment_ == HorizontalAlignment::right)
  {
    columns_t const aligned_columns = right_align_excluding_trailing_whitespace_ ? columns_excluding_trailing_whitespace_ : columns_;
    leading_spaces = aligned_columns < max_columns_ ? max_columns_ - aligned_columns : 0;
  }

  bool row_full = false;          // Set once a character was clipped; from then on only white-space may follow.
  columns_t remaining_columns = max_columns_ - leading_spaces;
  for (GraphemeRun const& grapheme_run : grapheme_runs_)
  {
    // An empty GraphemeRun has nothing to write; just go to the next one.
    if (grapheme_run.empty())
      continue;

    // Determine how many wide characters of this grapheme_run fit (also) on the basic_window row.
    std::size_t wide_character_count = 0;
    for (auto const& character_metadata : grapheme_run.metadata())
    {
      if (!row_full && character_metadata.columns <= remaining_columns)
      {
        remaining_columns -= character_metadata.columns;
        ++wide_character_count;
      }
      else
      {
        // Only white-space may exceed max_columns_. This should have been enforced by Paragraph::wrap.
        ASSERT(character_metadata.whitespace);
        // This should be just a space.
        ASSERT(character_metadata.columns == 1);
        // This follows because the first time we get here, a one-column character does not fit in remaining_columns.
        ASSERT(remaining_columns == 0);
        row_full = true;
      }
    }
    per_grapheme_run_wide_character_count.push_back(wide_character_count);

    // If the row is full then we expect any additional characters, of subsequent GraphemeRun's if any, to be whitespace.
    if (row_full)
      break;
  }

  // Preserve centered alignment's existing treatment of trailing white-space: center the columns that were actually retained.
  if (alignment_ == HorizontalAlignment::centered)
  {
    leading_spaces = remaining_columns / 2;
    remaining_columns -= leading_spaces;
  }

  if (leading_spaces > 0)
    basic_window.addspaces(leading_spaces, default_rendition);

  std::size_t gr = 0;
  for (GraphemeRun const& grapheme_run : grapheme_runs_)
  {
    // An empty GraphemeRun has nothing to write; just go to the next one.
    if (grapheme_run.empty())
      continue;

    // Since grapheme_run is not empty there was at least one element in grapheme_run.characters_meta(),
    // therefore now either count is larger than 0 or row_full is true, or both.
    //
    // If count is zero then no character in this grapheme_run did fit on this row and all characters
    // truncated were whitespace (see the above ASSERT).
    if (per_grapheme_run_wide_character_count[gr] > 0)
    {
      TextSpan const* text_span = grapheme_run.text_span();
      Rendition const required_rendition = text_span->use_default_rendition() ? default_rendition : text_span->rendition();
      basic_window.addstr(grapheme_run.str().data(), static_cast<int>(per_grapheme_run_wide_character_count[gr]), required_rendition);
    }

    // If the row is full then we expect any additional characters, of subsequent GraphemeRun's if any, to be whitespace.
    if (++gr == per_grapheme_run_wide_character_count.size())
      break;
  }

  // Write the trailing spaces, if any.
  if (write_trailing_filler_spaces && remaining_columns > 0)
    basic_window.addspaces(remaining_columns, default_rendition);

  // Restore the rendition that the basic_window had before writing this row.
  basic_window.restore_rendition(original_rendition);
}

} // namespace ava::tui::terminal
