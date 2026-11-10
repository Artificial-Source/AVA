// Updates the spinner phase on timer ticks; TuiSession paints the requested frame later.
#include "sys.h"
#include "Spinner.h"
#include "composer_internal.h"

#include <utility>
#include "debug.h"

namespace ava::tui {

Spinner::Spinner(TuiTimerRegistry& timer_registry, std::function<void(std::size_t)> publish_frame)
    : publish_frame_(std::move(publish_frame)), timer_(timer_registry.create_timer(*this, &Spinner::tick))
{
  // Supply a live presentation member binding; an empty callback cannot publish animation frames.
  ASSERT(publish_frame_);
}

void Spinner::start(std::size_t current_frame)
{
  frame_ = current_frame;
  if (publish_frame_)
    timer_.arm_every(detail::kProcessingIndicatorFrameDelay);
}

void Spinner::stop()
{
  timer_.disarm();
}

bool Spinner::active() const
{
  return timer_.armed();
}

void Spinner::tick(TuiTimerRegistry::Clock::time_point)
{
  ++frame_;
  publish_frame_(frame_);
}

} // namespace ava::tui
