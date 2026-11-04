// Routes one decoded input through the session's ordered modal and feature handlers.
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

// TUI-thread handler result; dispatch stops on the first result other than Pass.
enum class TuiInputOutcome
{
  // This owner did not consume the event; the next handler may handle it.
  Pass,
  // This owner consumed the event successfully; lower-priority handlers must not see it.
  Handled,
  // The event requests session exit; TuiSession must leave the input stage.
  Exit,
  // Handling failed, including a legacy paint failure; TuiSession terminates the turn.
  Failure
};

// TuiSession and ActiveRunInput own TUI-thread-only dispatchers with handlers in modal
// priority order. It owns callable bindings but borrows their targets, which must
// outlive it and any dispatch in progress. Pass leaves input unconsumed for the next
// owner; every other result stops dispatch. Destruction only releases bindings and
// never invokes them; it must happen outside an active dispatch.
class TuiInputDispatcher final
{
 public:
  // TUI-thread callable borrowing input only for the call; may consume it by
  // returning a non-Pass result. Captured targets must outlive the dispatcher.
  using Handler = std::function<TuiInputOutcome(runtime_input::RuntimeInput const&)>;
  // Legacy TUI-thread modal binding: nullopt declines input, true consumes it
  // successfully, and false consumes it with a paint/handling failure.
  using EventHandler = std::function<std::optional<bool>(InputEvent const&)>;

  // TUI thread: copy nonempty handler bindings in priority order. Their borrowed
  // targets must be fully constructed before the first dispatch and outlive this owner.
  explicit TuiInputDispatcher(std::initializer_list<Handler> handlers);
  // TUI thread with all targets alive: offer input in order until a handler returns
  // something other than Pass. Input is borrowed only until return; all-Pass leaves
  // it unhandled, and Exit or Failure is returned unchanged for TuiSession to act on.
  [[nodiscard]] TuiInputOutcome dispatch(runtime_input::RuntimeInput const& input) const;
  // TUI thread: wrap a nonempty legacy binding without invoking it. The returned
  // handler borrows only input.event on dispatch and maps optional render success
  // to Pass/Handled/Failure; the original binding's targets still must outlive use.
  [[nodiscard]] static Handler event_handler(EventHandler handler);

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Initialized once on the TUI thread, then visited in stable modal priority order.
  // Owns bindings but never extends the lifetime of their borrowed feature targets.
  std::vector<Handler> handlers_;
};

} // namespace ava::tui
