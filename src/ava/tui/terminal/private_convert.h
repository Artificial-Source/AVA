// private_convert.h
//
// This header may *only* be included from terminal/*.cxx files.

#include "ComplexChar.h"

#define NCURSES_NOMACROS
#include <curses.h>

#undef getbegyx
#undef getmaxyx
#undef getparyx
#undef getsyx
#undef getyx
#undef setsyx
#undef L_ctermid
#undef L_cuserid
#undef L_tmpnam
#undef offsetof
#undef P_tmpdir
#undef stderr
#undef stdin
#undef stdout
#undef timeout

// Sanity check.
#if NCURSES_WIDECHAR != 1
#error "NCURSES_WIDECHAR is expected to be defined to 1."
#endif

using Attributes = ava::tui::terminal::Attributes;
using Attribute = ava::tui::terminal::Attribute;
using ComplexChar = ava::tui::terminal::ComplexChar;

attr_t convert_to_attr(Attributes attributes);
Attributes convert_to_Attributes(attr_t attributes);
cchar_t convert_to_cchar(ComplexChar const& complex_char);
ComplexChar convert_to_ComplexChar(cchar_t const& cchar);

namespace ava::tui::terminal {
struct ConvertToColorPair
{
  int extended_color_pair_;

  operator ColorPair() const
  {
    return ColorPair{{}, static_cast<uint32_t>(extended_color_pair_)};
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};
} // namespace ava::tui::terminal
