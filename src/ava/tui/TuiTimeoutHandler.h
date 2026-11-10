#pragma once

#include "TuiTimer.h"
#include "TuiTimerHandle.h"                     // TuiTimerHandle, TuiTimerId
#include "ava/debug/print_members_on.h"
#include "threadsafe/threadsafe.h"

#include <atomic>
#include <functional>
#include <map>
#include <optional>

namespace ava::tui {

// TuiTimeoutHandler
//
// Registry of feature timers on the TUI thread; TuiFrontEnd::run asks it when to wake up next.
// See docs/engineering/tui-architecture.md for ownership.
//
// It tells the caller of `next_deadline` when it wants to be called back.
//
// All const member functions are thread-safe, so any thread that has a TuiTimeoutHandler const& can
// create a disarmed timer; the only non-const member, `expire`, is TUI-thread-only.
//
// Handles returned by `create_timer`, including disarmed ones, must die before this registry;
// callback Target's must stay at stable addresses until their timers are removed and active calls end.
//
class TuiTimeoutHandler
{
 public:
  // Use monotonic deadlines throughout the TUI so wall-clock changes cannot reschedule work.
  using Clock = std::chrono::steady_clock;
  using timers_ts = threadsafe::Unlocked<std::map<TuiTimerId, TuiTimer>, threadsafe::policy::Primitive<std::mutex>>;

 private:
  //---------------------------------------------------------------------------
  // These members may be accessed by any thread.

  // All timers, with entries created and removed through TuiTimerHandle handles.
  // Mutable because it is thread-safe. Callbacks are copied out and invoked without
  // holding its lock, so that they may create, arm or remove timers.
  mutable timers_ts timers_;
  // Assigns increasing IDs and rejects exhaustion to avoid confusing a
  // removed timer or dispatch snapshot with a later timer. Mutable because it is thread-safe.
  mutable std::atomic<TuiTimerId> next_timer_id_{1};

  //---------------------------------------------------------------------------
  // These members may only be accessed by the TUI thread.

#if CW_DEBUG
  // TUI thread holds this through `expire`, including callbacks; rejects recursive
  // dispatch and is cleared on both normal return and exception propagation.
  bool inside_expire_ = false;
#endif

  // Set by `stop`; from then on there are no more deadlines and no timer runs.
  bool stopping_ = false;

 public:
  // Use monotonic deadlines throughout the TUI so wall-clock changes cannot reschedule work.
  // Create an empty registry; this may happen before the TUI thread exists.
  TuiTimeoutHandler() = default;
  // Outside dispatch and after every timer handle is destroyed.
  ~TuiTimeoutHandler();
  // Not copyable or movable: timer handles borrow this registry at a stable address.
  TuiTimeoutHandler(TuiTimeoutHandler const&) = delete;
  TuiTimeoutHandler& operator=(TuiTimeoutHandler const&) = delete;
  TuiTimeoutHandler(TuiTimeoutHandler&&) = delete;
  TuiTimeoutHandler& operator=(TuiTimeoutHandler&&) = delete;

  // Thread-safe: register an initially disarmed member callback of target and return the handle that owns it.
  //
  // target must stay at this address until the timer is removed and any executing callback returns.
  // Callbacks run on the TUI thread. They may update feature state or request a frame, but must not
  // paint, poll input, run session control checks, recursively dispatch timers or wait.
  template <typename Target>
  [[nodiscard]] TuiTimerHandle create_timer(Target& target, void (Target::*callback)(Clock::time_point)) const
  {
    return register_timer(std::bind_front(callback, &target));
  }

  //===========================================================================
  // The member functions below may only be called by the tui-thread.
  // Therefore they must be non-const: otherwise they become publically accessible through TuiFrontEnd.

  // Stop running timers: next_deadline returns nothing and `expire` does nothing from now on.
  //
  // Timers can not be restarted afterwards. Calling this again has no effect.
  void stop() noexcept { stopping_ = true; }

  // Return when `expire` must be called next, or nothing when no timer is armed or after `stop`.
  //
  // A deadline in the past means that the caller should not sleep at all.
  [[nodiscard]] std::optional<Clock::time_point> next_deadline() noexcept;

  // Run the callbacks of all timers that are due at now, unless `stop` was called.
  //
  // TUI thread, never recursively: snapshot due IDs and generations, then invoke each
  // still-current callback at most once. Earlier callbacks may remove or rearm later ones;
  // newly due work waits for another pass. Exceptions propagate after clearing dispatch state.
  void expire(Clock::time_point now);

  // Thread-safe, but should only be called from the TUI thread: check current deadlines without dispatching.
  [[nodiscard]] bool has_due_work(Clock::time_point now = Clock::now()) noexcept;

  // End of tui-thread only functions.
  //===========================================================================

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Handles alone may mutate timers by ID; feature clients use their handles.
  friend class TuiTimerHandle;
  // Store a nonempty callback under a never-reused ID and return its only handle responsible for removing it.
  // Targets stay alive until removal and any active call returns.
  [[nodiscard]] TuiTimerHandle register_timer(TuiTimer::Callback callback) /*threadsafe-*/ const;
  // Remove the ID when its handle dies; copied callbacks already executing
  // are unaffected, so their targets must survive until the invocation returns.
  void remove_timer(TuiTimerId timer_id) /*threadsafe-*/ const noexcept;
  // Distinguish an armed deadline from a disarmed or removed timer.
  [[nodiscard]] bool timer_armed(TuiTimerId timer_id) /*threadsafe-*/ const noexcept;
  // Disarm an existing ID and invalidate its pending dispatch snapshot;
  // retain the timer so the same handle can arm it again.
  void disarm_timer(TuiTimerId timer_id) /*threadsafe-*/ const noexcept;

  // The functions below do not race, hence const, but TuiTimerHandle only calls them from
  // its TUI-thread-only members: a deadline set by another thread would not wake the event loop.

  // Replace an existing ID's schedule with one absolute deadline;
  // invalidate an older dispatch snapshot without invoking a callback here.
  void arm_timer_at(TuiTimerId timer_id, Clock::time_point deadline) /*threadsafe-*/ const;
  // Arm one shot relative to now, clamping nonpositive delay to now
  // and overflow to the last representable deadline; missing IDs are ignored.
  void arm_timer_after(TuiTimerId timer_id, Clock::duration delay, Clock::time_point now) /*threadsafe-*/ const;
  // Replace an existing schedule with positive-interval repetition;
  // the first tick follows now and missed ticks will be coalesced during dispatch.
  void arm_timer_every(TuiTimerId timer_id, Clock::duration interval, Clock::time_point now) /*threadsafe-*/ const;
};

} // namespace ava::tui
