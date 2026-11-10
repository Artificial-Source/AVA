// Implements registry-safe timer dispatch for the TUI thread.
#include "sys.h"
#include "TuiTimerRegistry.h"

#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "debug.h"

namespace ava::tui {

TuiTimerRegistry::~TuiTimerRegistry()
{
  // Destroy every TuiTimerHandle before its registry, including disarmed handles; declare the registry before its clients.
  ASSERT(timers_.empty());
}

TuiTimerHandle TuiTimerRegistry::register_timer(TuiTimer::Callback callback)
{
  if (next_timer_id_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("TUI timer registration IDs exhausted");
  auto const timer_id = next_timer_id_++;
  timers_.try_emplace(timer_id, std::move(callback));
  return TuiTimerHandle(*this, timer_id);
}

void TuiTimerRegistry::remove_timer(TuiTimerId timer_id) noexcept
{
  timers_.erase(timer_id);
}

void TuiTimerRegistry::disarm_timer(TuiTimerId timer_id) noexcept
{
  if (auto timer = timers_.find(timer_id); timer != timers_.end())
    timer->second.disarm();
}

bool TuiTimerRegistry::timer_armed(TuiTimerId timer_id) const noexcept
{
  auto const timer = timers_.find(timer_id);
  return timer != timers_.end() && timer->second.deadline().has_value();
}

void TuiTimerRegistry::arm_timer_at(TuiTimerId timer_id, Clock::time_point deadline)
{
  if (auto timer = timers_.find(timer_id); timer != timers_.end())
    timer->second.arm_at(deadline);
}

void TuiTimerRegistry::arm_timer_after(TuiTimerId timer_id, Clock::duration delay, Clock::time_point now)
{
  if (auto timer = timers_.find(timer_id); timer != timers_.end())
    timer->second.arm_after(delay, now);
}

void TuiTimerRegistry::arm_timer_every(TuiTimerId timer_id, Clock::duration interval, Clock::time_point now)
{
  if (auto timer = timers_.find(timer_id); timer != timers_.end())
    timer->second.arm_every(interval, now);
}

void TuiTimerRegistry::service_due(Clock::time_point now)
{
  // Do not call service_due from a timer callback; arm it for the next loop turn instead.
  ASSERT(!servicing_due_);
  if (servicing_due_)
    return;

  servicing_due_ = true;
  try
  {
    // Fix this pass's eligible work before invoking clients: callbacks can add,
    // remove or reschedule timers, and newly armed work belongs to a later pass.
    std::vector<std::pair<TuiTimerId, std::uint64_t>> due;
    due.reserve(timers_.size());
    for (auto const& [timer_id, timer] : timers_)
    {
      if (timer.due(now))
        due.emplace_back(timer_id, timer.generation());
    }
    for (auto const& [timer_id, generation] : due)
    {
      TuiTimer::Callback callback;
      {
        // Re-find by ID and generation because an earlier callback may have
        // invalidated this entry even though it was due when the pass began.
        auto timer = timers_.find(timer_id);
        if (timer == timers_.end() || timer->second.generation() != generation || !timer->second.due(now))
          continue;
        callback = timer->second.prepare_dispatch(now);
      }
      // No registry reference/iterator survives this call. The callback may
      // disarm, rearm or destroy any timer, including its own.
      callback(now);
    }
  }
  catch (...)
  {
    // A throwing callback must not leave the registry permanently marked as inside
    // dispatch if the session catches the exception and performs cleanup.
    servicing_due_ = false;
    throw;
  }
  servicing_due_ = false;
}

bool TuiTimerRegistry::has_due_work(Clock::time_point now) const noexcept
{
  for (auto const& [timer_id, timer] : timers_)
  {
    if (timer.due(now))
      return true;
  }
  return false;
}

std::optional<TuiTimerRegistry::Clock::time_point> TuiTimerRegistry::next_deadline() const noexcept
{
  // Only armed timers constrain sleep.
  std::optional<Clock::time_point> deadline;
  for (auto const& [timer_id, timer] : timers_)
  {
    auto const next = timer.deadline();
    if (next && (!deadline || *next < *deadline))
      deadline = next;
  }
  return deadline;
}

} // namespace ava::tui
