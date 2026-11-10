// Transfers and removes timers on the TUI thread, leaving
// deadline policy and callback dispatch to the registry-owned TuiTimer.
#include "sys.h"
#include "TuiTimerHandle.h"
#include "TuiTimerRegistry.h"

#include <utility>

namespace ava::tui {

TuiTimerHandle::TuiTimerHandle(TuiTimerRegistry& timer_registry, TuiTimerId timer_id) noexcept : registry_(&timer_registry), timer_id_(timer_id)
{
}

TuiTimerHandle::~TuiTimerHandle()
{
  release();
}

TuiTimerHandle::TuiTimerHandle(TuiTimerHandle&& other) noexcept
    : registry_(std::exchange(other.registry_, nullptr)), timer_id_(std::exchange(other.timer_id_, 0))
{
}

TuiTimerHandle& TuiTimerHandle::operator=(TuiTimerHandle&& other) noexcept
{
  if (this != &other)
  {
    // Remove this handle's old callback before transferring ownership, so move
    // assignment cannot strand a timer that still borrows a feature target.
    release();
    registry_ = std::exchange(other.registry_, nullptr);
    timer_id_ = std::exchange(other.timer_id_, 0);
  }
  return *this;
}

void TuiTimerHandle::release() noexcept
{
  if (registry_)
    registry_->remove_timer(timer_id_);
  registry_ = nullptr;
  timer_id_ = 0;
}

void TuiTimerHandle::disarm() noexcept
{
  if (registry_)
    registry_->disarm_timer(timer_id_);
}

bool TuiTimerHandle::armed() const noexcept
{
  return registry_ && registry_->timer_armed(timer_id_);
}

void TuiTimerHandle::arm_at(Clock::time_point deadline)
{
  if (registry_)
    registry_->arm_timer_at(timer_id_, deadline);
}

void TuiTimerHandle::arm_after(Clock::duration delay, Clock::time_point now)
{
  if (registry_)
    registry_->arm_timer_after(timer_id_, delay, now);
}

void TuiTimerHandle::arm_every(Clock::duration interval, Clock::time_point now)
{
  if (registry_)
    registry_->arm_timer_every(timer_id_, interval, now);
}

} // namespace ava::tui
