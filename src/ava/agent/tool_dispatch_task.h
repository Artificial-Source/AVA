#pragma once

#include "ava/agent/tool_dispatch_services.h"
#include "ava/agent/tool_types.h"
#include "ava/tools/tool_context.h"

namespace ava::agent {

[[nodiscard]] ToolDispatchResult task_result(ava::tools::ToolContext const& context, ToolDispatchServices const& services, ProviderToolCall const& call);

}  // namespace ava::agent
