// Coordinates TUI waiting and feature timers; TuiSession owns the turn ordering.
// See docs/engineering/tui-architecture.md for ownership and the complete turn.
#pragma once

#include "TuiTimer.h"
#include "TuiTimerRegistration.h"
#include "TuiWakeup.h"
#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

namespace ava::tui {

class TuiWaitState;

// Chosen by TuiSession on the TUI thread after servicing the turn's work.
enum class TuiWaitMode
{
  // Sleep until an armed deadline, input readiness or a producer notification.
  Block,
  // Collect and consume readiness without sleeping when another turn is runnable.
  ReadyOnly
};

// TuiSession's sole waiter and registry for feature-owned TuiTimer handles. All
// members are TUI-thread-only; producers use a previously obtained TuiWakeup.
// Handles, including disarmed ones, must die before the loop; callback targets
// stay at stable addresses until their registrations are removed and active calls end.
// Owners join producers before destroying objects those producers borrow. A wakeup
// alone requires no join: its temporary shared ownership protects only OS resources.
class TuiEventLoop final
{
 public:
  // Use monotonic deadlines throughout the TUI so wall-clock changes cannot reschedule work.
  using Clock = std::chrono::steady_clock;

  // TUI thread: borrow a valid input fd until destruction and create the OS waiter.
  // Construction may throw if the descriptor cannot be registered or resources run out.
  explicit TuiEventLoop(int input_fd);
  // TUI thread, outside dispatch and after every timer handle is destroyed. Releases
  // the loop's wait-state share; an in-flight notifier may release the last share later.
  ~TuiEventLoop();
  // Not copyable or movable: timer handles borrow this registry at a stable address.
  TuiEventLoop(TuiEventLoop const&) = delete;
  TuiEventLoop& operator=(TuiEventLoop const&) = delete;
  TuiEventLoop(TuiEventLoop&&) = delete;
  TuiEventLoop& operator=(TuiEventLoop&&) = delete;

  // TUI thread: register an initially disarmed member callback. target must stay
  // at this address until the registration is removed and any executing callback returns.
  // Callbacks may update feature state or request a frame, but must not paint, poll
  // input, run session control checks, recursively dispatch timers or wait; TuiSession orders those stages.
  template <typename Target>
  [[nodiscard]] TuiTimer member_timer(Target& target, void (Target::*callback)(Clock::time_point))
  {
    return register_timer(std::bind_front(callback, &target));
  }

  // TUI thread, once in the timer stage and never recursively: snapshot due IDs
  // and generations, then invoke each still-current callback at most once. Earlier
  // callbacks may remove or rearm later ones; newly due work waits for another pass.
  // A stop request ends dispatch. Exceptions propagate after clearing dispatch state.
  void service_due(Clock::time_point now = Clock::now());
  // TUI thread, after control, timers, application updates, input and paint. Block
  // sleeps to the earliest timer/input/notification; ReadyOnly collects readiness.
  // This consumes notification bits, so recheck every mailbox next turn. It neither
  // consumes input nor calls features; drain ncurses/replay buffers before Block.
  // Stop, hangup, terminal error and notifier failure latches prevent further waits.
  // utils retries EINTR: signals alone do not return control. The session's 100 ms
  // control timer supplies a nominal check cadence, not a maximum response latency.
  void wait(TuiWaitMode mode);
  // TUI thread while this loop lives: create a copyable token for producer notifications.
  // The token does not retain this loop or any callback target. Producers must store
  // their payload in a synchronized mailbox before calling notify().
  [[nodiscard]] TuiWakeup wakeup() const noexcept;
  // TUI thread: permanently end dispatch and waiting for this loop; does not notify
  // a different thread, cancel registrations, or stop/join producer work.
  void request_stop() noexcept { stopped_ = true; }
  // TUI thread: observe the irreversible stop latch without servicing any work.
  [[nodiscard]] bool stopped() const noexcept { return stopped_; }
  // TUI thread: report hangup observed by a completed wait; remains true thereafter.
  [[nodiscard]] bool terminal_hangup() const noexcept;
  // TUI thread: report descriptor error observed by a completed wait; never clears it.
  [[nodiscard]] bool terminal_error() const noexcept;
  // TUI thread: observe a producer's recorded notify failure so TuiSession can exit.
  // A failed notification itself does not guarantee that an already blocked wait wakes.
  [[nodiscard]] bool notification_failed() const noexcept;
  // TUI thread after timer/application stages: check current deadlines without
  // dispatching, so work newly armed for now can keep the next turn from blocking.
  [[nodiscard]] bool has_due_work(Clock::time_point now = Clock::now()) const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Handles alone may mutate registrations by ID; feature clients use their handles.
  friend class TuiTimer;
  // TUI thread: store a nonempty callback under a never-reused ID and return its
  // only handle responsible for removing it. Targets stay alive until removal and any active call returns.
  [[nodiscard]] TuiTimer register_timer(TuiTimerRegistration::Callback callback);
  // TUI thread: remove the ID when its handle dies; copied callbacks already executing
  // are unaffected, so their targets must survive until the invocation returns.
  void remove_timer(std::uint64_t id) noexcept;
  // TUI thread: disarm an existing ID and invalidate its pending dispatch snapshot;
  // retain the registration so the same handle can arm it again.
  void cancel_timer(std::uint64_t id) noexcept;
  // TUI thread: distinguish an armed deadline from a disarmed or removed registration.
  [[nodiscard]] bool timer_active(std::uint64_t id) const noexcept;
  // TUI thread: replace an existing ID's schedule with one absolute deadline;
  // invalidate an older dispatch snapshot without invoking a callback here.
  void arm_timer_at(std::uint64_t id, Clock::time_point deadline);
  // TUI thread: arm one shot relative to now, clamping nonpositive delay to now
  // and overflow to the last representable deadline; missing IDs are ignored.
  void arm_timer_after(std::uint64_t id, Clock::duration delay, Clock::time_point now);
  // TUI thread: replace an existing schedule with positive-interval repetition;
  // the first tick follows now and missed ticks will be coalesced during dispatch.
  void repeat_timer(std::uint64_t id, Clock::duration interval, Clock::time_point now);

  // Set at TUI-thread construction and released at destruction. Shared ownership
  // keeps OS resources alive during producer notify calls; the last such call may
  // destroy this state on its producer thread. Tokens expire after all shares are gone.
  std::shared_ptr<TuiWaitState> wait_state_;
  // TUI-thread registry, with entries created and removed through TuiTimer handles.
  // Callbacks are copied out before invocation so callbacks may mutate this map.
  std::map<std::uint64_t, TuiTimerRegistration> timers_;
  // TUI thread assigns increasing IDs and rejects exhaustion to avoid confusing a
  // removed registration or dispatch snapshot with a later registration.
  std::uint64_t next_timer_id_ = 1;
  // TUI thread sets this irreversible latch; it suppresses further callbacks and waits.
  bool stopped_ = false;
  // TUI thread holds this through service_due, including callbacks; rejects recursive
  // dispatch/wait and is cleared on both normal return and exception propagation.
  bool servicing_due_ = false;
};

} // namespace ava::tui
