#pragma once

#include "TuiTimerRegistry.h"
#include "ava/debug/print_members_on.h"

#include <optional>

namespace ava::tui {

// TuiTimeoutHandler
//
// Owns the timers of the TUI thread and receives the timeout events of its event loop.
//
// It tells the caller when it wants to be called back, without knowing how the caller waits for that moment.
class TuiTimeoutHandler
{
 private:
  // Declared first: every timer handle, including the one below, must die before the registry.
  TuiTimerRegistry timer_registry_;
  // Set by stop(); from then on there are no more deadlines and no timer runs.
  bool stopping_ = false;
  // Plain wake every 100 ms, once armed by arm_control_timer().
  TuiTimerHandle control_timer_;

 public:
  // Create the registry and a disarmed control timer.
  //
  // This touches no TUI state, so it may happen before the TUI thread exists.
  // The object is not movable: the timer callback and all timer handles hold its address.
  TuiTimeoutHandler();

  //===========================================================================
  // The member functions below may only be called by the tui-thread.
  // Therefore they must be non-const: otherwise they become publically accessible through TuiFrontEnd.

  // Return the registry on which the features create their timers.
  //
  // The returned reference stays valid for the lifetime of this object.
  [[nodiscard]] TuiTimerRegistry& timer_registry() { return timer_registry_; }

  // Start the 100 ms control timer, with its ticks counted from epoch.
  //
  // Give timers that should not cause extra wake-ups the same epoch.
  void arm_control_timer(TuiTimerRegistry::Clock::time_point epoch);

  // Stop running timers: next_deadline returns nothing and expire does nothing from now on.
  //
  // Timers can not be restarted afterwards. Calling this again has no effect.
  void stop() noexcept { stopping_ = true; }

  // Return when expire must be called next, or nothing when no timer is armed or after stop().
  //
  // A deadline in the past means that the caller should not sleep at all.
  [[nodiscard]] std::optional<TuiTimerRegistry::Clock::time_point> next_deadline() noexcept;

  // Run the callbacks of all timers that are due at now, unless stop() was called.
  void expire(TuiTimerRegistry::Clock::time_point now);

  // End of tui-thread only functions.
  //===========================================================================

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Callback of control_timer_; the work is in the wake-up that its deadline causes.
  void wake_for_control(TuiTimerRegistry::Clock::time_point now);
};

} // namespace ava::tui
