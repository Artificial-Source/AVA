#pragma once

#include <optional>
#include <string>
#include <string_view>
#include "debug.h"

namespace ava::tui {

// Test-facing parsing boundary retained for TUI tool-card tests.
[[nodiscard]] std::optional<std::string> parse_tui_tool_command_argument(std::string_view submitted);

namespace runtime_commands {

struct CopyTarget
{
  std::string name;
  std::string query = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class ReloadTarget
{
  KeyBindings,
  DisplaySettings,
};

[[nodiscard]] bool is_compact_command(std::string_view line) noexcept;
[[nodiscard]] bool shell_helper_submission(std::string_view submitted);
[[nodiscard]] bool exact_command(std::string_view submitted, std::string_view command);
[[nodiscard]] bool session_switching_command(std::string_view submitted);
[[nodiscard]] bool local_display_settings_command(std::string_view submitted);
[[nodiscard]] std::optional<std::string> reload_command_argument(std::string_view submitted);
[[nodiscard]] std::optional<std::string> copy_command_argument(std::string_view submitted);
[[nodiscard]] std::optional<std::string> stash_command_argument(std::string_view submitted);
[[nodiscard]] std::optional<std::string> tool_command_argument(std::string_view submitted);
[[nodiscard]] std::optional<std::string> diff_command_argument(std::string_view submitted);
[[nodiscard]] std::optional<std::string> search_command_argument(std::string_view submitted);
[[nodiscard]] std::optional<std::string> fork_from_command_argument(std::string_view submitted);
[[nodiscard]] std::optional<std::string> attach_command_argument(std::string_view submitted);
[[nodiscard]] CopyTarget parse_copy_target(std::string_view argument);
[[nodiscard]] std::optional<ReloadTarget> reload_target_from_argument(std::string_view target);

}  // namespace runtime_commands

}  // namespace ava::tui
