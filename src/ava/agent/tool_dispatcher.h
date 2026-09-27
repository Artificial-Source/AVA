#pragma once

#include "ava/agent/tool_dispatch_services.h"
#include "ava/agent/tool_metadata.h"
#include "ava/agent/tool_registry.h"
#include "ava/agent/tool_types.h"
#include "ava/agent/tool_visibility.h"
#include "ava/tools/tool_context.h"
#include "ava/core/result.h"

#include <span>
#include <string>
#include <vector>

namespace ava::agent {

class ToolDispatcher
{
 public:
  explicit ToolDispatcher(ava::tools::ToolContext context, ToolDispatchServices services = {}, ToolVisibilityOptions visibility = {});
  [[nodiscard]] static ava::core::Result<ToolDispatcher> create_strict(ava::tools::ToolContext context, ToolDispatchServices services = {},
                                                                       ToolVisibilityOptions visibility = {});

  [[nodiscard]] ava::core::Result<ToolDispatchResult> dispatch(ProviderToolCall const& call) const;
  [[nodiscard]] ava::core::Result<ToolDispatchResult> dispatch_with_context(ava::tools::ToolContext context, ToolDispatchServices const& services,
                                                                            ProviderToolCall const& call) const;
  [[nodiscard]] std::vector<ToolMetadata> registered_tool_metadata() const;
  [[nodiscard]] std::vector<std::string> registered_tool_schemas_json() const;
  [[nodiscard]] static std::span<ToolMetadata const> tool_metadata();
  [[nodiscard]] static std::vector<ToolMetadata> tool_metadata(ava::tools::ToolContext const& context, ToolVisibilityOptions const& visibility = {});
  [[nodiscard]] static std::vector<std::string> tool_schemas_json();
  [[nodiscard]] static std::vector<std::string> tool_schemas_json(ava::tools::ToolContext const& context, ToolVisibilityOptions const& visibility = {});

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  ToolDispatcher(ava::tools::ToolContext context, ToolDispatchServices services, ToolRegistry registry);

  ava::tools::ToolContext context_;
  ToolDispatchServices services_;
  ToolRegistry registry_;
};

}  // namespace ava::agent
