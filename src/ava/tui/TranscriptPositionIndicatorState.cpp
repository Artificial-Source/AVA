// Tracks the indicator deadline without owning a timer; RuntimeRenderer schedules the wake.
// Implements the transcript indicator deadline used by the renderer timer.
#include "sys.h"
#include "TranscriptPositionIndicatorState.h"

namespace ava::tui {

void TranscriptPositionIndicatorState::show(Clock::time_point now) noexcept
{
  expires_at_ = now + kVisibleDuration;
}
void TranscriptPositionIndicatorState::hide() noexcept
{
  expires_at_.reset();
}
bool TranscriptPositionIndicatorState::visible() const noexcept
{
  return expires_at_.has_value();
}
bool TranscriptPositionIndicatorState::expire_if_due(Clock::time_point now) noexcept
{
  if (!expires_at_ || now < *expires_at_)
    return false;
  expires_at_.reset();
  return true;
}
std::optional<TranscriptPositionIndicatorState::Clock::time_point> TranscriptPositionIndicatorState::expires_at() const noexcept
{
  return expires_at_;
}

} // namespace ava::tui
