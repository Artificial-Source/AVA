// Publishes animation phase changes through a loop timer; rendering stays in the
// session's paint stage rather than running inside a timer callback.
#pragma once

#include "TuiEventLoop.h"
#include "TuiTimer.h"
#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <functional>

namespace ava::tui {

// ActiveRun owns this TUI-thread animation state and its timer registration. The
// loop and publication target outlive this stationary object. All calls, publication
// and destruction run on the TUI thread, with no callback active during destruction.
// The implicit destructor retires timer_ before releasing its publication binding;
// stop disarms animation but keeps that registration available for another start.
class Spinner final
{
 public:
  // TUI thread with loop and publication target alive: bind a disarmed member timer.
  // publish_frame must be nonempty and only publish/invalidate; it must not paint,
  // poll input, service control, wait or dispatch timers recursively.
  Spinner(TuiEventLoop& loop, std::function<void(std::size_t)> publish_frame);
  // Copying would duplicate ownership of animation phase and its registration.
  Spinner(Spinner const&) = delete;
  // Preserve one animation owner instead of copying another owner's timer binding.
  Spinner& operator=(Spinner const&) = delete;
  // The timer borrows this address, so moving on any thread would leave a stale target.
  Spinner(Spinner&&) = delete;
  // Keep the registered callback target stationary throughout its lifetime.
  Spinner& operator=(Spinner&&) = delete;

  // TUI thread: resume from the caller's currently published phase and reset the
  // repeating schedule; the first new frame is published on a later timer tick.
  void start(std::size_t current_frame);
  // TUI thread: suppress future ticks without changing the last published phase.
  // A publication already executing must return before this owner can be destroyed.
  void stop();
  // TUI thread: report whether the animation has an armed deadline; does not advance it.
  [[nodiscard]] bool active() const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI timer stage only while this owner and its publication target live: advance
  // one phase per dispatched tick and publish it; missed intervals are coalesced.
  void tick(TuiEventLoop::Clock::time_point now);

  // Constructor-installed binding, invoked only by TUI timer ticks. Its borrowed
  // presentation target outlives this owner; destruction follows timer retirement.
  std::function<void(std::size_t)> publish_frame_;
  // TUI-thread phase seeded by start and incremented by tick; the consumer maps
  // this running counter to a glyph, so stopping preserves the current animation.
  std::size_t frame_ = 0;
  // TUI-thread RAII registration borrowing this owner; declared last so destruction
  // removes its callback before frame_ and publish_frame_ are released.
  TuiTimer timer_;
};

} // namespace ava::tui
