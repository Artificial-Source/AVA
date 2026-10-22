#include "sys.h"
#include "ava/tui/command_completions.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {
namespace {

// Return whether item already contains the semantic insertion identified by
// argument_index, previous_args, and value. Presentation metadata is ignored.
bool local_completion_exists(SlashCommandItem const& item, std::size_t argument_index, std::vector<std::string> const& previous_args, std::string_view value)
{
  return std::ranges::any_of(item.argument_completions, [&](auto const& completion) {
    return completion.argument_index == argument_index && completion.required_previous_args == previous_args && completion.value == value;
  });
}

// Add one local completion to item unless its semantic insertion already
// exists. Empty values are ignored; remaining parameters become presentation
// metadata and insertion behavior on the new record.
void add_local_completion(SlashCommandItem& item, std::size_t argument_index, std::string value, std::string description = {}, std::string category = "General",
                          std::vector<std::string> previous_args = {}, bool append_space = true, std::string display_label = {})
{
  if (value.empty() || local_completion_exists(item, argument_index, previous_args, value))
    return;
  item.argument_completions.push_back(SlashCommandArgumentCompletion{.value = std::move(value),
                                                                     .display_label = std::move(display_label),
                                                                     .description = std::move(description),
                                                                     .category = std::move(category),
                                                                     .required_previous_args = std::move(previous_args),
                                                                     .argument_index = argument_index,
                                                                     .append_space = append_space});
}

}  // namespace

// Replace dynamic local subsets and add static local choices without disturbing
// backend records. Missing bindings intentionally produce empty command hints.
void compose_tui_slash_command_items(std::vector<SlashCommandItem>& commands, std::vector<ThemeOptionItem> const& custom_themes,
                                     std::vector<TuiKeyBindingHelpItem> const& keybinding_help)
{
  auto keys_for_help_action = [&](std::string_view action) {
    auto const found = std::ranges::find_if(keybinding_help, [&](auto const& item) { return item.action == action; });
    return found == keybinding_help.end() ? std::string{} : found->keys;
  };
  for (auto& command : commands)
  {
    if (command.command == "/mode")
      command.key_display = keys_for_help_action("mode_toggle");
    else if (command.command == "/details")
      command.key_display = keys_for_help_action("details_toggle");
    else if (command.command == "/quit")
      command.key_display = keys_for_help_action("exit");

    if (command.command == "/hotkeys")
    {
      std::erase_if(command.argument_completions, [](auto const& completion) { return completion.category == "Keybindings"; });
      add_local_completion(command, 0, "init", "Create $XDG_CONFIG_HOME/ava/keybinds.json from AVA defaults");
      add_local_completion(command, 0, "import", "Import a validated keybindings JSON file", "General", {}, false);
      add_local_completion(command, 0, "set", "Set one keybinding action in keybinds.json");
      add_local_completion(command, 0, "reset", "Remove one action override from keybinds.json");
      add_local_completion(command, 0, "unset", "Alias for reset");
      add_local_completion(command, 0, "validate", "Validate $XDG_CONFIG_HOME/ava/keybinds.json without reloading", "General", {}, false);
      add_local_completion(command, 1, "--force", "Replace an existing keybinds.json starter file", "General", {"init"}, false);
      add_local_completion(command, 2, "--force", "Replace the existing keybinds.json with the imported file", "General", {"import"}, false);
      for (auto const& item : keybinding_help)
      {
        auto description = item.keys;
        if (!description.empty() && description != item.action)
          description += " · " + item.action;
        else if (description.empty())
          description = item.action;
        for (auto const& action : {"set", "reset", "unset"})
          add_local_completion(command, 1, item.action, description, "Keybindings", {action}, true, item.label.empty() ? item.action : item.label);
      }
    }
    else if (command.command == "/details")
    {
      add_local_completion(command, 0, "compact", "Use one fitted summary row per tool", "General", {}, false);
      add_local_completion(command, 0, "rich", "Use human calls with bounded useful output", "General", {}, false);
      add_local_completion(command, 0, "expanded", "Show all retained output within the defensive display cap", "General", {}, false);
    }
    else if (command.command == "/cursor")
    {
      for (auto const& style : {"default", "block", "underline", "bar"})
      {
        auto description = style == std::string_view("default") ? std::string("Do not override the shell cursor") : "Use a " + std::string(style) + " cursor";
        add_local_completion(command, 0, style, std::move(description), "General", {}, false);
        add_local_completion(command, 1, "blink", "Use the blinking variant", "General", {style}, false);
        add_local_completion(command, 1, "steady", "Use the steady variant", "General", {style}, false);
      }
    }
    else if (command.command == "/theme")
    {
      std::erase_if(command.argument_completions, [](auto const& completion) { return completion.category == "Themes"; });
      add_local_completion(command, 0, "dark", "Persist the built-in dark palette", "General", {}, false);
      add_local_completion(command, 0, "light", "Persist the built-in light palette", "General", {}, false);
      add_local_completion(command, 0, "plain", "Persist no-ANSI output", "General", {}, false);
      add_local_completion(command, 0, "reset", "Use the built-in default unless an environment override is set", "General", {}, false);
      for (auto const& theme : custom_themes)
        add_local_completion(command, 0, theme.name, "Persist custom theme from " + theme.detail, "Themes", {}, false);
    }
    else if (command.command == "/reload")
    {
      add_local_completion(command, 0, "keybindings", "Reload $XDG_CONFIG_HOME/ava/keybinds.json inside the TUI", "General", {}, false);
      add_local_completion(command, 0, "theme", "Reload $XDG_CONFIG_HOME/ava/display.json and repaint the TUI", "General", {}, false);
    }
  }
}

}  // namespace ava::tui
