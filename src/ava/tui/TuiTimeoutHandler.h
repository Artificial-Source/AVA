#pragma once

#include "TuiTimer.h"
#include "TuiTimerHandle.h"                     // TuiTimerHandle, TuiTimerId
#include "TuiWakeup.h"
#include "ava/debug/print_members_on.h"
#include "threadsafe/threadsafe.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace ava::tui {

// TuiTimeoutHandler
//
// Registry of feature timers whose callbacks run on the TUI thread.
// See docs/engineering/tui-architecture.md for ownership.
//
// TuiFrontEnd::run calls `expire` at the start of every turn of its loop; that runs what is due
// and returns until when the loop may sleep.
//
// All const member functions are thread-safe, so any thread that has a TuiTimeoutHandler const& can
// create a timer; the timer's owner can arm and disarm it on any thread too (see TuiTimerHandle).
// Once `attach` was called, arming a timer on another thread wakes the TUI thread when needed.
// The non-const member functions are TUI-thread-only.
//
// Handles returned by `create_timer`, including disarmed ones, must die before this registry;
// callback Target's must stay at stable addresses until their timers are removed and active calls end.
//
class TuiTimeoutHandler
{
 public:
  // Use monotonic deadlines throughout the TUI so wall-clock changes cannot reschedule work.
  using Clock = std::chrono::steady_clock;

 private:
  // All state that is protected by the mutex of timers_ts.
  struct Timers
  {
    using by_id_type = std::map<TuiTimerId, TuiTimer>;

    // All timers, with entries created and removed through TuiTimerHandle handles.
    by_id_type by_id;
    // The deadline of every armed timer in by_id, earliest first.
    // Every change of a deadline in by_id must be followed by a call to reindex.
    std::set<std::pair<Clock::time_point, TuiTimerId>> by_deadline;
    // Until when the TUI thread sleeps, as far as arming timers on other threads is concerned:
    // an armed deadline before this must wake the TUI thread.
    // Clock::time_point::max() means "without deadline", which is also the initial value.
    // Clock::time_point::min() means that waking it is pointless: it is already being woken, or `stop` was called.
    Clock::time_point sleeping_until = Clock::time_point::max();
    // The TUI thread, as recorded by `attach`. Arming a timer on the TUI thread never needs to wake it.
    std::thread::id tui_thread;
    // Wakes the TUI thread; inert until `attach`.
    TuiWakeup wakeup;

    // Update by_deadline after the deadline of timer, an entry of by_id, changed from old_deadline.
    //
    // When by_deadline can not be updated, timer is disarmed before the exception propagates,
    // so that by_id and by_deadline always agree.
    void reindex(by_id_type::iterator timer, std::optional<Clock::time_point> old_deadline);

    // Return true when a timer that was just armed for deadline on the current thread must wake the TUI thread.
    //
    // Returning true sets sleeping_until to Clock::time_point::min(), so that later arms do not wake it again.
    [[nodiscard]] bool must_wake_for(Clock::time_point deadline);

    // Return the earliest armed deadline, or nothing when no timer is armed, and record it as the
    // deadline that the TUI thread is about to sleep until.
    std::optional<Clock::time_point> record_sleep() noexcept;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  using timers_ts = threadsafe::Unlocked<Timers, threadsafe::policy::Primitive<std::mutex>>;

  //---------------------------------------------------------------------------
  // These members may be accessed by any thread.

  // All timers and their bookkeeping.
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

  // Let timers that other threads arm wake the TUI thread through wakeup, and record the calling thread as the TUI thread.
  //
  // Call this once, before the TUI thread starts calling `expire`.
  // Before this call, arming a timer on another thread wakes nothing.
  void attach(TuiWakeup wakeup);

  // Stop running timers: `expire` and `next_deadline` return nothing and `expire` runs nothing from now on.
  //
  // Timers can not be restarted afterwards and arming them no longer wakes the TUI thread. Calling this again has no effect.
  void stop() noexcept;

  // Run the callbacks of all timers that are due at now, unless `stop` was called, then return
  // until when the caller may sleep: the earliest deadline, or nothing when no timer is armed.
  //
  // Call this at the start of every turn of the event loop, and sleep at most until the returned
  // deadline. A returned deadline in the past was armed by a callback: poll for events without
  // sleeping, and call this again.
  // When nothing is due this takes the lock once and does not search the timers.
  //
  // Never recursively: snapshot due IDs and generations, then invoke each still-current callback
  // at most once. Earlier callbacks may remove or rearm later ones; newly due work waits for the
  // next call. Exceptions propagate after clearing dispatch state.
  std::optional<Clock::time_point> expire(Clock::time_point now);

  // Return the earliest deadline, or nothing when no timer is armed or after `stop`, without running anything.
  //
  // Like `expire`, this records the returned deadline as the one the TUI thread is about to sleep until:
  // call it instead of using the return value of `expire` when timers might have been armed since.
  // Cheap: it does not search the timers.
  [[nodiscard]] std::optional<Clock::time_point> next_deadline() noexcept;

  // Return true when a timer is due at now, without running anything.
  //
  // Thread-safe, but only meaningful on the TUI thread. Cheap: it does not search the timers.
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
  //
  // Never wakes the TUI thread: a removed deadline at most causes a wakeup in which nothing is due.
  void disarm_timer(TuiTimerId timer_id) /*threadsafe-*/ const noexcept;

  // The arm functions below wake the TUI thread when they are called on another thread
  // and the new deadline lies before the deadline that the TUI thread sleeps until.

  // Apply arm to the timer of timer_id, if it still exists, keep the deadline index in sync and wake the TUI thread if needed.
  template <typename Arm>
  void arm_timer(TuiTimerId timer_id, Arm&& arm) /*threadsafe-*/ const;
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
