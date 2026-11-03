// Publishes spinner phases at timer cadence; the session later paints any invalidated frame.
#include "sys.h"
#include "Spinner.h"
#include "composer_internal.h"

#include <utility>
#include "debug.h"

namespace ava::tui {

Spinner::Spinner(TuiEventLoop& loop, std::function<void(std::size_t)> publish_frame)
    : publish_frame_(std::move(publish_frame)), timer_(loop.member_timer(*this, &Spinner::tick))
{
  // Supply a live presentation member binding; an empty callback cannot publish animation frames.
  ASSERT(publish_frame_);
}

void Spinner::start(std::size_t current_frame)
{
  frame_ = current_frame;
  if (publish_frame_)
    timer_.repeat(detail::kProcessingIndicatorFrameDelay);
}

void Spinner::stop()
{
  timer_.cancel();
}

bool Spinner::active() const
{
  return timer_.active();
}

void Spinner::tick(TuiEventLoop::Clock::time_point)
{
  ++frame_;
  publish_frame_(frame_);
}

} // namespace ava::tui
