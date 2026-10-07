#pragma once

#include "ava/app/commands.h"

#include <optional>
#include <string_view>

namespace ava::app {

// Execute a normalized TUI configuration command, returning no value when the command belongs to another family.
// Canonical command discovery, enablement, normalization, and security policy remain owned by run_command.
[[nodiscard]] std::optional<ava::core::Result<CommandResult>> run_tui_configuration_command(runtime::session_ts& unlocked_session,
                                                                                            std::string_view normalized_command,
                                                                                            std::vector<CommandHotkey> const& hotkeys);

}  // namespace ava::app
