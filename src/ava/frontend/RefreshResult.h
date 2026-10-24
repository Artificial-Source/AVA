#pragma once

#include "ava/debug/print_members_on.h"

namespace ava::frontend {

// Return a refreshed projection value together with its independently consumed change notification.
template <typename T>
struct RefreshResult
{
  T value;
  bool changed;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::frontend
