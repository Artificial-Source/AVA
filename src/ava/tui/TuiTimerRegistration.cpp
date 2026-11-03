#include "sys.h"
#include "TuiTimerRegistration.h"

#include <type_traits>
#include <utility>
#include "debug.h"

namespace ava::tui {
namespace {

TuiTimerRegistration::Clock::time_point saturated_deadline(TuiTimerRegistration::Clock::time_point now, TuiTimerRegistration::Clock::duration delay)
{
  using Clock = TuiTimerRegistration::Clock;
  if (delay <= Clock::duration::zero())
    return now;
  if (now.time_since_epoch() > Clock::duration::max() - delay)
    return Clock::time_point::max();
  return now + delay;
}

} // namespace

TuiTimerRegistration::TuiTimerRegistration(Callback callback) : callback_(std::move(callback))
{
}

bool TuiTimerRegistration::due(Clock::time_point now) const noexcept
{
  return deadline_ && *deadline_ <= now;
}

void TuiTimerRegistration::cancel() noexcept
{
  ++generation_;
  deadline_.reset();
  interval_ = Clock::duration::zero();
}

void TuiTimerRegistration::arm_at(Clock::time_point deadline)
{
  ++generation_;
  deadline_ = deadline;
  interval_ = Clock::duration::zero();
}

void TuiTimerRegistration::arm_after(Clock::duration delay, Clock::time_point now)
{
  arm_at(saturated_deadline(now, delay));
}

void TuiTimerRegistration::repeat(Clock::duration interval, Clock::time_point now)
{
  // Supply a positive repeat interval; use arm_at(now) for a one-shot immediate callback.
  ASSERT(interval > Clock::duration::zero());
  if (interval <= Clock::duration::zero())
  {
    cancel();
    return;
  }
  ++generation_;
  deadline_ = saturated_deadline(now, interval);
  interval_ = interval;
}

TuiTimerRegistration::Callback TuiTimerRegistration::prepare_dispatch(Clock::time_point now)
{
  // Dispatch only a registration whose deadline is due; recheck its generation in the loop first.
  ASSERT(due(now));
  if (!due(now))
    return {};

  auto callback = callback_;
  ++generation_;
  if (interval_ > Clock::duration::zero() && now < Clock::time_point::max())
  {
    // Unsigned subtraction also covers the full signed clock range without
    // overflowing when an explicit timestamp is far beyond the prior deadline.
    using UnsignedRep = std::make_unsigned_t<Clock::duration::rep>;
    auto const elapsed = static_cast<UnsignedRep>(now.time_since_epoch().count()) - static_cast<UnsignedRep>(deadline_->time_since_epoch().count());
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
