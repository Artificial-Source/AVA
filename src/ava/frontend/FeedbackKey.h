#pragma once

#include "ava/debug/print_members_on.h"
#ifdef CWDEBUG
#include <iosfwd>
#endif

namespace ava::app::frontend {

enum class KeyType
{
  normal,               // Includes ctrl-<alphanumeric> key presses, '\t', '\e', '\n' (Enter).
#if 0 // Commented out for now: none of this is really necessary.
  function,             // A function key.
  backspace,
  del,
  home,
  end,
  insert,
  pgup,
  pgdwn,
  up,
  down,
  left,
  right
#endif
};

struct FeedbackKey
{
  KeyType type_;
  union {
    wchar_t wch;        // Valid iff type_ == normal.
    int fn;             // Function key number (1...12) iff type_ == function.
  } value_;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const;
#endif

  // struct has a custom print_on.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::app::frontend
