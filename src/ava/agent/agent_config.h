#pragma once

#include "ava/agent/tool_visibility.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace ava::agent {

// Selects whether an agent inherits its caller's tool visibility or is narrowed to read-only tools.
enum class AgentToolPreset
{
  Inherit,
  ReadOnly,
};

enum class AgentDefinitionProvenance
{
  Unknown,
  Builtin,
  Global,
  Project,
};

[[nodiscard]] std::string_view to_string(AgentDefinitionProvenance provenance) noexcept;

struct AgentDefinition
{
  std::string name;
  std::string description;
  std::string system_prompt;
  AgentToolPreset tool_preset = AgentToolPreset::Inherit;
  std::optional<std::size_t> max_tool_iterations = std::nullopt;
  bool hidden = false;
  AgentDefinitionProvenance provenance = AgentDefinitionProvenance::Unknown;
  std::filesystem::path path = {};

  // Primary definitions retain prompt text and a source path. Keep generated diagnostics from printing either; expose only bounded selection metadata.
  void print_on(std::ostream& os) const
  {
    os << "{name_bytes:" << name.size() << ",tool_preset:" << (tool_preset == AgentToolPreset::ReadOnly ? "read-only" : "inherit") << ",hidden:" << hidden
       << ",provenance:" << to_string(provenance) << '}';
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Describes a bounded agent-definition loading problem and whether it blocks primary-agent selection.
struct AgentDiagnostic
{
  std::filesystem::path path;
  std::string message;
  std::optional<std::string> agent_name = std::nullopt;
  bool blocks_primary_selection = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Controls agent discovery; empty directory lists select the default global and project search roots.
struct AgentLoadOptions
{
  std::filesystem::path workspace_root;
  std::vector<std::filesystem::path> global_agent_dirs = {};
  std::vector<std::filesystem::path> project_agent_dirs = {};
  bool include_project_agents = true;
  std::size_t max_file_bytes = 64 * 1024;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Returns discovered task and primary agents together with invalid selections and non-fatal diagnostics.
struct AgentLoadResult
{
  std::vector<AgentDefinition> subagents;
  std::vector<AgentDefinition> primary_agents;
  std::vector<std::string> invalid_primary_agents;
  std::vector<AgentDiagnostic> diagnostics;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Returns whether name is a non-empty, bounded agent identifier containing only supported characters and separators.
[[nodiscard]] bool valid_agent_name(std::string_view name);
[[nodiscard]] std::vector<AgentDefinition> builtin_subagents();
// Returns the ordered global directories searched for agent definition files.
[[nodiscard]] std::vector<std::filesystem::path> default_global_agent_dirs();
// Returns the ordered project directories beneath workspace_root, or an empty list for an empty root.
[[nodiscard]] std::vector<std::filesystem::path> default_project_agent_dirs(std::filesystem::path const& workspace_root);
// Discovers agents according to options and returns usable definitions plus bounded validation diagnostics.
[[nodiscard]] AgentLoadResult load_agents(AgentLoadOptions options = {});
[[nodiscard]] AgentDefinition const* find_subagent(std::vector<AgentDefinition> const& subagents, std::string_view name);
// Resolves name from loaded primary agents, returning an actionable validation or availability error on failure.
[[nodiscard]] ava::core::Result<AgentDefinition> resolve_primary_agent(AgentLoadResult const& loaded, std::string_view name);
[[nodiscard]] std::string subagent_names_csv(std::vector<AgentDefinition> const& subagents);
[[nodiscard]] std::string primary_agent_names_csv(std::vector<AgentDefinition> const& primary_agents);
[[nodiscard]] std::string format_available_subagents_for_prompt(std::vector<AgentDefinition> const& subagents);
[[nodiscard]] ToolVisibilityOptions narrow_tool_visibility_to_read_only(ToolVisibilityOptions visibility);

}  // namespace ava::agent
