#pragma once

#include <string_view>

namespace ava::agent {

// Selects whether an agent inherits its caller's tool visibility or is narrowed to read-only tools.
enum class AgentToolPreset
{
  Inherit,
  ReadOnly,
};

// Identifies whether an agent definition is built in, loaded from global configuration, or loaded from project resources.
// Unknown denotes unspecified provenance; project definitions require project trust before use.
enum class AgentDefinitionProvenance
{
  Unknown,
  Builtin,
  Global,
  Project,
};

[[nodiscard]] std::string_view to_string(AgentToolPreset preset) noexcept;
[[nodiscard]] std::string_view to_string(AgentDefinitionProvenance provenance) noexcept;

}  // namespace ava::agent
