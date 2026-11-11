// Removes a feature's timer from the registry when its owning handle is destroyed.
#pragma once

#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>

namespace ava::tui {

class TuiTimeoutHandler;

// Identifies one TuiTimer in its TuiTimeoutHandler; IDs are never reused.
using TuiTimerId = std::uint64_t;

// Move-only handle owned by the feature whose callback TuiTimeoutHandler stores.
//
// Any thread can create a handle, through TuiTimeoutHandler::create_timer, and destroy it.
// The owner may also arm and disarm it on any thread: arming on another thread wakes the TUI thread
// when the new deadline is earlier than the one it sleeps until. See disarm for what disarming
// on another thread does and does not guarantee.
// A single handle is not thread-safe: it has one owner at a time.
//
// The registry and stable callback target outlive the handle; an executing callback must
// also finish before its target dies, which matters when destroying an armed handle on
// another thread. Destruction removes the timer, while disarm only clears its deadline.
// Moving transfers responsibility for removing the timer without changing its callback target.
class TuiTimerHandle
{
 private:
  // Borrowed stable registry address, changed only by moves/release.
  // Null means empty; a nonnull pointer must stay valid until this timer is removed.
  TuiTimeoutHandler const* registry_ = nullptr;
  // Registry-assigned identity paired with registry_; moves transfer it and release
  // clears it. Zero denotes an empty handle and is never a registered ID.
  TuiTimerId timer_id_ = 0;

 public:
  // Share the registry's monotonic time domain for every arm operation.
  using Clock = std::chrono::steady_clock;

  // Create an empty handle; scheduling calls remain harmless no-ops.
  TuiTimerHandle() = default;
  // Before the registry dies: remove this timer, even if disarmed.
  // A callback already copied for its current invocation still has to return.
  ~TuiTimerHandle();
  // Not copyable: exactly one handle owns removal of each timer.
  TuiTimerHandle(TuiTimerHandle const&) = delete;
  TuiTimerHandle& operator=(TuiTimerHandle const&) = delete;
  // Take other's timer and leave other empty; the callback's
  // target address and the requirement that its registry outlive this handle persist.
  TuiTimerHandle(TuiTimerHandle&& other) noexcept;
  // With both registries alive: remove this handle's old timer, then
  // take other's. Self-assignment preserves it; callback targets are never moved.
  TuiTimerHandle& operator=(TuiTimerHandle&& other) noexcept;

  // Disarm without releasing ownership; any due-work snapshot becomes stale. Empty is harmless.
  //
  // May be called on any thread by the owner of this handle; no wakeup is needed, because
  // a removed deadline at most causes one wakeup in which nothing is due.
  // Does not interrupt an already executing callback. On the TUI thread that means the callback
  // will not run anymore once this returns. On another thread the callback may still be running,
  // or start once more, if expire already took it out before this call: its target must stay alive
  // until any such invocation returns.
  void disarm() noexcept;
  // Report an armed deadline, not whether a callback is currently running.
  // Empty and disarmed handles report false.
  //
  // Not const, because it is not thread-safe: it reads this handle's members, which its owner may change.
  // On another thread than the TUI thread the answer may already be outdated when it is returned:
  // `expire` disarms a one-shot timer when it runs it.
  [[nodiscard]] bool armed() noexcept;
  // Replace any schedule with one shot. A past deadline is serviced by
  // a later `expire` pass, never synchronously by this call; empty is harmless.
  void arm_at(Clock::time_point deadline);
  // Schedule one shot relative to now; nonpositive delay means now,
  // overflow saturates, and no callback runs until dispatch. Empty is harmless.
  void arm_after(Clock::duration delay, Clock::time_point now = Clock::now());
  // Require a positive interval and schedule the first tick after now.
  // Dispatch skips missed ticks while keeping the original cadence; empty is harmless.
  void arm_every(Clock::duration interval, Clock::time_point now = Clock::now());

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // The registry alone may manufacture a handle for an ID it already owns.
  friend class TuiTimeoutHandler;
  // After inserting timer_id: borrow tui_timeout_handler and assume exclusive responsibility
  // for removing that timer before registry destruction.
  TuiTimerHandle(TuiTimeoutHandler const& tui_timeout_handler, TuiTimerId timer_id) noexcept;
  // With the borrowed registry alive: erase the timer then become
  // empty. Used by destruction/move assignment so neither leaves a stale callback.
  void release() noexcept;
};

} // namespace ava::tui
