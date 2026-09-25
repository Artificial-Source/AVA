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

enum class SubagentToolPreset
{
  Inherit,
  ReadOnly,
};

enum class SubagentDefinitionProvenance
{
  Unknown,
  Builtin,
  Global,
  Project,
};

[[nodiscard]] std::string_view to_string(SubagentDefinitionProvenance provenance) noexcept;

struct SubagentDefinition
{
  std::string name;
  std::string description;
  std::string system_prompt;
  SubagentToolPreset tool_preset = SubagentToolPreset::Inherit;
  std::optional<std::size_t> max_tool_iterations = std::nullopt;
  bool hidden = false;
  SubagentDefinitionProvenance provenance = SubagentDefinitionProvenance::Unknown;
  std::filesystem::path path = {};

  // Primary definitions retain prompt text and a source path. Keep generated diagnostics from printing either; expose only bounded selection metadata.
  void print_on(std::ostream& os) const
  {
    os << "{name_bytes:" << name.size() << ",tool_preset:" << (tool_preset == SubagentToolPreset::ReadOnly ? "read-only" : "inherit") << ",hidden:" << hidden
       << ",provenance:" << to_string(provenance) << '}';
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct SubagentDiagnostic
{
  std::filesystem::path path;
  std::string message;
  std::optional<std::string> agent_name = std::nullopt;
  bool blocks_primary_selection = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SubagentLoadOptions
{
  std::filesystem::path workspace_root;
  std::vector<std::filesystem::path> global_agent_dirs = {};
  std::vector<std::filesystem::path> project_agent_dirs = {};
  bool include_project_agents = true;
  std::size_t max_file_bytes = 64 * 1024;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SubagentLoadResult
{
  std::vector<SubagentDefinition> subagents;
  std::vector<SubagentDefinition> primary_agents;
  std::vector<std::string> invalid_primary_agents;
  std::vector<SubagentDiagnostic> diagnostics;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] bool valid_subagent_name(std::string_view name);
[[nodiscard]] std::vector<SubagentDefinition> builtin_subagents();
[[nodiscard]] std::vector<std::filesystem::path> default_global_subagent_dirs();
[[nodiscard]] std::vector<std::filesystem::path> default_project_subagent_dirs(std::filesystem::path const& workspace_root);
[[nodiscard]] SubagentLoadResult load_subagents(SubagentLoadOptions options = {});
[[nodiscard]] SubagentDefinition const* find_subagent(std::vector<SubagentDefinition> const& subagents, std::string_view name);
[[nodiscard]] ava::core::Result<SubagentDefinition> resolve_primary_agent(SubagentLoadResult const& loaded, std::string_view name);
[[nodiscard]] std::string subagent_names_csv(std::vector<SubagentDefinition> const& subagents);
[[nodiscard]] std::string primary_agent_names_csv(std::vector<SubagentDefinition> const& primary_agents);
[[nodiscard]] std::string format_available_subagents_for_prompt(std::vector<SubagentDefinition> const& subagents);
[[nodiscard]] ToolVisibilityOptions narrow_tool_visibility_to_read_only(ToolVisibilityOptions visibility);

}  // namespace ava::agent
