#pragma once

#include "ava/agent/tool_types.h"
#include "ava/tools/tool_context.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ava::agent::tool_dispatch {

[[nodiscard]] std::string json_bool(bool value);
[[nodiscard]] ava::tools::ToolContext context_for_provider_tool(ava::tools::ToolContext const& context, ProviderToolCall const& call);
void append_spill_fields(std::string& text, std::filesystem::path const& path, bool spill_truncated);
[[nodiscard]] ToolDispatchResult tool_error_result(ProviderToolCall const& call, ava::core::Error const& error);
[[nodiscard]] ToolDispatchResult simple_error_result(ProviderToolCall const& call, ava::core::ErrorCategory category, std::string message);
[[nodiscard]] bool is_canceled(ava::tools::ToolContext const& context);
[[nodiscard]] ava::core::Error canceled_error(ProviderToolCall const& call);
[[nodiscard]] ava::core::VoidResult check_canceled(ava::tools::ToolContext const& context, ProviderToolCall const& call);
[[nodiscard]] ava::core::Result<std::string> required_string_arg(std::string_view arguments, std::string_view field, std::string_view tool_name);
[[nodiscard]] ava::core::VoidResult reject_nul_arg(std::string_view value, std::string_view field, std::string_view tool_name);
[[nodiscard]] ava::core::VoidResult reject_control_arg(std::string_view value, std::string_view field, std::string_view tool_name);
[[nodiscard]] ava::core::VoidResult reject_control_value(std::string_view value, std::string_view field, std::string_view message);
[[nodiscard]] ava::core::Result<std::string> required_text_arg(std::string_view arguments, std::string_view field, std::string_view tool_name);
[[nodiscard]] ava::core::Result<std::string> required_safe_string_arg(std::string_view arguments, std::string_view field, std::string_view tool_name);
[[nodiscard]] std::filesystem::path workspace_path(ava::tools::ToolContext const& context, std::string_view path);
[[nodiscard]] std::filesystem::path permission_dedupe_path(std::filesystem::path const& path);
[[nodiscard]] std::size_t optional_size_arg(std::string_view arguments, std::string_view field, std::size_t fallback, std::size_t maximum);
void append_changed_files_json(std::string& text, std::vector<std::filesystem::path> const& paths);
void append_diff_json(std::string& text, std::string_view diff, bool truncated);
[[nodiscard]] ava::core::Result<bool> optional_bool_arg(std::string_view arguments, std::string_view field, bool fallback, std::string_view tool_name);
[[nodiscard]] ava::core::VoidResult reject_provider_no_ignore(std::string_view arguments, std::string_view tool_name);

}  // namespace ava::agent::tool_dispatch
