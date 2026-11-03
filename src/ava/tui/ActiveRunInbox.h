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

using ActiveRunTerminal = std::variant<TuiSubmitResult, std::exception_ptr>;

// Construct with shared ownership before creating endpoints. Copied producers
// retain this synchronized inbox independently of the active UI operation;
// retirement rejects later publications without borrowing presentation state.
class ActiveRunInbox final : public std::enable_shared_from_this<ActiveRunInbox>
{
 public:
  using FollowUpStarted = std::function<ava::core::VoidResult(TuiQueuedFollowUp const&)>;

  explicit ActiveRunInbox(TuiWakeup wakeup);
  ActiveRunInbox(ActiveRunInbox const&) = delete;
  ActiveRunInbox& operator=(ActiveRunInbox const&) = delete;
  [[nodiscard]] ava::event::RuntimeEventSink event_sink();
  [[nodiscard]] ava::event::EventEnvelopeSink envelope_sink();
  [[nodiscard]] ava::agent::SubagentLaunchSink subagent_launch_sink();
  [[nodiscard]] FollowUpStarted wrap_follow_up_started(FollowUpStarted original);
  void set_request_id(std::string request_id);
  [[nodiscard]] bool request_stop();
  [[nodiscard]] bool stop_requested() const noexcept;
  [[nodiscard]] std::vector<QueuedTuiEvent> drain(std::size_t maximum = std::numeric_limits<std::size_t>::max());
  // Readiness includes an unconsumed terminal payload; received_any is only
  // historical event evidence and must never decide whether the loop sleeps.
  [[nodiscard]] bool has_pending() const;
  // A published terminal payload must be consumed before decoding more input.
  [[nodiscard]] bool terminal_ready() const;
  [[nodiscard]] bool received_any() const;
  void discard();
  void publish_terminal(ActiveRunTerminal terminal);
  [[nodiscard]] std::optional<ActiveRunTerminal> take_terminal();
  // Call after producer join and queue finish receipts, before final draining.
  // Retirement does not discard already accepted events or the terminal value.
  void retire();

  // Runtime events/results and request context contain arbitrary message and
  // authority data; generated traversal would expose those retained aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] ava::core::VoidResult enqueue_event(ava::event::RuntimeEvent const& event);
  [[nodiscard]] ava::core::VoidResult enqueue_envelope(ava::event::EventEnvelope const& envelope);
  void enqueue_subagent_launch(ava::agent::SubagentLaunchNotification const& notification);
  [[nodiscard]] ava::core::VoidResult follow_up_started(FollowUpStarted const& original, TuiQueuedFollowUp const& follow_up);

  TuiWakeup wakeup_;
  mutable std::mutex mutex_;
  RuntimeEventQueue events_;
  ava::event::EventEnvelopeContext context_;
  std::atomic_bool stop_requested_{false};
  bool accepting_ = true;
  // A consumed marker preserves first-publication wins after the payload moves out.
  enum class TerminalHistory
  {
    Consumed // TUI has taken the first payload; all later publications remain rejected.
  };
  // Under mutex_: empty permits publication; payload is ready; consumed rejects repeats.
  std::variant<std::monostate, ActiveRunTerminal, TerminalHistory> terminal_;
};

} // namespace ava::tui
