// Maintains timer deadlines and dispatch generations, skipping missed repeating
// ticks without calling feature code or retaining registry iterators.
#include "sys.h"
#include "TuiTimer.h"

#include <type_traits>
#include <utility>
#include "debug.h"

namespace ava::tui {
namespace {

// Used by TUI-thread scheduling to avoid signed overflow for caller-supplied times.
// Nonpositive delay means immediate eligibility; an unrepresentable future deadline
// saturates at the clock limit. Pure calculation with no borrowed object lifetime.
TuiTimer::Clock::time_point saturated_deadline(TuiTimer::Clock::time_point now, TuiTimer::Clock::duration delay)
{
  using Clock = TuiTimer::Clock;
  if (delay <= Clock::duration::zero())
    return now;
  if (now.time_since_epoch() > Clock::duration::max() - delay)
    return Clock::time_point::max();
  return now + delay;
}

} // namespace

TuiTimer::TuiTimer(Callback callback) : callback_(std::move(callback))
{
}

bool TuiTimer::due(Clock::time_point now) const noexcept
{
  return deadline_ && *deadline_ <= now;
}

void TuiTimer::disarm() noexcept
{
  ++generation_;
  deadline_.reset();
  interval_ = Clock::duration::zero();
}

void TuiTimer::arm_at(Clock::time_point deadline)
{
  ++generation_;
  deadline_ = deadline;
  interval_ = Clock::duration::zero();
}

void TuiTimer::arm_after(Clock::duration delay, Clock::time_point now)
{
  arm_at(saturated_deadline(now, delay));
}

void TuiTimer::arm_every(Clock::duration interval, Clock::time_point now)
{
  // Supply a positive repeat interval; use arm_at(now) for a one-shot immediate callback.
  ASSERT(interval > Clock::duration::zero());
  if (interval <= Clock::duration::zero())
  {
    disarm();
    return;
  }
  ++generation_;
  deadline_ = saturated_deadline(now, interval);
  interval_ = interval;
}

TuiTimer::Callback TuiTimer::prepare_dispatch(Clock::time_point now)
{
  // Dispatch only a timer whose deadline is due; recheck its generation in the registry first.
  ASSERT(due(now));
  if (!due(now))
    return {};

  // Copy before changing the schedule so allocation failure leaves the old state
  // intact. The copy can survive removal of this record by the eventual callback.
  auto callback = callback_;
  ++generation_;
  if (interval_ > Clock::duration::zero() && now < Clock::time_point::max())
  {
    // Unsigned subtraction also covers the full signed clock range without
    // overflowing when an explicit timestamp is far beyond the prior deadline.
    using UnsignedRep = std::make_unsigned_t<Clock::duration::rep>;
    auto const elapsed = static_cast<UnsignedRep>(now.time_since_epoch().count()) - static_cast<UnsignedRep>(deadline_->time_since_epoch().count());
    // Keep the original interval phase while skipping all missed ticks. Scheduling
    // from now + interval instead would accumulate drift whenever dispatch is late.
    auto const remainder = elapsed % static_cast<UnsignedRep>(interval_.count());
    auto const until_next = interval_ - Clock::duration(static_cast<Clock::duration::rep>(remainder));
    deadline_ = saturated_deadline(now, until_next);
  }
  else
  {
    // There is no representable future tick after time_point::max().
    deadline_.reset();
    interval_ = Clock::duration::zero();
  }
  return callback;
}

} // namespace ava::tui
