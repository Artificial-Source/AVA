// Removes a feature's event-loop timer registration when its owning handle is destroyed.
#pragma once

#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>

namespace ava::tui {

class TuiEventLoop;

// Move-only handle owned by the feature whose callback TuiEventLoop stores. All
// operations and destruction run on the TUI thread. The loop and stable callback
// target outlive the handle; an executing callback must also finish before its
// target dies. Destruction removes the registration, while cancel only disarms it.
// Moving transfers responsibility for removing the registration without changing its callback target.
class TuiTimer final
{
 public:
  // Share the registry's monotonic time domain for every arm and repeat operation.
  using Clock = std::chrono::steady_clock;

  // TUI thread: create an empty handle; scheduling calls remain harmless no-ops.
  TuiTimer() = default;
  // TUI thread before the loop dies: remove this registration, even if disarmed.
  // A callback already copied for its current invocation still has to return.
  ~TuiTimer();
  // Not copyable: exactly one handle owns removal of each registration.
  TuiTimer(TuiTimer const&) = delete;
  TuiTimer& operator=(TuiTimer const&) = delete;
  // TUI thread: take other's registration and leave other empty; the callback's
  // target address and the requirement that its loop outlive this handle persist.
  TuiTimer(TuiTimer&& other) noexcept;
  // TUI thread with both loops alive: remove this handle's old registration, then
  // take other's. Self-assignment preserves it; callback targets are never moved.
  TuiTimer& operator=(TuiTimer&& other) noexcept;

  // TUI thread: disarm without releasing ownership; any due-work snapshot becomes
  // stale. Does not interrupt an already executing callback; empty is harmless.
  void cancel() noexcept;
  // TUI thread with the loop alive: report an armed deadline, not whether a callback
  // is currently running. Empty and disarmed handles report false.
  [[nodiscard]] bool active() const noexcept;
  // TUI thread: replace any schedule with one shot. A past deadline is serviced by
  // a later service_due pass, never synchronously by this call; empty is harmless.
  void arm_at(Clock::time_point deadline);
  // TUI thread: schedule one shot relative to now; nonpositive delay means now,
  // overflow saturates, and no callback runs until dispatch. Empty is harmless.
  void arm_after(Clock::duration delay, Clock::time_point now = Clock::now());
  // TUI thread: require a positive interval and schedule the first tick after now.
  // Dispatch skips missed ticks while keeping the original cadence; empty is harmless.
  void repeat(Clock::duration interval, Clock::time_point now = Clock::now());

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // The registry alone may manufacture a handle for an ID it already owns.
  friend class TuiEventLoop;
  // TUI thread after inserting id: borrow loop and assume exclusive responsibility
  // for removing that registration before loop destruction.
  TuiTimer(TuiEventLoop& loop, std::uint64_t id) noexcept;
  // TUI thread with the borrowed loop alive: erase the registration then become
  // empty. Used by destruction/move assignment so neither leaves a stale callback.
  void release() noexcept;

  // Borrowed stable registry address, changed only by TUI-thread moves/release.
  // Null means empty; a nonnull pointer must stay valid until this registration is removed.
  TuiEventLoop* loop_ = nullptr;
  // Registry-assigned identity paired with loop_; TUI moves transfer it and release
  // clears it. Zero denotes an empty handle and is never a registered ID.
  std::uint64_t id_ = 0;
};

} // namespace ava::tui
