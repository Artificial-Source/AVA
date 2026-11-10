// Encapsulates one registry-owned deadline and its dispatch generation; TuiTimerHandle removes
// the timer on destruction and TuiTimeoutHandler decides when the callback may run.
#pragma once

#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace ava::tui {

// Stored by value in TuiTimeoutHandler's map.
//
// This class is not thread-safe, and its const members are const in the C++ sense only.
// It is safe anyway because a TuiTimer can only be reached through TuiTimeoutHandler::timers_,
// so every access, from whichever thread, happens under that map's lock.
// Only the callback, invoked by TuiTimeoutHandler::expire, is bound to the TUI thread.
//
// TuiTimerHandle operations update its schedule; the registry snapshots its generation to
// reject work changed by an earlier callback. prepare_dispatch copies the callback
// and advances/disarms the schedule before invocation, so removing a timer
// inside a callback is safe. Callback targets must survive the executing call as
// well as their handle; destroying this record does not destroy borrowed targets.
class TuiTimer final
{
 public:
  // Use the same monotonic clock as the registry and public timer handles.
  using Clock = std::chrono::steady_clock;
  // Callback, run on the TUI thread, that updates feature state or requests a frame. It must not
  // paint, poll input, run session control checks, wait or recursively dispatch timers. Its target
  // stays alive until the callback returns.
  using Callback = std::function<void(Clock::time_point)>;

  // Own a nonempty binding in a disarmed record. The binding's borrowed
  // targets must survive removal of this timer and any copied invocation in progress.
  explicit TuiTimer(Callback callback);
  // Read the revision used to reject a stale due-work snapshot; this
  // does not retain the record, so dispatch must look up its ID again before use.
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  // Copy the current wake deadline; absence means disarmed. Later
  // scheduling changes may invalidate this value, so callers retain no reference.
  [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept { return deadline_; }
  // Check whether the deadline is due at now without changing the schedule
  // or running the callback; the registry rechecks after earlier callbacks have run.
  [[nodiscard]] bool due(Clock::time_point now) const noexcept;
  // Remove deadline/repetition and advance the generation so a saved
  // due entry cannot fire; preserve the callback so this timer can be armed again.
  void disarm() noexcept;
  // Replace any repeating/one-shot schedule and advance generation.
  // A deadline at or before now becomes eligible on the next dispatch pass.
  void arm_at(Clock::time_point deadline);
  // Arm once relative to now, mapping nonpositive delay to now and
  // overflow to time_point::max; invalidates old dispatch work through arm_at.
  void arm_after(Clock::duration delay, Clock::time_point now);
  // Require a positive interval and replace the schedule, first due
  // after now. Invalidates old work; invalid intervals disarm in nonasserting builds.
  void arm_every(Clock::duration interval, Clock::time_point now);
  // After checking due(now) and the saved generation: copy the binding,
  // advance generation and disarm one shots or advance repeats beyond now. The copy
  // may run after this record is removed, provided its target stays alive through return.
  [[nodiscard]] Callback prepare_dispatch(Clock::time_point now);

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Binding installed on construction and copied for dispatch. It owns
  // the callable, not the lifetime of create_timer's borrowed target.
  Callback callback_;
  // Scheduling methods set/clear the next eligible time; prepare_dispatch clears
  // one shots or advances repeats before the callback is allowed to mutate the registry.
  std::optional<Clock::time_point> deadline_;
  // Records positive repeat cadence, or zero for one shot/disarmed.
  // Dispatch skips missed periods instead of calling back once per missed tick.
  Clock::duration interval_{};
  // Incremented on every arm/disarm and prepare_dispatch. The registry
  // snapshots (id, generation) before callbacks, then skips entries changed by earlier
  // callbacks; no callback retains a registry iterator across an invocation.
  std::uint64_t generation_ = 0;
};

} // namespace ava::tui
