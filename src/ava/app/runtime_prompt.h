#pragma once

#include "ava/app/runtime.h"
#include "ava/agent/agent_config.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>

namespace ava::app::runtime {

enum class PrimaryAgentResolutionPolicy
{
  RequireAvailable,
  AllowUnavailable,
};

// Resolve only from the catalog permitted by the supplied project-resource decision. With no request, or when AllowUnavailable cannot resolve the
// requested definition, returns AVA's safe built-in execution definition.
[[nodiscard]] ava::core::Result<ava::agent::AgentDefinition> resolve_runtime_primary_agent(ava::config::XdgPaths const& paths,
                                                                                           std::filesystem::path const& workspace_dir,
                                                                                           bool include_project_resources,
                                                                                           std::optional<std::string> const& requested_primary_agent,
                                                                                           PrimaryAgentResolutionPolicy policy);

[[nodiscard]] ava::core::Result<PromptState> load_runtime_prompt_state(ava::config::XdgPaths const& paths, ava::config::ModelInfo const& model,
                                                                       ava::agent::Mode mode, std::filesystem::path const& workspace_dir,
                                                                       std::filesystem::path const& current_dir, bool include_project_resources,
                                                                       PromptOverrides const& prompt_overrides,
                                                                       ava::agent::AgentDefinition const& selected_primary_agent);

}  // namespace ava::app::runtime
