// Wakes the TUI thread's event loop, keeping it alive through the notification with a temporary shared reference.
#include "sys.h"
#include "TuiWakeup.h"

#include <utility>

namespace ava::tui {

TuiWakeup::TuiWakeup(std::weak_ptr<utils::threading::EventLoop> event_loop, utils::threading::EventLoop::Event event) noexcept
    : event_loop_(std::move(event_loop)), event_(event)
{
}

void TuiWakeup::notify() const noexcept
{
  // Keep the local shared owner through notify. Releasing it may destroy the loop on this producer thread.
  // EventLoop::notify only throws when writing to the loop's own eventfd fails, which requires an invalid
  // descriptor and therefore cannot happen while we hold a reference; this noexcept function would terminate.
  if (auto event_loop = event_loop_.lock())
    event_loop->notify(event_);
}

} // namespace ava::tui
