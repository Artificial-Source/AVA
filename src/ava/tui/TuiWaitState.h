// Holds the OS wait resources that producer wakeups may briefly retain after the
// TUI loop is gone; feature and session objects are deliberately not retained.
#pragma once

#include "ava/debug/print_members_on.h"
#include "utils/threading/EventLoop.h"

#include <atomic>
#include <chrono>
#include <optional>

namespace ava::tui {

// TuiEventLoop owns this through a shared_ptr and is its sole TUI-thread waiter.
// Producers use TuiWakeup to acquire temporary shared ownership only for notify().
// A notify call may outlive the loop and keep this state available to other tokens
// until all shared references are gone. Final destruction may run on a producer
// thread. The implicit destructor closes only OS wait resources after the last
// user releases its share. Producers
// must still be joined before their other borrowed objects die. The input fd is
// borrowed until the loop stops waiting and is never closed by this object.
class TuiWaitState final
{
 public:
  // Match the loop's monotonic deadlines without exposing feature scheduling here.
  using Clock = std::chrono::steady_clock;

  // TUI thread before giving wakeup tokens to producers: register the borrowed valid input
  // fd and application event once. Failure to allocate/register OS resources throws.
  explicit TuiWaitState(int input_fd);
  // Any thread holding a shared reference: store the payload in its synchronized
  // mailbox first, then notify the current or next wait. Repeated notifications
  // may merge into one wakeup and carry no payload. Record OS notification failures
  // for the TUI reader; a failed notification may leave an existing wait blocked.
  void notify() noexcept;
  // Sole TUI waiter, while the input fd is valid: consume pending notifications
  // and readiness, optionally sleeping until deadline. Latch terminal errors/hangup
  // without reading input. OS wait errors propagate; utils retries EINTR internally.
  void wait(std::optional<Clock::time_point> deadline);
  // TUI thread: inspect the hangup latch set by wait; once true it stays true.
  [[nodiscard]] bool terminal_hangup() const noexcept { return terminal_hangup_; }
  // TUI thread: inspect the descriptor-error latch set by wait; it never resets.
  [[nodiscard]] bool terminal_error() const noexcept { return terminal_error_; }
  // Any thread holding this state alive: read the permanent failure flag set by
  // notify. Observing failure does not imply a failed syscall woke the waiter.
  [[nodiscard]] bool notification_failed() const noexcept;

  // utils::threading::EventLoop has no printer for its platform wait resources.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Constructed before event registration and destroyed after it; the TUI waits,
  // producers only notify, and shared ownership prevents concurrent destruction.
  utils::threading::EventLoop waiter_;
  // Immutable after TUI construction; identifies borrowed terminal-fd readiness
  // when wait updates the two TUI-only latches below.
  utils::threading::EventLoop::Event terminal_event_;
  // Immutable notification bit registered before producers receive tokens. All
  // producers share this event because their payloads live in separate mailboxes.
  utils::threading::EventLoop::Event application_event_;
  // Producers release-store true on notify failure; acquire readers see a permanent
  // error indication without sharing the TUI-only terminal readiness fields.
  std::atomic_bool notification_failed_{false};
  // Only the TUI waiter sets/reads this; retain hangup across later readiness results.
  bool terminal_hangup_ = false;
  // Only the TUI waiter sets/reads this; retain descriptor failure until TuiSession exits.
  bool terminal_error_ = false;
};

} // namespace ava::tui
