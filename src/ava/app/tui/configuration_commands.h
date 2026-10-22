#pragma once

#include "ava/app/commands.h"
#include "ava/config/xdg_paths.h"

#include <optional>
#include <string_view>

namespace ava::app {

// Persist or query normalized_command using paths without applying terminal state.
// Returns InvalidArgument when normalized_command is not a supported display command.
//
// This path is safe for headless/RPC workers; the interactive frontend applies
// the resulting document through its TUI-thread-local owner.
[[nodiscard]] ava::core::Result<CommandResult> run_tui_display_configuration_command(ava::config::XdgPaths const& paths, std::string_view normalized_command);

// Execute a normalized TUI configuration command, returning no value when the command belongs to another family.
// Canonical command discovery, enablement, normalization, and security policy remain owned by run_command.
[[nodiscard]] std::optional<ava::core::Result<CommandResult>> run_tui_configuration_command(runtime::session_ts& unlocked_session,
                                                                                            std::string_view normalized_command,
                                                                                            std::vector<CommandHotkey> const& hotkeys);

}  // namespace ava::app
