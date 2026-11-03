#include "sys.h"
#include "TuiWaitState.h"
#include "TuiWakeup.h"

namespace ava::tui {

TuiWakeup::TuiWakeup(std::shared_ptr<TuiWaitState> const& state) noexcept : state_(state)
{
}

void TuiWakeup::notify() const noexcept
{
  if (auto state = state_.lock())
    state->notify();
}

} // namespace ava::tui
