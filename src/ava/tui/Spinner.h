// Updates the displayed animation phase through a loop timer; rendering stays in the
// session's paint stage rather than running inside a timer callback.
#pragma once

#include "TuiEventLoop.h"
#include "TuiTimer.h"
#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <functional>

namespace ava::tui {

// ActiveRun owns this TUI-thread animation state and its timer registration. The
// loop and the target of publish_frame_ outlive this stationary object. All calls
// and destruction run on the TUI thread, with no callback active during destruction.
// The implicit destructor removes timer_'s registration before releasing publish_frame_;
// stop disarms animation but keeps that registration available for another start.
class Spinner final
{
 public:
  // TUI thread with loop and publish_frame's target alive: bind a disarmed member timer.
  // publish_frame must be nonempty and only update display data or request a frame;
  // it must not paint, poll input, run session control checks, wait or dispatch timers recursively.
  Spinner(TuiEventLoop& loop, std::function<void(std::size_t)> publish_frame);
  // Not copyable or movable: one spinner owns the animation and its timer borrows this address.
  Spinner(Spinner const&) = delete;
  Spinner& operator=(Spinner const&) = delete;
  Spinner(Spinner&&) = delete;
  Spinner& operator=(Spinner&&) = delete;

  // TUI thread: resume from the caller's current display phase and reset the
  // repeating schedule; the first new phase reaches publish_frame_ on a later tick.
  void start(std::size_t current_frame);
  // TUI thread: suppress future ticks without changing the last display phase.
  // Any executing publish_frame_ call must return before this owner can be destroyed.
  void stop();
  // TUI thread: report whether the animation has an armed deadline; does not advance it.
  [[nodiscard]] bool active() const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI timer stage while this owner and publish_frame_'s target live: advance
  // one phase per dispatched tick and pass it to publish_frame_; missed ticks are skipped.
  void tick(TuiEventLoop::Clock::time_point now);

  // Constructor-installed binding, invoked only by TUI timer ticks. Its borrowed
  // presentation target outlives this owner; the timer registration is removed first.
  std::function<void(std::size_t)> publish_frame_;
  // TUI-thread phase seeded by start and incremented by tick; the consumer maps
  // this running counter to a glyph, so stopping preserves the current animation.
  std::size_t frame_ = 0;
  // TUI-thread RAII registration borrowing this owner; declared last so destruction
  // removes its callback before frame_ and publish_frame_ are released.
  TuiTimer timer_;
};

} // namespace ava::tui
