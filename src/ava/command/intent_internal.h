#pragma once

#include "ava/command/command.h"

namespace ava::command::detail {

struct CompatibilityParse
{
  std::vector<std::string> argv;
  bool requires_raw_shell = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Error command_error(ava::core::ErrorCategory category, std::string message, std::string_view field = {}, std::string_view value = {});
[[nodiscard]] bool has_forbidden_path_byte(std::filesystem::path const& path);
[[nodiscard]] ava::core::VoidResult validate_limits(CommandLimits const& limits);
[[nodiscard]] ava::core::Result<CompatibilityParse> parse_compatibility_command(std::string_view command, CommandLimits const& limits);
[[nodiscard]] ava::core::VoidResult validate_structured_argv(std::vector<std::string> const& argv, CommandLimits const& limits);
[[nodiscard]] ava::core::VoidResult validate_raw_shell(std::string_view shell_text, CommandLimits const& limits);

}  // namespace ava::command::detail
