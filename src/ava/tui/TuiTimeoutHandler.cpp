#include "sys.h"
#include "TuiTimeoutHandler.h"

#include <atomic>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "debug.h"
#if CW_DEBUG
#include "utils/at_scope_end.h"
#endif

namespace ava::tui {

TuiTimeoutHandler::~TuiTimeoutHandler()
{
  // Destroy every TuiTimerHandle before its registry, including disarmed handles; declare the registry before its clients.
  ASSERT(timers_ts::crat(timers_)->empty());
}

TuiTimerHandle TuiTimeoutHandler::register_timer(TuiTimer::Callback callback) /*threadsafe-*/ const
{
  // Claim the next ID, but never move past the last one: once exhausted, every later call throws too.
  // Relaxed is enough, because the ID only has to be unique; the map insertion below is ordered by its lock.
  TuiTimerId timer_id = next_timer_id_.load(std::memory_order_relaxed);
  do
  {
    if (timer_id == std::numeric_limits<TuiTimerId>::max())
      throw std::overflow_error("TUI timer registration IDs exhausted");
  }
  while (!next_timer_id_.compare_exchange_weak(timer_id, timer_id + 1, std::memory_order_relaxed));
  timers_ts::wat(timers_)->try_emplace(timer_id, std::move(callback));
  return TuiTimerHandle(*this, timer_id);
}

void TuiTimeoutHandler::remove_timer(TuiTimerId timer_id) /*threadsafe-*/ const noexcept
{
  timers_ts::wat(timers_)->erase(timer_id);
}

void TuiTimeoutHandler::disarm_timer(TuiTimerId timer_id) /*threadsafe-*/ const noexcept
{
  timers_ts::wat timers_w(timers_);
  if (auto timer = timers_w->find(timer_id); timer != timers_w->end())
    timer->second.disarm();
}

bool TuiTimeoutHandler::timer_armed(TuiTimerId timer_id) /*threadsafe-*/ const noexcept
{
  timers_ts::crat timers_r(timers_);
  auto const timer = timers_r->find(timer_id);
  return timer != timers_r->end() && timer->second.deadline().has_value();
}

void TuiTimeoutHandler::arm_timer_at(TuiTimerId timer_id, Clock::time_point deadline) /*threadsafe-*/ const
{
  timers_ts::wat timers_w(timers_);
  if (auto timer = timers_w->find(timer_id); timer != timers_w->end())
    timer->second.arm_at(deadline);
}

void TuiTimeoutHandler::arm_timer_after(TuiTimerId timer_id, Clock::duration delay, Clock::time_point now) /*threadsafe-*/ const
{
  timers_ts::wat timers_w(timers_);
  if (auto timer = timers_w->find(timer_id); timer != timers_w->end())
    timer->second.arm_after(delay, now);
}

void TuiTimeoutHandler::arm_timer_every(TuiTimerId timer_id, Clock::duration interval, Clock::time_point now) /*threadsafe-*/ const
{
  timers_ts::wat timers_w(timers_);
  if (auto timer = timers_w->find(timer_id); timer != timers_w->end())
    timer->second.arm_every(interval, now);
}

void TuiTimeoutHandler::expire(Clock::time_point now)
{
#if CW_DEBUG
  // Do not call expire from a timer callback; arm it for the next loop turn instead.
  ASSERT(!inside_expire_);
  inside_expire_ = true;
  auto&& reset_servicing_due = at_scope_end([this]() { inside_expire_ = false; });
#endif

  // Fix this pass's eligible work before invoking clients: callbacks can add,
  // remove or reschedule timers, and newly armed work belongs to a later pass.
  std::vector<std::pair<TuiTimerId, std::uint64_t>> due;
  {
    timers_ts::crat timers_r(timers_);
    due.reserve(timers_r->size());
    for (auto const& [timer_id, timer] : *timers_r)
    {
      if (timer.due(now))
        due.emplace_back(timer_id, timer.generation());
    }
  }
  for (auto const& [timer_id, generation] : due)
  {
    TuiTimer::Callback callback;
    {
      // Re-find by ID and generation because an earlier callback may have
      // invalidated this entry even though it was due when the pass began.
      timers_ts::wat timers_w(timers_);
      auto timer = timers_w->find(timer_id);
      if (timer == timers_w->end() || timer->second.generation() != generation || !timer->second.due(now))
        continue;
      callback = timer->second.prepare_dispatch(now);
    }
    // No lock, registry reference or iterator survives this call. The callback may
    // create a timer or disarm, rearm or destroy any timer, including its own.
    callback(now);
  }
}

bool TuiTimeoutHandler::has_due_work(Clock::time_point now) noexcept
{
  timers_ts::crat timers_r(timers_);
  for (auto const& [timer_id, timer] : *timers_r)
  {
    if (timer.due(now))
      return true;
  }
  return false;
}

std::optional<TuiTimeoutHandler::Clock::time_point> TuiTimeoutHandler::next_deadline() noexcept
{
  // Only armed timers constrain sleep.
  std::optional<Clock::time_point> deadline;
  timers_ts::crat timers_r(timers_);
  for (auto const& [timer_id, timer] : *timers_r)
  {
    auto const next = timer.deadline();
    if (next && (!deadline || *next < *deadline))
      deadline = next;
  }
  return deadline;
}

} // namespace ava::tui
