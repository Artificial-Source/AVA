// Gives producers a payload-free wakeup token for the TUI thread's event loop.
#pragma once

#include "ava/debug/print_members_on.h"
#include "utils/threading/EventLoop.h"

#include <memory>

namespace ava::tui {

// Copyable token that producers (worker threads, mailbox owners, and the TUI thread itself) use to
// wake the TUI thread. It names one notification Event of the event loop that TuiFrontEnd owns.
//
// The token holds only a weak reference to that loop, so it can be stored in objects that outlive
// the TUI session and even the frontend; once the loop is gone notify() does nothing. A notify()
// that locked the loop keeps it alive until that call returns, so the last owner may then be
// released on the producer thread. Copy, move and destruction follow ordinary object
// synchronization rules: do not replace or destroy a token while another thread calls it.
class TuiWakeup
{
 private:
  // Initialized at construction/copy and only read during notify.
  std::weak_ptr<utils::threading::EventLoop> event_loop_;

  // The notification Event of event_loop_ that notify() publishes.
  utils::threading::EventLoop::Event event_{};

 public:
  // Any thread: create an inert token for owners that have not attached to a loop.
  TuiWakeup() = default;

  // TUI thread: create a token that notifies event, which must have been returned by event_loop->watch_event().
  TuiWakeup(std::weak_ptr<utils::threading::EventLoop> event_loop, utils::threading::EventLoop::Event event) noexcept;

  // Any thread: wake the TUI thread.
  //
  // Store the payload in its synchronized mailbox first: several notifications may merge into one
  // wakeup and carry no payload. Empty and expired tokens do nothing.
  void notify() const noexcept;

  // Printing reports only the event; it never locks or prints the loop.
  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui
