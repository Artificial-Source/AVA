#include "sys.h"
#include "ava/app/command_help.h"
#include "ava/app/tui/command_hotkeys.h"

#include <algorithm>

namespace ava::app {
namespace {

std::string aliases_text(CommandCatalogEntry const& entry)
{
  std::string text;
  for (auto const& alias : entry.aliases)
  {
    if (!text.empty())
      text += ", ";
    text += alias;
  }
  return text;
}

std::string command_display(CommandCatalogEntry const& entry)
{
  auto text = entry.command;
  if (!entry.hint.empty())
    text += " " + entry.hint;
  auto const aliases = aliases_text(entry);
  if (!aliases.empty())
    text += " (alias: " + aliases + ")";
  return text;
}

std::string command_rows(bool enabled)
{
  std::size_t width = 0;
  std::vector<CommandCatalogEntry const*> entries;
  for (auto const& entry : command_catalog())
  {
    if (entry.enabled != enabled)
      continue;
    entries.push_back(&entry);
    width = std::max(width, command_display(entry).size());
  }

  std::string output;
  for (auto const* entry : entries)
  {
    auto display = command_display(*entry);
    output += "  " + display;
    if (display.size() < width)
      output += std::string(width - display.size(), ' ');
    output += "  " + entry->description;
    if (!entry->enabled && !entry->disabled_reason.empty())
      output += " — disabled: " + entry->disabled_reason;
    output += '\n';
  }
  return output;
}

}  // namespace

std::string command_hotkeys_text(std::vector<CommandHotkey> const& hotkeys)
{
  auto const items = effective_command_hotkeys(hotkeys);
  std::size_t label_width = 0;
  std::size_t keys_width = 0;
  std::vector<std::string> primaries;
  primaries.reserve(items.size());
  for (auto const& item : items)
  {
    primaries.push_back(command_hotkey_primary_label(item));
    label_width = std::max(label_width, primaries.back().size());
    keys_width = std::max(keys_width, item.keys.size());
  }

  std::string output = "Keybindings:\n";
  output += "  Config: $XDG_CONFIG_HOME/ava/keybinds.json\n";
  output += "  Init: /keybindings init creates a validated starter file\n";
  output += "  Import: /keybindings import <path> [--force] validates and installs a JSON file\n";
  output += "  Set: /keybindings set <action> <key>[,<key>...] edits one action\n";
  output += "  Reset: /keybindings reset <action> removes one override\n";
  output += "  Validate: /keybindings validate checks keybinds.json without reloading\n";
  output += "  Reload: /reload keybindings inside the interactive TUI\n";
  for (std::size_t index = 0; index < items.size(); ++index)
  {
    auto const& item = items[index];
    auto const& primary = primaries[index];
    output += "  " + primary;
    if (primary.size() < label_width)
      output += std::string(label_width - primary.size(), ' ');
    output += "  " + item.keys;
    if (item.keys.size() < keys_width)
      output += std::string(keys_width - item.keys.size(), ' ');
    // Machine id stays secondary after the human label and bound keys; drop long action_description text.
    if (!item.action.empty() && item.action != primary)
      output += "  " + item.action;
    output += '\n';
  }
  return output;
}

std::string command_help_text(std::vector<CommandHotkey> const& hotkeys)
{
  std::string output = "Commands:\n";
  output += command_rows(true);
  output += "\nShell helpers:\n";
  output += "  !<command>   Run a permissioned shell command through /bash\n";
  output +=
      "  !!<command>  Run the same permissioned shell command as a hidden-output helper; AVA keeps shell output out of provider context unless you paste it "
      "into a later prompt\n";
  output += "\nUnavailable commands:\n";
  output += command_rows(false);
  output += '\n';
  output += command_hotkeys_text(hotkeys);
  if (!output.empty() && output.back() == '\n')
    output.pop_back();
  return output;
}

}  // namespace ava::app
