#include "sys.h"
#include "ava/app/tui/command_hotkeys.h"
#include "ava/tui/keybindings.h"

namespace ava::app {

std::vector<CommandHotkey> default_command_hotkeys()
{
  std::vector<CommandHotkey> hotkeys;
  for (auto const& item : ava::tui::key_binding_help_items(ava::tui::default_key_bindings()))
  {
    hotkeys.push_back(CommandHotkey{.action = item.action, .description = item.label.empty() ? item.action : item.label, .keys = item.keys});
  }
  return hotkeys;
}

std::vector<CommandHotkey> effective_command_hotkeys(std::vector<CommandHotkey> const& hotkeys)
{
  return hotkeys.empty() ? default_command_hotkeys() : hotkeys;
}

std::string command_hotkey_primary_label(CommandHotkey const& hotkey)
{
  if (!hotkey.description.empty())
    return hotkey.description;
  if (auto const action = ava::tui::key_binding_action_from_name(hotkey.action))
  {
    auto label = ava::tui::action_label(*action);
    if (!label.empty())
      return label;
  }
  return hotkey.action;
}

}  // namespace ava::app
