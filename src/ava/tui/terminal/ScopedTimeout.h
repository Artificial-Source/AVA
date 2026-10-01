#pragma once

#include <chrono>

namespace ava::tui::terminal {

struct ScopedTimeout
{
  ScopedTimeout(std::chrono::milliseconds delay_ms);
  ~ScopedTimeout();
};

} // namespace ava::tui::terminal
