#pragma once

#include "ava/agent/tool_types.h"
#include "ava/tools/tool_context.h"

namespace ava::agent {

[[nodiscard]] ToolDispatchResult lsp_diagnostics_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ToolDispatchResult lsp_document_symbols_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ToolDispatchResult lsp_workspace_symbols_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ToolDispatchResult lsp_definition_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ToolDispatchResult lsp_references_result(ava::tools::ToolContext const& context, ProviderToolCall const& call);

}  // namespace ava::agent
