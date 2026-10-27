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

// Clears a selector and paints truthful pending authority state before invoking
// a synchronous model/session callback. The callback remains authoritative;
// this boundary never applies or claims its result.
[[nodiscard]] ava::core::Result<std::string> dispatch_tui_selector_authority(ComposerSnapshot& snapshot, std::string pending_status,
                                                                             std::function<bool()> const& render,
                                                                             std::function<ava::core::Result<std::string>()> const& callback);

// Presentation-only state transitions used by the interactive reasoning-cycle path.
void apply_reasoning_cycle_success(ComposerSnapshot& snapshot, std::string feedback);
void clear_reasoning_feedback_for_user_input(ComposerSnapshot& snapshot);

}  // namespace ava::tui
