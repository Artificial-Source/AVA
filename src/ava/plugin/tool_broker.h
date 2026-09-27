#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tools/tool_context.h"
#include "ava/tools/tool_types.h"
#include "ava/plugin/runner.h"
#include "ava/core/result.h"

#include <functional>
#include <string>
#include <string_view>

namespace ava::plugin {

struct PluginBrokeredTool
{
  std::string model_tool_name;
  std::string description;
  std::string schema_json;
  std::string permission_category;
  std::string output_bound_summary;
  std::string execution_mode;
  std::string event_rendering_hint;
  std::string description_family;
  std::string source_id;
  std::string plugin_name;
  std::function<ava::tools::ToolDispatchResult(ava::tools::ToolContext const&, ava::tools::ProviderToolCall const&)> executor;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using PluginBrokeredToolVisitor = std::function<ava::core::VoidResult(PluginBrokeredTool const&)>;

[[nodiscard]] std::string plugin_model_tool_name(std::string_view plugin_id, std::string_view tool_name);
[[nodiscard]] PluginProxyHandler make_core_service_proxy_handler(ava::tools::ToolContext context, PluginManifest manifest, std::string contribution_kind,
                                                                 std::string contribution_name, std::string model_operation_name, std::string call_id);
[[nodiscard]] ava::core::VoidResult visit_enabled_plugin_tools(ava::tools::ToolContext const& context, PluginBrokeredToolVisitor const& visitor);

}  // namespace ava::plugin
