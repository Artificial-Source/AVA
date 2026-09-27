#pragma once

#include "ava/agent/tool_types.h"
#include "ava/tools/tool_context.h"

namespace ava::agent {

[[nodiscard]] ToolDispatchResult glob_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ToolDispatchResult list_directory_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ToolDispatchResult grep_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);

}  // namespace ava::agent
