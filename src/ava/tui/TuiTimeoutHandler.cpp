#include "sys.h"
#include "TuiSession.h"
#include "TuiTimeoutHandler.h"

namespace ava::tui {

std::optional<TuiEventLoop::Clock::time_point> TuiTimeoutHandler::next_deadline() noexcept
{
  return session_->next_timer_deadline();
}

void TuiTimeoutHandler::expire(TuiEventLoop::Clock::time_point now)
{
  session_->expire_timers(now);
}

} // namespace ava::tui
