// Implements registry-safe timer dispatch and the final wait stage of a TUI turn.
#include "sys.h"
#include "TuiEventLoop.h"
#include "TuiWaitState.h"

#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "debug.h"

namespace ava::tui {

TuiEventLoop::TuiEventLoop(int input_fd) : wait_state_(std::make_shared<TuiWaitState>(input_fd))
{
}

TuiEventLoop::~TuiEventLoop()
{
  // Destroy every TuiTimer handle before its loop, including disarmed handles; declare the loop before its clients.
  ASSERT(timers_.empty());
}

TuiTimer TuiEventLoop::register_timer(TuiTimerRegistration::Callback callback)
{
  if (next_timer_id_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("TUI timer registration IDs exhausted");
  auto const id = next_timer_id_++;
  timers_.try_emplace(id, std::move(callback));
  return TuiTimer(*this, id);
}

void TuiEventLoop::remove_timer(std::uint64_t id) noexcept
{
  timers_.erase(id);
}

void TuiEventLoop::cancel_timer(std::uint64_t id) noexcept
{
  if (auto timer = timers_.find(id); timer != timers_.end())
    timer->second.cancel();
}

bool TuiEventLoop::timer_active(std::uint64_t id) const noexcept
{
  auto const timer = timers_.find(id);
  return timer != timers_.end() && timer->second.deadline().has_value();
}

void TuiEventLoop::arm_timer_at(std::uint64_t id, Clock::time_point deadline)
{
  if (auto timer = timers_.find(id); timer != timers_.end())
    timer->second.arm_at(deadline);
}

void TuiEventLoop::arm_timer_after(std::uint64_t id, Clock::duration delay, Clock::time_point now)
{
  if (auto timer = timers_.find(id); timer != timers_.end())
    timer->second.arm_after(delay, now);
}

void TuiEventLoop::repeat_timer(std::uint64_t id, Clock::duration interval, Clock::time_point now)
{
  if (auto timer = timers_.find(id); timer != timers_.end())
    timer->second.repeat(interval, now);
}

void TuiEventLoop::service_due(Clock::time_point now)
{
  // Do not call service_due from a timer callback; arm it for the next loop turn instead.
  ASSERT(!servicing_due_);
  if (servicing_due_ || stopped_)
    return;

  servicing_due_ = true;
  try
  {
    // Fix this pass's eligible work before invoking clients: callbacks can add,
    // remove or reschedule timers, and newly armed work belongs to a later pass.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> due;
    due.reserve(timers_.size());
    for (auto const& [id, timer] : timers_)
    {
      if (timer.due(now))
        due.emplace_back(id, timer.generation());
    }
    for (auto const& [id, generation] : due)
    {
      if (stopped_)
        break;
      TuiTimerRegistration::Callback callback;
      {
        // Re-find by ID and generation because an earlier callback may have
        // invalidated this entry even though it was due when the pass began.
        auto timer = timers_.find(id);
        if (timer == timers_.end() || timer->second.generation() != generation || !timer->second.due(now))
          continue;
        callback = timer->second.prepare_dispatch(now);
      }
      // No registry reference/iterator survives this call. The callback may
      // cancel, rearm or destroy any registration, including its own.
      callback(now);
    }
  }
  catch (...)
  {
    // A throwing callback must not leave the loop permanently marked as inside
    // dispatch if the session catches the exception and performs cleanup.
    servicing_due_ = false;
    throw;
  }
  servicing_due_ = false;
}

bool TuiEventLoop::has_due_work(Clock::time_point now) const noexcept
{
  for (auto const& [id, timer] : timers_)
  {
    if (timer.due(now))
      return true;
  }
  return false;
}

void TuiEventLoop::wait(TuiWaitMode mode)
{
  // Timer callbacks must return to composition before waiting; they may only publish state or invalidate a frame.
  ASSERT(!servicing_due_);
  if (servicing_due_ || stopped_ || terminal_hangup() || terminal_error() || notification_failed())
    return;

  std::optional<Clock::time_point> deadline;
  if (mode == TuiWaitMode::ReadyOnly)
    // Even a runnable turn collects readiness so hangup/error and notification
    // consumption follow the same path as a sleeping turn.
    deadline = Clock::now();
  else
  {
    // Only armed registrations constrain sleep. Their callbacks run next turn,
    // after composition's control stage, rather than inside this OS wait.
    for (auto const& [id, timer] : timers_)
    {
      auto const next = timer.deadline();
      if (next && (!deadline || *next < *deadline))
        deadline = next;
    }
  }
  // wait consumes coalesced notification bits, not the producer payloads. The
  // next turn must revisit every application mailbox regardless of wake reason.
  wait_state_->wait(deadline);
}

TuiWakeup TuiEventLoop::wakeup() const noexcept
{
  return TuiWakeup(wait_state_);
}

bool TuiEventLoop::terminal_hangup() const noexcept
{
  return wait_state_->terminal_hangup();
}

bool TuiEventLoop::terminal_error() const noexcept
{
  return wait_state_->terminal_error();
}

bool TuiEventLoop::notification_failed() const noexcept
{
  return wait_state_->notification_failed();
}

} // namespace ava::tui
