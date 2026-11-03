// Applies modal priority to decoded input and adapts legacy optional paint results
// to the session's explicit handled/exit/failure outcomes.
#include "sys.h"
#include "TuiInputDispatcher.h"
#include "runtime_input_internal.h"

#include <utility>

namespace ava::tui {

TuiInputDispatcher::TuiInputDispatcher(std::initializer_list<Handler> handlers) : handlers_(handlers)
{
}

TuiInputOutcome TuiInputDispatcher::dispatch(runtime_input::RuntimeInput const& input) const
{
  for (auto const& handler : handlers_)
  {
    auto const outcome = handler(input);
    if (outcome != TuiInputOutcome::Pass)
      return outcome;
  }
  return TuiInputOutcome::Pass;
}

TuiInputDispatcher::Handler TuiInputDispatcher::event_handler(EventHandler handler)
{
  return [handler = std::move(handler)](runtime_input::RuntimeInput const& input) {
    auto const outcome = handler(input.event);
    if (!outcome)
      return TuiInputOutcome::Pass;
    return *outcome ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
  };
}

} // namespace ava::tui
