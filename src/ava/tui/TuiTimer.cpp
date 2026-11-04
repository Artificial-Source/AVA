// Transfers and removes timer registrations on the TUI thread, leaving
// deadline policy and callback dispatch to the loop-owned registration.
#include "sys.h"
#include "TuiEventLoop.h"
#include "TuiTimer.h"

#include <utility>

namespace ava::tui {

TuiTimer::TuiTimer(TuiEventLoop& loop, std::uint64_t id) noexcept : loop_(&loop), id_(id)
{
}

TuiTimer::~TuiTimer()
{
  release();
}

TuiTimer::TuiTimer(TuiTimer&& other) noexcept : loop_(std::exchange(other.loop_, nullptr)), id_(std::exchange(other.id_, 0))
{
}

TuiTimer& TuiTimer::operator=(TuiTimer&& other) noexcept
{
  if (this != &other)
  {
    // Remove this handle's old callback before transferring ownership, so move
    // assignment cannot strand a registration that still borrows a feature target.
    release();
    loop_ = std::exchange(other.loop_, nullptr);
    id_ = std::exchange(other.id_, 0);
  }
  return *this;
}

void TuiTimer::release() noexcept
{
  if (loop_)
    loop_->remove_timer(id_);
  loop_ = nullptr;
  id_ = 0;
}

void TuiTimer::cancel() noexcept
{
  if (loop_)
    loop_->cancel_timer(id_);
}

bool TuiTimer::active() const noexcept
{
  return loop_ && loop_->timer_active(id_);
}

void TuiTimer::arm_at(Clock::time_point deadline)
{
  if (loop_)
    loop_->arm_timer_at(id_, deadline);
}

void TuiTimer::arm_after(Clock::duration delay, Clock::time_point now)
{
  if (loop_)
    loop_->arm_timer_after(id_, delay, now);
}

void TuiTimer::repeat(Clock::duration interval, Clock::time_point now)
{
  if (loop_)
    loop_->repeat_timer(id_, interval, now);
}

} // namespace ava::tui
