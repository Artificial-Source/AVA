// Registry of feature timers on the TUI thread; TuiFrontEnd::run asks it when to wake up next.
// See docs/engineering/tui-architecture.md for ownership.
#pragma once

#include "TuiTimer.h"
#include "TuiTimerHandle.h"
#include "TuiWakeup.h"
#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace ava::tui {

// TuiSession's registry for feature-owned timers, plus the factory of wakeup tokens.
//
// It does not wait: TuiFrontEnd::run owns the event loop, asks next_deadline() how long it may sleep
// and calls service_due() after every wakeup. All members are TUI-thread-only; producers use a
// previously obtained TuiWakeup. Handles, including disarmed ones, must die before the registry;
// callback targets stay at stable addresses until their timers are removed and active calls end.
class TuiTimerRegistry final
{
 public:
  // Use monotonic deadlines throughout the TUI so wall-clock changes cannot reschedule work.
  using Clock = std::chrono::steady_clock;

  // TUI thread: create an empty registry whose wakeup() tokens are copies of wakeup.
  explicit TuiTimerRegistry(TuiWakeup wakeup);
  // TUI thread, outside dispatch and after every timer handle is destroyed.
  ~TuiTimerRegistry();
  // Not copyable or movable: timer handles borrow this registry at a stable address.
  TuiTimerRegistry(TuiTimerRegistry const&) = delete;
  TuiTimerRegistry& operator=(TuiTimerRegistry const&) = delete;
  TuiTimerRegistry(TuiTimerRegistry&&) = delete;
  TuiTimerRegistry& operator=(TuiTimerRegistry&&) = delete;

  // TUI thread: register an initially disarmed member callback. target must stay
  // at this address until the timer is removed and any executing callback returns.
  // Callbacks may update feature state or request a frame, but must not paint, poll
  // input, run session control checks, recursively dispatch timers or wait.
  template <typename Target>
  [[nodiscard]] TuiTimerHandle create_timer(Target& target, void (Target::*callback)(Clock::time_point))
  {
    return register_timer(std::bind_front(callback, &target));
  }

  // TUI thread, never recursively: snapshot due IDs and generations, then invoke each
  // still-current callback at most once. Earlier callbacks may remove or rearm later ones;
  // newly due work waits for another pass. Exceptions propagate after clearing dispatch state.
  void service_due(Clock::time_point now = Clock::now());
  // TUI thread: return the earliest deadline of all armed timers, or nothing when no timer is armed.
  //
  // A returned deadline may already have passed; the caller should then not sleep at all.
  [[nodiscard]] std::optional<Clock::time_point> next_deadline() const noexcept;
  // TUI thread while this registry lives: create a copyable token for producer notifications.
  // Producers must store their payload in a synchronized mailbox before calling notify().
  [[nodiscard]] TuiWakeup wakeup() const noexcept { return wakeup_; }
  // TUI thread: check current deadlines without dispatching.
  [[nodiscard]] bool has_due_work(Clock::time_point now = Clock::now()) const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Handles alone may mutate timers by ID; feature clients use their handles.
  friend class TuiTimerHandle;
  // TUI thread: store a nonempty callback under a never-reused ID and return its
  // only handle responsible for removing it. Targets stay alive until removal and any active call returns.
  [[nodiscard]] TuiTimerHandle register_timer(TuiTimer::Callback callback);
  // TUI thread: remove the ID when its handle dies; copied callbacks already executing
  // are unaffected, so their targets must survive until the invocation returns.
  void remove_timer(TuiTimerId timer_id) noexcept;
  // TUI thread: disarm an existing ID and invalidate its pending dispatch snapshot;
  // retain the timer so the same handle can arm it again.
  void disarm_timer(TuiTimerId timer_id) noexcept;
  // TUI thread: distinguish an armed deadline from a disarmed or removed timer.
  [[nodiscard]] bool timer_armed(TuiTimerId timer_id) const noexcept;
  // TUI thread: replace an existing ID's schedule with one absolute deadline;
  // invalidate an older dispatch snapshot without invoking a callback here.
  void arm_timer_at(TuiTimerId timer_id, Clock::time_point deadline);
  // TUI thread: arm one shot relative to now, clamping nonpositive delay to now
  // and overflow to the last representable deadline; missing IDs are ignored.
  void arm_timer_after(TuiTimerId timer_id, Clock::duration delay, Clock::time_point now);
  // TUI thread: replace an existing schedule with positive-interval repetition;
  // the first tick follows now and missed ticks will be coalesced during dispatch.
  void arm_timer_every(TuiTimerId timer_id, Clock::duration interval, Clock::time_point now);

  // Token copied for every producer; it names the notification Event of TuiFrontEnd's event loop.
  TuiWakeup wakeup_;
  // TUI-thread registry, with entries created and removed through TuiTimerHandle handles.
  // Callbacks are copied out before invocation so callbacks may mutate this map.
  std::map<TuiTimerId, TuiTimer> timers_;
  // TUI thread assigns increasing IDs and rejects exhaustion to avoid confusing a
  // removed timer or dispatch snapshot with a later timer.
  TuiTimerId next_timer_id_ = 1;
  // TUI thread holds this through service_due, including callbacks; rejects recursive
  // dispatch and is cleared on both normal return and exception propagation.
  bool servicing_due_ = false;
};

} // namespace ava::tui
