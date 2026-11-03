#include "sys.h"
#include "TuiWaitState.h"

namespace ava::tui {

TuiWaitState::TuiWaitState(int input_fd)
    : terminal_event_(waiter_.watch_fd(input_fd, utils::threading::EventLoop::PollEvents::read)), application_event_(waiter_.watch_event())
{
}

void TuiWaitState::notify() noexcept
{
  if (!enabled_.load(std::memory_order_acquire))
    return;
  try
  {
    waiter_.notify(application_event_);
  }
  catch (...)
  {
    notification_failed_.store(true, std::memory_order_release);
  }
}

void TuiWaitState::disable() noexcept
{
  enabled_.store(false, std::memory_order_release);
}

bool TuiWaitState::notification_failed() const noexcept
{
  return notification_failed_.load(std::memory_order_acquire);
}

void TuiWaitState::wait(std::optional<Clock::time_point> deadline)
{
  if (deadline)
    waiter_.wait_until(*deadline);
  else
    waiter_.wait();

  auto const& result = waiter_.result();
  if ((result.events() & terminal_event_).any())
  {
    auto const ready = result.ready_events(utils::threading::EventLoop::index(terminal_event_));
    terminal_hangup_ = terminal_hangup_ || has_event(ready, utils::threading::EventLoop::ReadyEvents::hangup);
    terminal_error_ = terminal_error_ || has_event(ready, utils::threading::EventLoop::ReadyEvents::error);
  }
}

} // namespace ava::tui
