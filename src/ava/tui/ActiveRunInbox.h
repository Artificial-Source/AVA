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

// Owned terminal payload transfers a backend result or captured worker exception to the TUI.
using ActiveRunTerminal = std::variant<TuiSubmitResult, std::exception_ptr>;

// Construct with shared ownership before creating endpoints. Copied producers
// retain this synchronized inbox independently of the active UI operation;
// retirement rejects later publications without borrowing presentation state.
// One mutex guards the pure FIFO, envelope context, admission and terminal history;
// stop alone is atomic. Final destruction may occur on the last producer thread.
class ActiveRunInbox final : public std::enable_shared_from_this<ActiveRunInbox>
{
 public:
  // Backend callback type retained by copied endpoints; it may outlive the UI operation
  // and must own, or otherwise safely retain, every object it captures.
  using FollowUpStarted = std::function<ava::core::VoidResult(TuiQueuedFollowUp const&)>;

  // TUI thread before endpoint creation: install a weak loop wake capability.
  explicit ActiveRunInbox(TuiWakeup wakeup);
  // Shared endpoints must agree on one admission gate and terminal history.
  ActiveRunInbox(ActiveRunInbox const&) = delete;
  // Do not replace synchronization state while retained producer endpoints use it.
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
  // TUI thread: readiness includes an unconsumed terminal payload; received_any is only
  // historical event evidence and must never decide whether the loop sleeps.
  [[nodiscard]] bool has_pending() const;
  // TUI thread: detect a published terminal payload before decoding more input;
  // the caller must consume completion before another key can act on this run.
  [[nodiscard]] bool terminal_ready() const;
  // TUI thread: synchronized historical event evidence for final-result fallback,
  // independent of whether any current queue entries remain.
  [[nodiscard]] bool received_any() const;
  // TUI thread: drop queued old-session events and reset received_any evidence
  // under lock. Preserve terminal history so it can never admit a second result.
  void discard();
  // Worker, after all backend callback scopes retire: publish the first result as
  // its last operation action. TUI may join immediately after taking this value.
  void publish_terminal(ActiveRunTerminal terminal);
  // TUI thread: transfer the first terminal payload once and leave a consumed marker,
  // so a later producer cannot publish a replacement after the move.
  [[nodiscard]] std::optional<ActiveRunTerminal> take_terminal();
  // TUI thread after producer join and queue finish receipts, before final draining.
  // Retirement does not discard already accepted events or the terminal value.
  void retire();

  // Runtime events/results and request context contain arbitrary message and
  // authority data; generated traversal would expose those retained aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Producer thread: stamp and enqueue atomically with context changes; notify only
  // after publication and unlock. Retired endpoints harmlessly accept no more data.
  [[nodiscard]] ava::core::VoidResult enqueue_event(ava::event::RuntimeEvent const& event);
  // Producer thread: enqueue the complete envelope under admission lock, then wake.
  [[nodiscard]] ava::core::VoidResult enqueue_envelope(ava::event::EventEnvelope const& envelope);
  // Producer thread: share the same FIFO order and admission gate as runtime events.
  void enqueue_subagent_launch(ava::agent::SubagentLaunchNotification const& notification);
  // Worker thread: advance context under lock, then release it before the original
  // callback, which may publish receipts through this same inbox.
  [[nodiscard]] ava::core::VoidResult follow_up_started(FollowUpStarted const& original, TuiQueuedFollowUp const& follow_up);

  // Immutable weak loop capability; retained producers notify only after publishing.
  TuiWakeup wakeup_;
  // Sole lock for FIFO, context, admission and terminal state across both threads.
  mutable std::mutex mutex_;
  // Pure FIFO and event history; every access is serialized by mutex_, never internally.
  RuntimeEventQueue events_;
  // Identity for newly enqueued runtime events; changed under mutex_ at follow-up start.
  ava::event::EventEnvelopeContext context_;
  // Either thread latches cooperative stop; remains true for this mailbox lifetime.
  std::atomic_bool stop_requested_{false};
  // Cleared under mutex_ after join and queue finish; retained sinks then reject publication.
  bool accepting_ = true;
  // Private mailbox history under mutex_: preserve first-publication wins after
  // TUI take moves the payload out, until the final shared owner destroys this inbox.
  enum class TerminalHistory
  {
    Consumed // TUI has taken the first payload; all later publications remain rejected.
  };
  // Under mutex_: empty permits publication; payload is ready; consumed rejects repeats.
  std::variant<std::monostate, ActiveRunTerminal, TerminalHistory> terminal_;
};

} // namespace ava::tui
