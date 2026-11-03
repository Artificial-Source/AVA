#pragma once

#include "TuiEventLoop.h"
#include "TuiTimer.h"
#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <functional>

namespace ava::tui {

// Owns animation phase and its registration. The loop and the bound publication
// target outlive this nonmoving owner; publication runs only on the TUI thread.
class Spinner final
{
 public:
  Spinner(TuiEventLoop& loop, std::function<void(std::size_t)> publish_frame);
  Spinner(Spinner const&) = delete;
  Spinner& operator=(Spinner const&) = delete;
  Spinner(Spinner&&) = delete;
  Spinner& operator=(Spinner&&) = delete;

  void start(std::size_t current_frame);
  void stop();
  [[nodiscard]] bool active() const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  void tick(TuiEventLoop::Clock::time_point now);

  std::function<void(std::size_t)> publish_frame_;
  std::size_t frame_ = 0;
  // Registration retires before the callback binding above is destroyed.
  TuiTimer timer_;
};

} // namespace ava::tui
