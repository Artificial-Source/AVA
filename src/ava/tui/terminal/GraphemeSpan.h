#pragma once

#include "GraphemeRun.h"
#include "HorizontalAlignment.h"

#include <vector>

namespace ava::tui::terminal {

class BasicWindow;
class Rendition;

// class GraphemeSpan
//
// A series of a adjacent GraphemeRun's.
//
// A GraphemeSpan does not necessarily contain `max_columns_` wide characters,
// even if the last GraphemeRun ends on white-space (WS):
//
//                       max_columns_
//   ┊◄-----------------------------------------------►┊
//   ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┯━━━━┯━━┓
//   ┃                                         ╎ WS │🯟🯝┃
//   ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┷━━━━┷━━┛
//   ┊◄--------------------columns_----------------►┊
//   ┊◄-columns_excluding_trailing_whitespace_►┊
//
// It is also possible that the last GraphemeRun (or GraphemeRun's if the latter contain of only spaces)
// go beyond the end of the GraphemeSpan:
//
//                       max_columns_
//   ┊◄-----------------------------------------------►┊
//   ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┯━━━━┱───────┐
//   ┃                                            ╎ whitespace │
//   ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┷━━━━┹───────┘
//   ┊◄-------------------------columns_----------------------►┊
//   ┊◄--columns_excluding_trailing_whitespace_--►┊
//
// The `columns_` in the above examples are the concatenation of all the GraphemeRun::str_ wstring's.
// `columns_excluding_trailing_whitespace_` excludes only the trailing white-space from `str_` so right
// alignment can position the last visible grapheme independently of retained trailing spaces in
// GraphemeRun's.
//
class GraphemeSpan
{
  using grapheme_runs_type = std::vector<GraphemeRun, core::Application::Vec8Alloc::rebind<GraphemeRun>::other>;

 private:
  columns_t const max_columns_;                         // The maximum number of terminal columns in this row, ignoring trailing white-space.
  HorizontalAlignment const alignment_;                 // Where filler spaces need to go if max_columns_ is larger than the number of columns this span uses.
  bool const right_align_excluding_trailing_whitespace_;// Whether right alignment clips trailing whitespace part of the last GraphemeRun(s). True for Paragraph rows.
  columns_t columns_;                                   // The current number of terminal columns in this row, including trailing white-space.
  // Columns from the start through the last non-white-space grapheme, excluding only trailing white-space.
  columns_t columns_excluding_trailing_whitespace_;
  grapheme_runs_type grapheme_runs_;                    // A list of GraphemeRun's that make up the row.

 public:
  // Construct an empty GraphemeSpan with maximum width `max_columns`, horizontal `alignment`, and
  // optional right-alignment behavior that ignores retained trailing whitespace.
  GraphemeSpan(columns_t max_columns, HorizontalAlignment alignment)
      : max_columns_(max_columns),
        alignment_(alignment),
        right_align_excluding_trailing_whitespace_(true),
        columns_(0),
        columns_excluding_trailing_whitespace_(0),
        grapheme_runs_(core::Application::instance().vec8alloc())
  {
  }

  // Construct a GraphemeSpan from `source` clipped to `max_columns`.
  GraphemeSpan(TextSpan const& source, columns_t max_columns, HorizontalAlignment alignment);

  // Move constructor.
  GraphemeSpan(GraphemeSpan&& collector)
      : max_columns_(collector.max_columns_),
        alignment_(collector.alignment_),
        right_align_excluding_trailing_whitespace_(collector.right_align_excluding_trailing_whitespace_),
        columns_(collector.columns_),
        columns_excluding_trailing_whitespace_(collector.columns_excluding_trailing_whitespace_),
        grapheme_runs_(std::move(collector.grapheme_runs_))
  {
    // Reset the collector for further use(!) as if it was just constructed (empty).
    collector.columns_ = 0;
    collector.columns_excluding_trailing_whitespace_ = 0;
  }

  // Append as much of a GraphemeRun to the end as possible, returns what didn't fit.
  GraphemeRun append(GraphemeRun&& source);

  // Write the span to a BasicWindow at the current cursor position.
  void write_to(BasicWindow& basic_window, Rendition const& default_rendition, bool write_trailing_spaces) const;

  // Accessors.
  columns_t max_columns() const { return max_columns_; }
  columns_t columns() const { return columns_; }

  // Return the number of columns from the start through the final non-white-space grapheme, or zero for an all-white-space span.
  columns_t columns_excluding_trailing_whitespace() const { return columns_excluding_trailing_whitespace_; }
  grapheme_runs_type const& grapheme_runs() const { return grapheme_runs_; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
