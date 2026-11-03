// Adapts utils' OS waiter to coalescing TUI notifications and permanent terminal
// failure latches, keeping producer calls independent of session object lifetimes.
#include "sys.h"
#include "TuiWaitState.h"

namespace ava::tui {

TuiWaitState::TuiWaitState(int input_fd)
    : terminal_event_(waiter_.watch_fd(input_fd, utils::threading::EventLoop::PollEvents::read)), application_event_(waiter_.watch_event())
{
}

void TuiWaitState::notify() noexcept
{
  try
  {
    waiter_.notify(application_event_);
  }
  catch (...)
  {
    // Producer completion must not throw from its wakeup path. Publish failure
    // for the TUI's next check; the failed syscall itself may not wake a waiter.
    notification_failed_.store(true, std::memory_order_release);
  }
}

bool TuiWaitState::notification_failed() const noexcept
{
  return notification_failed_.load(std::memory_order_acquire);
}

void TuiWaitState::wait(std::optional<Clock::time_point> deadline)
{
  // utils consumes notifications and retries EINTR internally. Neither path reads
  // terminal bytes; composition drains the Context and application mailboxes later.
  if (deadline)
    waiter_.wait_until(*deadline);
  else
    waiter_.wait();

  auto const& result = waiter_.result();
  if ((result.events() & terminal_event_).any())
  {
    // Readiness is transient, but terminal failure must survive a later unrelated
    // wake until composition observes it and leaves the loop.
    auto const ready = result.ready_events(utils::threading::EventLoop::index(terminal_event_));
    terminal_hangup_ = terminal_hangup_ || has_event(ready, utils::threading::EventLoop::ReadyEvents::hangup);
    terminal_error_ = terminal_error_ || has_event(ready, utils::threading::EventLoop::ReadyEvents::error);
  }
}

} // namespace ava::tui
