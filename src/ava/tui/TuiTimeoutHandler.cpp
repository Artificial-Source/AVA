#include "sys.h"
#include "TuiTimeoutHandler.h"

#include <atomic>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include "debug.h"
#if CW_DEBUG
#include "utils/at_scope_end.h"
#endif

namespace ava::tui {

void TuiTimeoutHandler::Timers::reindex(by_id_type::iterator timer, std::optional<Clock::time_point> old_deadline)
{
  std::optional<Clock::time_point> const new_deadline = timer->second.deadline();
  if (new_deadline == old_deadline)
    return;
  if (new_deadline)
  {
    try
    {
      by_deadline.emplace(*new_deadline, timer->first);
    }
    catch (...)
    {
      // Out of memory: leave the timer disarmed, which by_deadline then correctly reflects.
      timer->second.disarm();
      if (old_deadline)
        by_deadline.erase({*old_deadline, timer->first});
      throw;
    }
  }
  if (old_deadline)
    by_deadline.erase({*old_deadline, timer->first});
}

bool TuiTimeoutHandler::Timers::must_wake_for(Clock::time_point deadline)
{
  // The TUI thread is awake when it arms a timer itself, and calls `expire` before it sleeps again.
  if (deadline >= sleeping_until || std::this_thread::get_id() == tui_thread)
    return false;
  // The TUI thread will call `expire` after this wakeup, which records a new sleeping_until.
  sleeping_until = Clock::time_point::min();
  return true;
}

std::optional<TuiTimeoutHandler::Clock::time_point> TuiTimeoutHandler::Timers::record_sleep() noexcept
{
  std::optional<Clock::time_point> deadline;
  if (!by_deadline.empty())
    deadline = by_deadline.begin()->first;
  sleeping_until = deadline.value_or(Clock::time_point::max());
  return deadline;
}

TuiTimeoutHandler::~TuiTimeoutHandler()
{
  // Destroy every TuiTimerHandle before its registry, including disarmed handles; declare the registry before its clients.
  ASSERT(timers_ts::crat(timers_)->by_id.empty());
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
  timers_ts::wat(timers_)->by_id.try_emplace(timer_id, std::move(callback));
  return TuiTimerHandle(*this, timer_id);
}

void TuiTimeoutHandler::remove_timer(TuiTimerId timer_id) /*threadsafe-*/ const noexcept
{
  timers_ts::wat timers_w(timers_);
  auto const timer = timers_w->by_id.find(timer_id);
  if (timer == timers_w->by_id.end())
    return;
  if (auto const deadline = timer->second.deadline())
    timers_w->by_deadline.erase({*deadline, timer_id});
  timers_w->by_id.erase(timer);
}

void TuiTimeoutHandler::disarm_timer(TuiTimerId timer_id) /*threadsafe-*/ const noexcept
{
  timers_ts::wat timers_w(timers_);
  auto const timer = timers_w->by_id.find(timer_id);
  if (timer == timers_w->by_id.end())
    return;
  if (auto const deadline = timer->second.deadline())
    timers_w->by_deadline.erase({*deadline, timer_id});
  timer->second.disarm();
}

bool TuiTimeoutHandler::timer_armed(TuiTimerId timer_id) /*threadsafe-*/ const noexcept
{
  timers_ts::crat timers_r(timers_);
  auto const timer = timers_r->by_id.find(timer_id);
  return timer != timers_r->by_id.end() && timer->second.deadline().has_value();
}

template <typename Arm>
void TuiTimeoutHandler::arm_timer(TuiTimerId timer_id, Arm&& arm) /*threadsafe-*/ const
{
  // Notify after releasing the lock; an inert token does nothing.
  TuiWakeup wakeup;
  {
    timers_ts::wat timers_w(timers_);
    auto const timer = timers_w->by_id.find(timer_id);
    if (timer == timers_w->by_id.end())
      return;
    std::optional<Clock::time_point> const old_deadline = timer->second.deadline();
    std::forward<Arm>(arm)(timer->second);
    timers_w->reindex(timer, old_deadline);
    if (auto const deadline = timer->second.deadline(); deadline && timers_w->must_wake_for(*deadline))
      wakeup = timers_w->wakeup;
  }
  wakeup.notify();
}

void TuiTimeoutHandler::arm_timer_at(TuiTimerId timer_id, Clock::time_point deadline) /*threadsafe-*/ const
{
  arm_timer(timer_id, [deadline](TuiTimer& timer) { timer.arm_at(deadline); });
}

void TuiTimeoutHandler::arm_timer_after(TuiTimerId timer_id, Clock::duration delay, Clock::time_point now) /*threadsafe-*/ const
{
  arm_timer(timer_id, [delay, now](TuiTimer& timer) { timer.arm_after(delay, now); });
}

void TuiTimeoutHandler::arm_timer_every(TuiTimerId timer_id, Clock::duration interval, Clock::time_point now) /*threadsafe-*/ const
{
  arm_timer(timer_id, [interval, now](TuiTimer& timer) { timer.arm_every(interval, now); });
}

void TuiTimeoutHandler::attach(TuiWakeup wakeup)
{
  timers_ts::wat timers_w(timers_);
  timers_w->tui_thread = std::this_thread::get_id();
  timers_w->wakeup = std::move(wakeup);
}

void TuiTimeoutHandler::stop() noexcept
{
  stopping_ = true;
  // No timer runs anymore, so there is no point in waking the TUI thread for one.
  timers_ts::wat(timers_)->sleeping_until = Clock::time_point::min();
}

std::optional<TuiTimeoutHandler::Clock::time_point> TuiTimeoutHandler::expire(Clock::time_point now)
{
#if CW_DEBUG
  // Do not call expire from a timer callback; arm it for the next loop turn instead.
  ASSERT(!inside_expire_);
  inside_expire_ = true;
  auto&& reset_inside_expire = at_scope_end([this]() { inside_expire_ = false; });
#endif

  if (stopping_)
    return std::nullopt;

  // Fix this pass's eligible work before invoking clients: callbacks can add,
  // remove or reschedule timers, and newly armed work belongs to a later pass.
  // Only the due prefix of by_deadline is visited.
  std::vector<std::pair<TuiTimerId, std::uint64_t>> due;
  {
    timers_ts::wat timers_w(timers_);
    for (auto const& [deadline, timer_id] : timers_w->by_deadline)
    {
      if (deadline > now)
        break;
      due.emplace_back(timer_id, timers_w->by_id.find(timer_id)->second.generation());
    }
    // The common case: nothing is due, so record and return the next deadline under this same lock.
    if (due.empty())
      return timers_w->record_sleep();
  }
  for (auto const& [timer_id, generation] : due)
  {
    TuiTimer::Callback callback;
    {
      // Re-find by ID and generation because an earlier callback may have
      // invalidated this entry even though it was due when the pass began.
      timers_ts::wat timers_w(timers_);
      auto const timer = timers_w->by_id.find(timer_id);
      if (timer == timers_w->by_id.end() || timer->second.generation() != generation || !timer->second.due(now))
        continue;
      std::optional<Clock::time_point> const old_deadline = timer->second.deadline();
      callback = timer->second.prepare_dispatch(now);
      timers_w->reindex(timer, old_deadline);
    }
    // No lock, registry reference or iterator survives this call. The callback may
    // create a timer or disarm, rearm or destroy any timer, including its own.
    callback(now);
  }
  // A callback may have called `stop`.
  if (stopping_)
    return std::nullopt;
  return timers_ts::wat(timers_)->record_sleep();
}

std::optional<TuiTimeoutHandler::Clock::time_point> TuiTimeoutHandler::next_deadline() noexcept
{
  if (stopping_)
    return std::nullopt;
  return timers_ts::wat(timers_)->record_sleep();
}

bool TuiTimeoutHandler::has_due_work(Clock::time_point now) noexcept
{
  if (stopping_)
    return false;
  timers_ts::crat timers_r(timers_);
  return !timers_r->by_deadline.empty() && timers_r->by_deadline.begin()->first <= now;
}

} // namespace ava::tui
