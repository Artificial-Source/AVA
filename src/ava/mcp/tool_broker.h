#pragma once
#include "ava/debug/print_members_on.h"
#include "ava/tools/tool_context.h"
#include "ava/tools/tool_types.h"
#include "ava/mcp/config.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>

namespace ava::mcp {

struct McpBrokeredTool
{
  std::string model_tool_name;
  ava::core::NoLogString description;
  ava::core::NoLogString schema_json;
  std::string permission_category;
  ava::core::NoLogString output_bound_summary;
  std::string execution_mode;
  std::string event_rendering_hint;
  std::string description_family;
  std::string source_id;
  std::string mcp_server;
  std::string mcp_name;
  ava::core::NoLogString resource_uri;
  std::function<ava::tools::ToolDispatchResult(ava::tools::ToolContext const&, ava::tools::ProviderToolCall const&)> executor;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using McpBrokeredToolVisitor = std::function<ava::core::VoidResult(McpBrokeredTool const&)>;

// Canonical launch composition spelling. Nonempty env is represented by a
// content-free domain-separated SHA-256 composition token: collision-resistant
// exact-grant binding, not encryption or secrecy. No-env session identities
// retain their existing spelling and are not truncated: identities above the
// persistent-rule command bound fail closed.
[[nodiscard]] std::string session_mcp_launch_identity(McpServerConfig const& server, std::filesystem::path const& canonical_child_cwd);
[[nodiscard]] std::string mcp_model_tool_name(std::string_view server_id, std::string_view tool_name);
[[nodiscard]] std::string mcp_model_resource_tool_name(std::string_view server_id, std::string_view resource_uri);
[[nodiscard]] ava::core::VoidResult visit_enabled_mcp_tools(ava::tools::ToolContext const& context, McpBrokeredToolVisitor const& visitor);

}  // namespace ava::mcp
