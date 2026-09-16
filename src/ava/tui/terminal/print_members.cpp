#include "sys.h"
// clang-format off
#include "ava/debug/debug_ostream_operators.h"  // Must be included immediately after "sys.h".
// clang-format on
#include "BasicWindow.h"
#include "Box.h"
#include "GraphemeCluster.h"
#include "GraphemeRun.h"
#include "LayoutItem.h"
#include "TextSpan.h"
#include "utils/debug_ostream_operators.h"
#include "utils/print_pointer.h"

namespace ava::tui::terminal {

void BasicWindow::print_members(std::ostream& os, char const* prefix) const
{
  os << prefix;
//  impl_->print_members(os, "impl_->");
}

void Box::print_members(std::ostream& os, char const* prefix) const
{
  os << prefix;
  AVA_USING_OSTREAM_PRELUDE(os)
     << __write__("ls:") << ls
     << __write__(", rs:") << rs
     << __write__(", ts:") << ts
     << __write__(", bs:") << bs
     << __write__(", tl:") << tl
     << __write__(", tr:") << tr
     << __write__(", bl:") << bl
     << __write__(", br:") << br
     << __write__(", index_to_pos:") << index_to_pos
     << __write__(", default_box")                              // We are not showing `default_box`.
     << __write__(", box_characters:") << box_characters_;
}

void GraphemeCluster::print_members(std::ostream& os, char const* prefix) const
{
  std::wstring_view const storage_view(storage_);

  os << prefix;
  AVA_USING_OSTREAM_PRELUDE(os)
     << __write__("space")                                      // We are not printing `space_`.
     << __write__(", capacity:") << capacity
     << __write__(", storage:") << storage_view;
}

std::u8string_view GraphemeRun::get_u8string_view() const
{
  // Don't call this on an empty GraphemeRun.
  ASSERT(!empty());
  return {&text_span_->text()[0] + metadata_.front().utf8_begin, utf8_size()};
}

void GraphemeRun::print_on(std::ostream& os) const
{
  LIBCWD_USING_OSTREAM_PRELUDE;
  os << "{text_span:" << print_pointer(text_span_) <<
     ", characters_:" << get_u8string_view() <<
     ", metadata:{";
  char const* separator = "";
  for (Metadata const& metadata : metadata_)
  {
    os << separator << '{';
    os << std::u8string_view{&text_span_->text()[0] + metadata.utf8_begin, metadata.utf8_size};
    os << ", columns:" << metadata.columns;
    if (metadata.utf8_size != 1)
      os << ", utf8_size:" << static_cast<unsigned int>(metadata.utf8_size);
    if (metadata.whitespace)
      os << " (WS)";
    else if (metadata.combining)
      os << " (combining)";
    os << '}';
    separator = ", ";
  }
  os << "}}";
}

void Width::print_on(std::ostream& os) const
{
  os << "{columns:";
  if (columns_ == unknown)
    os << "<unknown>";
  else if (columns_ == unlimited)
    os << "<unlimited>";
  else
    os << columns_;
  os << '}';
}

} // namespace ava::tui::terminal
