#pragma once

#include "ava/tui/composer.h"
#include "ava/tui/keybindings.h"

#include <vector>

namespace ava::tui {

// Compose TUI-owned slash choices into commands on the UI thread. custom_themes
// and keybinding_help are retained local facts; repeated calls replace their
// dynamic records, preserve backend records, and add each local choice once.
void compose_tui_slash_command_items(std::vector<SlashCommandItem>& commands, std::vector<ThemeOptionItem> const& custom_themes,
                                     std::vector<TuiKeyBindingHelpItem> const& keybinding_help);

}  // namespace ava::tui
