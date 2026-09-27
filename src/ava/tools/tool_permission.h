#pragma once

#include "ava/tools/tool_context.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ava::tools {

[[nodiscard]] ava::core::VoidResult announce_tool_execution_start(ToolContext const& context);
[[nodiscard]] ava::core::VoidResult ensure_permission(ToolContext const& context, ava::permissions::Operation operation,
                                                      std::filesystem::path const& target_path, std::string_view command, std::string_view tool_name,
                                                      std::string_view error_message, std::string_view diff_preview = {}, bool diff_truncated = false,
                                                      std::optional<ava::permissions::CommandPermissionMetadata> command_metadata = std::nullopt);
// Optional read filtering preserves Ask/outcome and deny audit records while
// avoiding a new audit record for every ordinary policy-Allowed filtered access.
[[nodiscard]] ava::core::VoidResult ensure_filtered_read_permission(ToolContext const& context, std::filesystem::path const& target_path,
                                                                    std::string_view tool_name, std::string_view error_message);
// Command approval receives an already prepared plan. The caller retains that
// exact preparation through execution, so resolver, audit, and executor share
// one sealed identity rather than reparsing compatibility text.
[[nodiscard]] ava::core::VoidResult ensure_command_permission(ToolContext const& context, std::string_view command,
                                                              ava::command::CommandPreparation const& preparation, bool unverified_delegated_executor,
                                                              std::string_view tool_name, std::string_view error_message);
[[nodiscard]] ava::core::VoidResult ensure_command_permission(ToolContext const& context, std::string_view command,
                                                              ava::command::CommandPreparation const& preparation,
                                                              ava::permissions::CommandContainmentInfo const& containment, bool unverified_delegated_executor,
                                                              std::string_view tool_name, std::string_view error_message);
[[nodiscard]] std::string permission_audit_data_json(PermissionAuditEvent const& event);

}  // namespace ava::tools
