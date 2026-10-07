#pragma once

#include "ava/app/command_catalog.h"

#include <string>
#include <vector>

namespace ava::app {

// Transitional app/TUI seam: TUI keybinding definitions are projected into
// frontend-neutral command records before backend help/catalog formatting.
[[nodiscard]] std::vector<CommandHotkey> default_command_hotkeys();
[[nodiscard]] std::vector<CommandHotkey> effective_command_hotkeys(std::vector<CommandHotkey> const& hotkeys);
[[nodiscard]] std::string command_hotkey_primary_label(CommandHotkey const& hotkey);

}  // namespace ava::app
