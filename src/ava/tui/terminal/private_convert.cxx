#include "sys.h"

#include <array>
#include <cwchar>
#include "debug.h"

// This header must be included last.
#include "private_convert.h"

namespace terminal = ava::tui::terminal;

// Translate public rendition attribute bits to native flags, omitting italic when ncurses lacks it.
attr_t convert_to_attr(Attributes attributes)
{
  auto const mask = attributes.mask();
  attr_t result = A_NORMAL;
  if ((mask & static_cast<attr_t>(Attribute::bold)))
    result |= A_BOLD;
  if ((mask & static_cast<attr_t>(Attribute::underline)))
    result |= A_UNDERLINE;
  if ((mask & static_cast<attr_t>(Attribute::standout)))
    result |= A_STANDOUT;
  if ((mask & static_cast<attr_t>(Attribute::blink)))
    result |= A_BLINK;
#ifdef A_ITALIC
  if ((mask & static_cast<attr_t>(Attribute::italic)))
    result |= A_ITALIC;
#endif
  if ((mask & static_cast<attr_t>(Attribute::reverse)))
    result |= A_REVERSE;
  return result;
}

// Convert the subset of ncurses attributes represented by terminal::Attributes back to the public terminal bitmask.
// Attribute bits that terminal::Attributes cannot represent are intentionally ignored; this keeps conversions stable
// when ncurses returns extra rendition bits that this wrapper does not model.
Attributes convert_to_Attributes(attr_t attributes)
{
  Attributes result;
  if ((attributes & A_BOLD))
    result |= Attribute::bold;
  if ((attributes & A_UNDERLINE))
    result |= Attribute::underline;
  if ((attributes & A_STANDOUT))
    result |= Attribute::standout;
  if ((attributes & A_BLINK))
    result |= Attribute::blink;
#ifdef A_ITALIC
  if ((attributes & A_ITALIC))
    result |= Attribute::italic;
#endif
  if ((attributes & A_REVERSE))
    result |= Attribute::reverse;
  return result;
}

cchar_t convert_to_cchar(ComplexChar const& complex_char)
{
  cchar_t result;
  terminal::GraphemeCluster const& grapheme_cluster = complex_char.cell_character();
  int color_pair_index = complex_char.rendition().color_pair().index();
  attr_t attributes = convert_to_attr(complex_char.rendition().attributes());
  if (grapheme_cluster.is_zero_terminated())
    ::setcchar(&result, complex_char.cell_character().data(), attributes, 0, &color_pair_index);
  else
  {
    // setcchar requires the grapheme to be zero terminated.
    std::array<wchar_t, CCHARW_MAX + 1> tmp;
    std::wmemcpy(tmp.data(), grapheme_cluster.data(), CCHARW_MAX);
    tmp[CCHARW_MAX] = L'\0';
    ::setcchar(&result, tmp.data(), attributes, 0, &color_pair_index);
  }
  return result;
}

ComplexChar convert_to_ComplexChar(cchar_t const& cchar)
{
  using GraphemeCluster = ava::tui::terminal::GraphemeCluster;

  attr_t attributes = A_NORMAL;
  NCURSES_PAIRS_T color_pair = 0;
  int extended_color_pair = 0;
  // We can not write directly into the Storage of result.cell_character() because the grapheme_cluster
  // returned by getcchar is null-terminated, even if it contains CCHARW_MAX non-zero characters!
  std::array<wchar_t, CCHARW_MAX + 1> grapheme_cluster_plus_one;
  [[maybe_unused]] int const status = ::getcchar(&cchar, grapheme_cluster_plus_one.data(), &attributes, &color_pair, &extended_color_pair);
  // getcchar returns ERR when cchar is not a wide complex character; call convert_to_ComplexChar only with a valid
  // cchar_t initialized by ncurses (for example via setcchar, convert_to_cchar, win_wch, or win_wchstr), using the
  // CCHARW_MAX + 1 buffer above.
  ASSERT(status == OK);
  // Copy the cluster into a temporary GraphemeCluster. This time, if it contains CCHARW_MAX non-zero characters, without null-termination.
  GraphemeCluster const grapheme_cluster(grapheme_cluster_plus_one.data());
  return {grapheme_cluster, ava::tui::terminal::ConvertToColorPair{extended_color_pair}, convert_to_Attributes(attributes)};
}
