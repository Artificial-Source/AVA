#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <string>

namespace ava::frontend {

// A string as displayed on the UI.
// This type is used to store (private) state in snapshot objects, in the form of a display string,
// that are members of the final frontend class.
struct DisplayString
{
  ava::core::NoLogString display_;
  bool changed_{true};

  bool clear_changed()
  {
    bool changed = changed_;
    changed_ = false;
    return changed;
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::frontend
