#include "sys.h"
#include "ava/mcp/stdio_client_internal.h"
#include "ava/core/json.h"
#include "ava/core/process_args.h"

#include <utility>

namespace ava::mcp {

ava::core::Error mcp_error(ava::core::ErrorCategory category, std::string message, McpServerConfig const& server)
{
  static_cast<void>(server);
  return ava::core::Error(category, std::move(message));
}

ava::core::Error protocol_error(std::string message, McpServerConfig const& server)
{
  return mcp_error(ava::core::ErrorCategory::Tool, std::move(message), server);
}

bool is_canceled(CancelCallback const& cancel_requested) noexcept
{
  if (!cancel_requested)
    return false;
  try
  {
    return cancel_requested();
  }
  catch (...)
  {
    return true;
  }
}

ava::core::Error canceled_error(std::string message, McpServerConfig const& server)
{
  static_cast<void>(server);
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, std::move(message), ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  return error;
}

std::string json_string(std::string_view value)
{
  return "\"" + ava::core::json::escape(value) + "\"";
}

std::string exit_detail(ava::process::ExitStatusV1 const& status)
{
  if (status.kind == ava::process::ExitKindV1::Exited && status.has_exit_code)
    return "exit " + std::to_string(status.exit_code);
  if (status.kind == ava::process::ExitKindV1::Signaled && status.has_signal_number)
    return "signal " + std::to_string(status.signal_number);
  return std::string(ava::process::to_string(status.reason));
}

std::vector<std::string> mcp_argv(McpServerConfig const& server)
{
  std::vector<std::string> argv;
  argv.reserve(server.args.size() + 1);
  argv.push_back(server.command);
  argv.insert(argv.end(), server.args.begin(), server.args.end());
  return argv;
}

std::string mcp_executable(McpServerConfig const& server, std::filesystem::path const& working_directory)
{
  auto const& command = server.command;
  std::filesystem::path const requested(command);
  if (command.find('/') == std::string::npos || requested.is_absolute())
    return command;
  return (working_directory / requested).lexically_normal().string();
}

std::filesystem::path child_working_dir(McpServerConfig const& server, McpStdioClientOptions const& options)
{
  if (server.scope == McpServerScope::Global)
    return ava::core::safe_global_process_cwd(server.source_path, options.workspace_dir);
  if (!options.workspace_dir.empty())
    return options.workspace_dir;
  return std::filesystem::current_path();
}

}  // namespace ava::mcp
