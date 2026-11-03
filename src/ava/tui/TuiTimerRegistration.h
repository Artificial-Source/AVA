// Encapsulates one loop-owned deadline and its dispatch generation; TuiTimer owns
// retirement and TuiEventLoop decides when the callback may run.
#pragma once

#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace ava::tui {

// Stored by value in TuiEventLoop's registry and accessed only on the TUI thread.
// TuiTimer operations update its schedule; the loop snapshots its generation to
// reject work changed by an earlier callback. prepare_dispatch copies the callback
// and advances/disarms the schedule before invocation, so removing a registration
// inside a callback is safe. Callback targets must survive the executing call as
// well as their handle; destroying this record does not destroy borrowed targets.
class TuiTimerRegistration final
{
 public:
  // Use the same monotonic clock as the loop and public timer handles.
  using Clock = std::chrono::steady_clock;
  // TUI-thread publication/invalidation only: callbacks must not paint, poll input,
  // service control, wait or recursively dispatch. Targets remain valid through return.
  using Callback = std::function<void(Clock::time_point)>;

  // TUI thread: own a nonempty binding in a disarmed record. The binding's borrowed
  // targets must survive registration removal and any copied invocation in progress.
  explicit TuiTimerRegistration(Callback callback);
  // TUI thread: read the revision used to reject a stale due-work snapshot; this
  // does not retain the record, so dispatch must look up its ID again before use.
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  // TUI thread: copy the current wake deadline; absence means disarmed. Later
  // scheduling changes may invalidate this value, so callers retain no reference.
  [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept { return deadline_; }
  // TUI thread: test deadline eligibility at now without changing repetition or
  // claiming dispatch; the loop rechecks after earlier callbacks have run.
  [[nodiscard]] bool due(Clock::time_point now) const noexcept;
  // TUI thread: remove deadline/repetition and advance the generation so a saved
  // due entry cannot fire; preserve the callback for future arms.
  void cancel() noexcept;
  // TUI thread: replace any repeating/one-shot schedule and advance generation.
  // A deadline at or before now becomes eligible on the next dispatch pass.
  void arm_at(Clock::time_point deadline);
  // TUI thread: arm once relative to now, mapping nonpositive delay to now and
  // overflow to time_point::max; invalidates old dispatch work through arm_at.
  void arm_after(Clock::duration delay, Clock::time_point now);
  // TUI thread: require a positive interval and replace the schedule, first due
  // after now. Invalidates old work; invalid intervals cancel in nonasserting builds.
  void repeat(Clock::duration interval, Clock::time_point now);
  // TUI thread after checking due(now) and the saved generation: copy the binding,
  // advance generation and disarm one shots or advance repeats beyond now. The copy
  // may run after this record is removed, provided its target stays alive through return.
  [[nodiscard]] Callback prepare_dispatch(Clock::time_point now);

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Binding installed on TUI-thread construction and copied for dispatch. It owns
  // the callable, not the lifetime of member_timer's borrowed target.
  Callback callback_;
  // TUI scheduling methods set/clear the next eligible time; prepare_dispatch clears
  // one shots or advances repeats before the callback is allowed to mutate the registry.
  std::optional<Clock::time_point> deadline_;
  // TUI thread records positive repeat cadence, or zero for one shot/disarmed.
  // Dispatch skips missed periods instead of calling back once per missed tick.
  Clock::duration interval_{};
  // TUI thread increments on every arm/cancel/repeat and prepare_dispatch. The loop
  // snapshots (id, generation) before callbacks, then skips entries changed by earlier
  // callbacks; no callback retains a registry iterator across an invocation.
  std::uint64_t generation_ = 0;
};

} // namespace ava::tui
