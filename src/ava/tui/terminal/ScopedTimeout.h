#pragma once

#include "ava/debug/print_members_on.h"
#include <chrono>

namespace ava::tui::terminal {

struct ScopedTimeout
{
  ScopedTimeout(std::chrono::milliseconds delay_ms);
  ~ScopedTimeout();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::tui::terminal
