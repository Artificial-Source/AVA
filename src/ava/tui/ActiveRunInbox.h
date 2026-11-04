// Synchronized worker mailbox for ActiveRun; presentation stays on the TUI thread.
#pragma once

#include "TuiWakeup.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/runtime_state_internal.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ava::tui {

// Final worker result: an owned backend response or captured exception for the TUI.
using ActiveRunResult = std::variant<TuiSubmitResult, std::exception_ptr>;

// Construct with shared ownership before creating endpoints. Copied producers
// retain this synchronized inbox independently of the active UI operation;
// retire() rejects later writes without borrowing screen state.
// One mutex guards the event queue, envelope context, accepting flag and final result;
// stop alone is atomic. Final destruction may occur on the last producer thread.
class ActiveRunInbox final : public std::enable_shared_from_this<ActiveRunInbox>
{
 public:
  // Backend callback type retained by copied endpoints; it may outlive the UI operation
  // and must own, or otherwise safely retain, every object it captures.
  using FollowUpStarted = std::function<ava::core::VoidResult(TuiQueuedFollowUp const&)>;

  // TUI thread before endpoint creation: install a weak loop wake capability.
  explicit ActiveRunInbox(TuiWakeup wakeup);
  // Not copyable: retained producer callbacks share one mutex, event queue and final result.
  ActiveRunInbox(ActiveRunInbox const&) = delete;
  ActiveRunInbox& operator=(ActiveRunInbox const&) = delete;
  // TUI thread after shared construction: return a producer callback that pins this
  // mailbox, stamps events under its mutex and never borrows presentation.
  [[nodiscard]] ava::event::RuntimeEventSink event_sink();
  // TUI thread after shared construction: return a retained, synchronized producer
  // endpoint preserving the supplied envelope identity.
  [[nodiscard]] ava::event::EventEnvelopeSink envelope_sink();
  // TUI thread after shared construction: pin transport for late launch notifications.
  [[nodiscard]] ava::agent::SubagentLaunchSink subagent_launch_sink();
  // TUI thread: retain the original callback and shared inbox; the worker updates
  // request context before invoking the original callback outside the mailbox lock.
  [[nodiscard]] FollowUpStarted wrap_follow_up_started(FollowUpStarted original);
  // TUI thread before launch: set the initial request and correlation IDs for
  // unstamped events. Worker follow-ups update them through follow_up_started().
  void set_request_id(std::string request_id);
  // Either thread: publish the atomic cancellation predicate and wake on its first
  // transition. True identifies the first request; it does not mean work has stopped.
  [[nodiscard]] bool request_stop();
  // Either thread: observe stop without taking the FIFO mutex; the worker must cooperate.
  [[nodiscard]] bool stop_requested() const noexcept;
  // TUI thread: transfer at most maximum oldest entries under the single mailbox
  // mutex. Historical received_any evidence survives drainage.
  [[nodiscard]] std::vector<QueuedTuiEvent> drain(std::size_t maximum = std::numeric_limits<std::size_t>::max());
  // TUI thread: true for queued events or an unconsumed final result. received_any()
  // only records past event arrivals and must never decide whether the loop sleeps.
  [[nodiscard]] bool has_pending() const;
  // TUI thread: detect a stored final result before decoding more input;
  // the caller must consume completion before another key can act on this run.
  [[nodiscard]] bool result_ready() const;
  // TUI thread: synchronized historical event evidence for final-result fallback,
  // independent of whether any current queue entries remain.
  [[nodiscard]] bool received_any() const;
  // TUI thread: drop queued old-session events and reset received_any evidence
  // under lock. Preserve the result slot/consumed marker so no second result is accepted.
  void discard();
  // Worker, after all backend callback scopes retire: publish the first result as
  // its last update to the operation. TUI may join immediately after taking this value.
  void publish_result(ActiveRunResult result);
  // TUI thread: transfer the first final result once and leave a consumed marker,
  // so a later producer cannot publish a replacement after the move.
  [[nodiscard]] std::optional<ActiveRunResult> take_result();
  // TUI thread after worker join and backend queue finish, before final draining:
  // reject later writes while keeping accepted events and the final result.
  void retire();

  // Runtime events, results and request context contain arbitrary message data;
  // generated traversal would expose it.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Producer thread: stamp and enqueue atomically with context changes; notify only
  // after storing the event and unlocking. Retired endpoints accept no more data.
  [[nodiscard]] ava::core::VoidResult enqueue_event(ava::event::RuntimeEvent const& event);
  // Producer thread: check accepting_ and enqueue the complete envelope under mutex_, then wake.
  [[nodiscard]] ava::core::VoidResult enqueue_envelope(ava::event::EventEnvelope const& envelope);
  // Producer thread: use the same queue order and accepting_ check as runtime events.
  void enqueue_subagent_launch(ava::agent::SubagentLaunchNotification const& notification);
  // Worker thread: advance context under lock, then release it before the original
  // callback, which may enqueue events through this same inbox.
  [[nodiscard]] ava::core::VoidResult follow_up_started(FollowUpStarted const& original, TuiQueuedFollowUp const& follow_up);

  // Immutable weak wake handle; retained producers store data before notifying the loop.
  TuiWakeup wakeup_;
  // Sole lock for event queue, context, accepting flag and final result across both threads.
  mutable std::mutex mutex_;
  // Event queue and received_any history; every access takes mutex_, with no internal lock.
  RuntimeEventQueue events_;
  // Identity for newly enqueued runtime events; changed under mutex_ at follow-up start.
  ava::event::EventEnvelopeContext context_;
  // Either thread latches cooperative stop; remains true for this mailbox lifetime.
  std::atomic_bool stop_requested_{false};
  // Cleared under mutex_ after join and backend queue finish; retained sinks then reject writes.
  bool accepting_ = true;
  // Remembers under mutex_ that the TUI already took the result, so a second
  // publish_result() remains rejected until the final shared owner destroys this inbox.
  enum class ResultHistory
  {
    Consumed // TUI took the first result; all later results remain rejected.
  };
  // Under mutex_: empty accepts a result; a value is ready to take; consumed rejects repeats.
  std::variant<std::monostate, ActiveRunResult, ResultHistory> result_;
};

} // namespace ava::tui
