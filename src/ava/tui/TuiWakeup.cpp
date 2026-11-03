// Implements producer notification with a temporary shared pin of OS resources.
#include "sys.h"
#include "TuiWaitState.h"
#include "TuiWakeup.h"

namespace ava::tui {

TuiWakeup::TuiWakeup(std::shared_ptr<TuiWaitState> const& state) noexcept : state_(state)
{
}

void TuiWakeup::notify() const noexcept
{
  // Keep the local shared owner through notify. Releasing it may destroy the
  // waiter on this producer thread; it gives no lifetime protection to UI objects.
  if (auto state = state_.lock())
    state->notify();
}

} // namespace ava::tui
