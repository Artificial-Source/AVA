#pragma once

#include "ava/debug/print_members_on.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ava::lsp {
class DiagnosticsProvider;
}  // namespace ava::lsp

namespace ava::mcp {
struct McpConfig;
}  // namespace ava::mcp

namespace ava::agent {

// Agent-owned tool composition inputs. Runtime and loop construction assemble
// this once; ToolContext mapping and child inheritance consume the nested
// fields without re-deriving discovery policy.
struct ToolResourceOptions
{
  bool include_project_resources = true;
  // The session and each active dispatcher keep the same diagnostics service alive.
  std::shared_ptr<ava::lsp::DiagnosticsProvider> lsp_diagnostics_provider = nullptr;
  std::filesystem::path plugin_global_plugins_dir = {};
  std::filesystem::path plugin_project_plugins_dir = {};
  std::filesystem::path plugin_enablement_file = {};
  bool include_plugin_tools = true;
  std::filesystem::path mcp_global_config_file = {};
  std::filesystem::path mcp_project_config_file = {};
  bool include_global_mcp_config = true;
  // Immutable session configuration is retained by concurrent parent/child loops.
  std::shared_ptr<ava::mcp::McpConfig const> session_mcp_config = nullptr;
  std::vector<std::filesystem::path> skill_global_dirs = {};
  std::vector<std::filesystem::path> skill_project_dirs = {};
  bool include_global_skills = true;
  std::optional<std::vector<std::string>> exact_builtin_tool_names = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::agent
