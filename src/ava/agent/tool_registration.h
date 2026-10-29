#pragma once

#include "ava/agent/tool_registry.h"

namespace ava::agent {

[[nodiscard]] ava::core::Result<ToolRegistry> compose_tool_registry(ava::tools::ToolContext const& context, ToolVisibilityOptions const& visibility);

}  // namespace ava::agent
