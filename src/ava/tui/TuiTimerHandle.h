// Removes a feature's timer from the registry when its owning handle is destroyed.
#pragma once

#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>

namespace ava::tui {

class TuiTimerRegistry;

// Identifies one TuiTimer in its TuiTimerRegistry; IDs are never reused.
using TuiTimerId = std::uint64_t;

// Move-only handle owned by the feature whose callback TuiTimerRegistry stores. All
// operations and destruction run on the TUI thread. The registry and stable callback
// target outlive the handle; an executing callback must also finish before its
// target dies. Destruction removes the timer, while disarm only clears its deadline.
// Moving transfers responsibility for removing the timer without changing its callback target.
class TuiTimerHandle final
{
 public:
  // Share the registry's monotonic time domain for every arm operation.
  using Clock = std::chrono::steady_clock;

  // TUI thread: create an empty handle; scheduling calls remain harmless no-ops.
  TuiTimerHandle() = default;
  // TUI thread before the registry dies: remove this timer, even if disarmed.
  // A callback already copied for its current invocation still has to return.
  ~TuiTimerHandle();
  // Not copyable: exactly one handle owns removal of each timer.
  TuiTimerHandle(TuiTimerHandle const&) = delete;
  TuiTimerHandle& operator=(TuiTimerHandle const&) = delete;
  // TUI thread: take other's timer and leave other empty; the callback's
  // target address and the requirement that its registry outlive this handle persist.
  TuiTimerHandle(TuiTimerHandle&& other) noexcept;
  // TUI thread with both registries alive: remove this handle's old timer, then
  // take other's. Self-assignment preserves it; callback targets are never moved.
  TuiTimerHandle& operator=(TuiTimerHandle&& other) noexcept;

  // TUI thread: disarm without releasing ownership; any due-work snapshot becomes
  // stale. Does not interrupt an already executing callback; empty is harmless.
  void disarm() noexcept;
  // TUI thread with the registry alive: report an armed deadline, not whether a callback
  // is currently running. Empty and disarmed handles report false.
  [[nodiscard]] bool armed() const noexcept;
  // TUI thread: replace any schedule with one shot. A past deadline is serviced by
  // a later service_due pass, never synchronously by this call; empty is harmless.
  void arm_at(Clock::time_point deadline);
  // TUI thread: schedule one shot relative to now; nonpositive delay means now,
  // overflow saturates, and no callback runs until dispatch. Empty is harmless.
  void arm_after(Clock::duration delay, Clock::time_point now = Clock::now());
  // TUI thread: require a positive interval and schedule the first tick after now.
  // Dispatch skips missed ticks while keeping the original cadence; empty is harmless.
  void arm_every(Clock::duration interval, Clock::time_point now = Clock::now());

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // The registry alone may manufacture a handle for an ID it already owns.
  friend class TuiTimerRegistry;
  // TUI thread after inserting timer_id: borrow timer_registry and assume exclusive responsibility
  // for removing that timer before registry destruction.
  TuiTimerHandle(TuiTimerRegistry& timer_registry, TuiTimerId timer_id) noexcept;
  // TUI thread with the borrowed registry alive: erase the timer then become
  // empty. Used by destruction/move assignment so neither leaves a stale callback.
  void release() noexcept;

  // Borrowed stable registry address, changed only by TUI-thread moves/release.
  // Null means empty; a nonnull pointer must stay valid until this timer is removed.
  TuiTimerRegistry* registry_ = nullptr;
  // Registry-assigned identity paired with registry_; TUI moves transfer it and release
  // clears it. Zero denotes an empty handle and is never a registered ID.
  TuiTimerId timer_id_ = 0;
};

} // namespace ava::tui
