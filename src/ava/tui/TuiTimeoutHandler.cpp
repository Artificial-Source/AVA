#include "sys.h"
#include "TuiTimeoutHandler.h"

#include <chrono>

namespace ava::tui {

namespace {
// Signals only set atomic bits, and the OS wait retries EINTR. This retained
// timer supplies a nominal 100 ms observation cadence (ten idle wakes/second).
// Decode, callbacks and synchronous work can delay the next control turn.
// The timer deadline wakes the loop so the next control stage can observe those bits.
constexpr auto kControlObservationInterval = std::chrono::milliseconds(100);
} // namespace

TuiTimeoutHandler::TuiTimeoutHandler() : control_timer_(timer_registry_.create_timer(*this, &TuiTimeoutHandler::wake_for_control))
{
}

void TuiTimeoutHandler::arm_control_timer(TuiTimerRegistry::Clock::time_point epoch)
{
  control_timer_.arm_every(kControlObservationInterval, epoch);
}

void TuiTimeoutHandler::wake_for_control(TuiTimerRegistry::Clock::time_point)
{
  // The timer deadline already wakes wait(). Control actions belong to the
  // next control stage, never to a due-timer callback.
}

std::optional<TuiTimerRegistry::Clock::time_point> TuiTimeoutHandler::next_deadline() noexcept
{
  // After stop() no timer runs anymore; do not keep waking the TUI thread.
  if (stopping_)
    return std::nullopt;
  return timer_registry_.next_deadline();
}

void TuiTimeoutHandler::expire(TuiTimerRegistry::Clock::time_point now)
{
  if (stopping_)
    return;
  timer_registry_.service_due(now);
}

} // namespace ava::tui
