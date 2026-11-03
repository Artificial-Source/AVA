#pragma once

#include "ava/debug/print_members_on.h"

#include <functional>
#include <initializer_list>
#include <optional>
#include <vector>

namespace ava::tui {
namespace runtime_input {
struct RuntimeInput;
}
struct InputEvent;

enum class TuiInputOutcome
{
  Pass,
  Handled,
  Exit,
  Failure
};

// Composition supplies handlers in modal priority order. A handler which returns
// Pass must leave input available to the next owner; every other result stops
// dispatch. Targets outlive the dispatcher and are never invoked during teardown.
class TuiInputDispatcher final
{
 public:
  using Handler = std::function<TuiInputOutcome(runtime_input::RuntimeInput const&)>;
  using EventHandler = std::function<std::optional<bool>(InputEvent const&)>;

  explicit TuiInputDispatcher(std::initializer_list<Handler> handlers);
  [[nodiscard]] TuiInputOutcome dispatch(runtime_input::RuntimeInput const& input) const;
  // Existing modal handlers use an optional render-success result: no value
  // passes input on, while a value consumes it and reports the render outcome.
  [[nodiscard]] static Handler event_handler(EventHandler handler);

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  std::vector<Handler> handlers_;
};

} // namespace ava::tui
