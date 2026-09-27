#pragma once

#include "ava/agent/tool_types.h"
#include "ava/tools/tool_context.h"

namespace ava::agent {

[[nodiscard]] ToolDispatchResult apply_patch_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);

}  // namespace ava::agent
