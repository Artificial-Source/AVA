#pragma once

#include "ava/agent/tool_types.h"
#include "ava/tools/tool_context.h"

namespace ava::agent {

[[nodiscard]] ToolDispatchResult read_file_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ToolDispatchResult write_file_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ToolDispatchResult edit_file_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);

}  // namespace ava::agent
