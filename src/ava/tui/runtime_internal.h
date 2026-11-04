// Shared TUI command and selector handoffs used by the feature owners.
#pragma once

#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_commands_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/core/NoLogString.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "debug.h"

namespace ava::tui {
class RuntimePresentationState;

class SelectorHost;

enum class TuiActiveNonblockingCommandDispatchKind
{
  Unrecognized,
  Blocked,
  Handled,
};

// Internal active-run dispatch boundary. It keeps completion-state rejection
// ahead of the queue callback that can mutate active job state.
struct TuiActiveNonblockingCommandDispatchResult
{
  TuiActiveNonblockingCommandDispatchKind kind = TuiActiveNonblockingCommandDispatchKind::Unrecognized;
  ava::core::NoLogString status = {};
  std::vector<ava::core::NoLogString> output = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::optional<std::vector<std::string>> dispatch_tui_active_nonblocking_command(TuiActiveRunQueues const& queues, std::string const& submitted);
[[nodiscard]] TuiActiveNonblockingCommandDispatchResult dispatch_tui_active_nonblocking_command_gated(ComposerSnapshot const& completion_snapshot,
                                                                                                      TuiActiveRunQueues const& queues,
                                                                                                      std::string const& submitted);

// TUI thread: close the selector and paint pending status before a synchronous
// model/session callback can block. render must perform the handoff paint, not
// merely request a frame. Failure returns an I/O error without calling callback.
// Otherwise return the backend result and display any error; the caller applies
// success. Borrowed owners and callbacks need only outlive this synchronous call.
[[nodiscard]] ava::core::Result<std::string> dispatch_tui_selector_authority(RuntimePresentationState& presentation, SelectorHost& selectors,
                                                                             std::string pending_status, std::function<bool()> const& render,
                                                                             std::function<ava::core::Result<std::string>()> const& callback);

// Presentation-only state transitions used by the interactive reasoning-cycle path.
void apply_reasoning_cycle_success(ComposerSnapshot& snapshot, std::string feedback);
void clear_reasoning_feedback_for_user_input(ComposerSnapshot& snapshot);

}  // namespace ava::tui
