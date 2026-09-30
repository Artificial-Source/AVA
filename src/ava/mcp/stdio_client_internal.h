#pragma once

#include "ava/process/supervisor.h"
#include "ava/mcp/stdio_client.h"
#include "ava/mcp/stdio_client_support.h"
#include "ava/core/result.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ava::mcp {

[[nodiscard]] ava::core::Error mcp_error(ava::core::ErrorCategory category, std::string message, McpServerConfig const& server);
[[nodiscard]] ava::core::Error protocol_error(std::string message, McpServerConfig const& server);
[[nodiscard]] bool is_canceled(CancelCallback const& cancel_requested) noexcept;
[[nodiscard]] ava::core::Error canceled_error(std::string message, McpServerConfig const& server);
[[nodiscard]] std::string json_string(std::string_view value);
[[nodiscard]] std::string exit_detail(ava::process::ExitStatusV1 const& status);
[[nodiscard]] std::vector<std::string> mcp_argv(McpServerConfig const& server);
[[nodiscard]] std::string mcp_executable(McpServerConfig const& server, std::filesystem::path const& working_directory);
[[nodiscard]] std::chrono::steady_clock::time_point saturating_add(std::chrono::steady_clock::time_point value,
                                                                   std::chrono::steady_clock::duration duration) noexcept;

}  // namespace ava::mcp
