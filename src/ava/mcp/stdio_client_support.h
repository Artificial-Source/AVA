#pragma once

#include "ava/mcp/stdio_client.h"

#include <filesystem>

namespace ava::mcp {

[[nodiscard]] std::filesystem::path child_working_dir(McpServerConfig const& server, McpStdioClientOptions const& options);

}  // namespace ava::mcp
