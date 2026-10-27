#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::mcp {

enum class McpServerScope
{
  Global,
  Project,
};

struct McpServerConfig
{
  std::string id;
  std::string name;
  ava::core::NoLogString command;
  std::vector<ava::core::NoLogString> args;
  std::vector<std::pair<ava::core::NoLogString, ava::core::NoLogString>> env;
  bool enabled = true;
  McpServerScope scope = McpServerScope::Global;
  std::filesystem::path source_path;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct McpConfig
{
  std::vector<McpServerConfig> servers;
  std::filesystem::path global_config_file;
  std::filesystem::path project_config_file;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct McpConfigLoadOptions
{
  std::filesystem::path workspace_dir;
  std::filesystem::path global_config_file;
  std::filesystem::path project_config_file;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string_view to_string(McpServerScope scope);
[[nodiscard]] bool is_valid_mcp_identifier(std::string_view id);
[[nodiscard]] McpConfigLoadOptions default_mcp_config_options(std::filesystem::path const& workspace_dir);
[[nodiscard]] ava::core::Result<McpConfig> parse_mcp_config(std::string_view json, std::filesystem::path config_path, McpServerScope scope);
[[nodiscard]] ava::core::Result<McpConfig> load_mcp_config(McpConfigLoadOptions const& options);

}  // namespace ava::mcp
