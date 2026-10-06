#pragma once

#include "ava/app/runtime.h"
#include "ava/agent/subagent_config.h"
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

// Resolve only from the catalog permitted by the supplied project-resource decision. AllowUnavailable preserves selection intent while returning no
// selected definition when a replacement or trust transition removes the named agent.
[[nodiscard]] ava::core::Result<std::optional<ava::agent::AgentDefinition>> resolve_runtime_primary_agent(
    ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir, bool include_project_resources,
    std::optional<std::string> const& requested_primary_agent, PrimaryAgentResolutionPolicy policy);

[[nodiscard]] ava::core::Result<PromptState> load_runtime_prompt_state(
    ava::config::XdgPaths const& paths, ava::config::ModelInfo const& model, ava::agent::Mode mode, std::filesystem::path const& workspace_dir,
    std::filesystem::path const& current_dir, bool include_project_resources, PromptOverrides const& prompt_overrides,
    std::optional<ava::agent::AgentDefinition> const& selected_primary_agent = std::nullopt);

}  // namespace ava::app::runtime
