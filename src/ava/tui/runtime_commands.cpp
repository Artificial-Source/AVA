#include "sys.h"
#include "ava/tui/runtime_commands_internal.h"

#include <cctype>

namespace ava::tui::runtime_commands {
namespace {

bool starts_with_command_submission(std::string_view submitted, std::string_view command)
{
  while (!submitted.empty() && std::isspace(static_cast<unsigned char>(submitted.front())) != 0)
    submitted.remove_prefix(1);
  while (!submitted.empty() && std::isspace(static_cast<unsigned char>(submitted.back())) != 0)
    submitted.remove_suffix(1);
  return submitted == command ||
         (submitted.starts_with(command) && submitted.size() > command.size() && std::isspace(static_cast<unsigned char>(submitted[command.size()])) != 0);
}

std::string trim_view_to_string(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    text.remove_suffix(1);
  return std::string(text);
}

}  // namespace

bool is_compact_command(std::string_view line) noexcept
{
  return line == "/compact" || (line.starts_with("/compact") && line.size() > 8 && line[8] == ' ');
}

bool shell_helper_submission(std::string_view submitted)
{
  return submitted.starts_with('!');
}

bool exact_command(std::string_view submitted, std::string_view command)
{
  while (!submitted.empty() && std::isspace(static_cast<unsigned char>(submitted.front())) != 0)
    submitted.remove_prefix(1);
  while (!submitted.empty() && std::isspace(static_cast<unsigned char>(submitted.back())) != 0)
    submitted.remove_suffix(1);
  return submitted == command;
}

bool session_switching_command(std::string_view submitted)
{
  return starts_with_command_submission(submitted, "/new") || starts_with_command_submission(submitted, "/clear") ||
         starts_with_command_submission(submitted, "/resume") || starts_with_command_submission(submitted, "/fork") ||
         starts_with_command_submission(submitted, "/clone");
}

bool local_display_settings_command(std::string_view submitted)
{
  return starts_with_command_submission(submitted, "/theme") || starts_with_command_submission(submitted, "/images") ||
         starts_with_command_submission(submitted, "/image-width") || starts_with_command_submission(submitted, "/cursor");
}

std::optional<std::string> reload_command_argument(std::string_view submitted)
{
  auto const normalized = trim_view_to_string(submitted);
  submitted = normalized;
  constexpr std::string_view kReload = "/reload";
  if (submitted == kReload)
    return std::string{};
  if (submitted.starts_with(kReload) && submitted.size() > kReload.size() && std::isspace(static_cast<unsigned char>(submitted[kReload.size()])) != 0)
  {
    return trim_view_to_string(submitted.substr(kReload.size() + 1));
  }
  return std::nullopt;
}

std::optional<std::string> copy_command_argument(std::string_view submitted)
{
  auto const normalized = trim_view_to_string(submitted);
  submitted = normalized;
  constexpr std::string_view kCopy = "/copy";
  if (submitted == kCopy)
    return std::string{};
  if (submitted.starts_with(kCopy) && submitted.size() > kCopy.size() && std::isspace(static_cast<unsigned char>(submitted[kCopy.size()])) != 0)
  {
    return trim_view_to_string(submitted.substr(kCopy.size() + 1));
  }
  return std::nullopt;
}

std::optional<std::string> stash_command_argument(std::string_view submitted)
{
  auto const normalized = trim_view_to_string(submitted);
  submitted = normalized;
  constexpr std::string_view kStash = "/stash";
  if (submitted == kStash)
    return std::string{};
  if (submitted.starts_with(kStash) && submitted.size() > kStash.size() && std::isspace(static_cast<unsigned char>(submitted[kStash.size()])) != 0)
  {
    return trim_view_to_string(submitted.substr(kStash.size() + 1));
  }
  return std::nullopt;
}

std::optional<std::string> tool_command_argument(std::string_view submitted)
{
  auto const normalized = trim_view_to_string(submitted);
  submitted = normalized;
  for (auto const command : {std::string_view("/tool"), std::string_view("/tools")})
  {
    if (submitted == command)
      return std::string{};
    if (submitted.starts_with(command) && submitted.size() > command.size() && std::isspace(static_cast<unsigned char>(submitted[command.size()])) != 0)
    {
      return trim_view_to_string(submitted.substr(command.size() + 1));
    }
  }
  return std::nullopt;
}

std::optional<std::string> diff_command_argument(std::string_view submitted)
{
  auto const normalized = trim_view_to_string(submitted);
  submitted = normalized;
  constexpr std::string_view kDiff = "/diff";
  if (submitted == kDiff)
    return std::string{};
  if (submitted.starts_with(kDiff) && submitted.size() > kDiff.size() && std::isspace(static_cast<unsigned char>(submitted[kDiff.size()])) != 0)
  {
    return trim_view_to_string(submitted.substr(kDiff.size() + 1));
  }
  return std::nullopt;
}

std::optional<std::string> search_command_argument(std::string_view submitted)
{
  auto const normalized = trim_view_to_string(submitted);
  submitted = normalized;
  constexpr std::string_view kSearch = "/search";
  if (submitted == kSearch)
    return std::string{};
  if (submitted.starts_with(kSearch) && submitted.size() > kSearch.size() && std::isspace(static_cast<unsigned char>(submitted[kSearch.size()])) != 0)
  {
    return trim_view_to_string(submitted.substr(kSearch.size() + 1));
  }
  return std::nullopt;
}

std::optional<std::string> fork_from_command_argument(std::string_view submitted)
{
  auto const normalized = trim_view_to_string(submitted);
  submitted = normalized;
  constexpr std::string_view kForkFrom = "/fork-from";
  if (submitted == kForkFrom)
    return std::string{};
  if (submitted.starts_with(kForkFrom) && submitted.size() > kForkFrom.size() && std::isspace(static_cast<unsigned char>(submitted[kForkFrom.size()])) != 0)
  {
    return trim_view_to_string(submitted.substr(kForkFrom.size() + 1));
  }
  return std::nullopt;
}

std::optional<std::string> attach_command_argument(std::string_view submitted)
{
  auto const normalized = trim_view_to_string(submitted);
  submitted = normalized;
  for (auto const command : {std::string_view("/attach"), std::string_view("/image")})
  {
    if (submitted == command)
      return std::string{};
    if (submitted.starts_with(command) && submitted.size() > command.size() && std::isspace(static_cast<unsigned char>(submitted[command.size()])) != 0)
    {
      auto argument = trim_view_to_string(submitted.substr(command.size() + 1));
      if (argument.size() >= 2 && ((argument.front() == '"' && argument.back() == '"') || (argument.front() == '\'' && argument.back() == '\'')))
      {
        argument = argument.substr(1, argument.size() - 2);
      }
      return argument;
    }
  }
  return std::nullopt;
}

CopyTarget parse_copy_target(std::string_view argument)
{
  auto const normalized = trim_view_to_string(argument);
  auto view = std::string_view(normalized);
  auto const name_end = view.find_first_of(" \t\r\n");
  if (name_end == std::string_view::npos)
    return CopyTarget{.name = normalized};
  return CopyTarget{.name = std::string(view.substr(0, name_end)), .query = trim_view_to_string(view.substr(name_end + 1))};
}

std::optional<ReloadTarget> reload_target_from_argument(std::string_view target)
{
  auto const normalized = trim_view_to_string(target);
  if (normalized.empty() || normalized == "keybindings" || normalized == "keybinds" || normalized == "keys")
    return ReloadTarget::KeyBindings;
  if (normalized == "theme" || normalized == "themes" || normalized == "display")
    return ReloadTarget::DisplaySettings;
  return std::nullopt;
}

}  // namespace ava::tui::runtime_commands

namespace ava::tui {
using runtime_commands::tool_command_argument;

std::optional<std::string> parse_tui_tool_command_argument(std::string_view submitted)
{
  return tool_command_argument(submitted);
}

}  // namespace ava::tui
