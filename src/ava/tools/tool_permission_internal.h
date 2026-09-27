#pragma once

#include "ava/tools/tool_context.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ava::tools::tool_permission_internal {

[[nodiscard]] std::string effective_tool_name(ToolContext const& context, ava::permissions::Operation operation, std::string_view tool_name);
[[nodiscard]] ava::core::VoidResult record_permission_audit(ToolContext const& context, PermissionAuditEvent const& event);
[[nodiscard]] PermissionAuditEvent audit_event(ToolContext const& context, std::string permission_request_id, ava::permissions::Operation operation,
                                               std::string tool_name, ava::permissions::PermissionDecision const& decision,
                                               std::filesystem::path const& target_path, std::string_view command,
                                               std::optional<ava::permissions::CommandPermissionMetadata> command_metadata);

}  // namespace ava::tools::tool_permission_internal
