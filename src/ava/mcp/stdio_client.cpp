#include "sys.h"
#include "ava/diagnostics/safe_failure.h"
#include "ava/mcp/protocol.h"
#include "ava/mcp/stdio_client.h"
#include "ava/mcp/stdio_client_support.h"
#include "ava/core/json.h"
#include "ava/core/strict_json.h"
#include "ava/core/version.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include "debug.h"

namespace ava::mcp {
namespace {

constexpr int kMaxDrainReadsPerPoll = 16;
constexpr std::size_t kMaxMcpListPages = 8;
constexpr std::size_t kMaxMcpPaginationCursorBytes = 64 * 1024;
constexpr std::size_t kMaxMcpEnvironmentVariables = 64;
constexpr std::size_t kMaxMcpEnvironmentValueBytes = 16 * 1024;

bool valid_environment_name(std::string_view name)
{
  if (name.empty() || name.size() > 128 || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_'))
    return false;
  return std::ranges::all_of(name.substr(1), [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '_';
  });
}

std::string compact_json_line(std::string_view message)
{
  std::string compact;
  compact.reserve(message.size() + 1);
  bool in_string = false;
  bool escaped = false;
  for (char const ch : message)
  {
    if (in_string)
    {
      compact.push_back(ch);
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        in_string = false;
      continue;
    }
    if (ch == '"')
    {
      in_string = true;
      compact.push_back(ch);
    }
    else if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
    {
      compact.push_back(ch);
    }
  }
  compact.push_back('\n');
  return compact;
}

ava::core::Result<std::optional<std::string>> pagination_cursor(std::string_view result_json, McpServerConfig const& server)
{
  if (!ava::core::json::field_value_start(result_json, "nextCursor"))
    return std::optional<std::string>{};
  auto cursor = ava::core::json::string_field(result_json, "nextCursor");
  if (!cursor || cursor->size() > kMaxMcpPaginationCursorBytes)
  {
    return std::unexpected(protocol_error("MCP list result has invalid nextCursor", server));
  }
  return std::optional<std::string>(std::move(*cursor));
}

ava::core::VoidResult validate_environment(McpServerConfig const& server)
{
  if (server.env.size() > kMaxMcpEnvironmentVariables)
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "too many MCP environment variables", server));
  std::vector<std::string_view> seen;
  for (auto const& [name, value] : server.env)
  {
    if (!valid_environment_name(name) || value.size() > kMaxMcpEnvironmentValueBytes || value.find('\0') != std::string::npos)
      return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP environment variable is invalid", server));
    if (std::ranges::find(seen, name) != seen.end())
      return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "duplicate MCP environment variable", server));
    seen.push_back(name);
  }
  return {};
}

}  // namespace

McpStdioClient::McpStdioClient(McpServerConfig server, McpStdioClientOptions options) : server_(std::move(server)), options_(std::move(options))
{
}

McpStdioClient::~McpStdioClient()
{
  terminate_child();
  close_fds();
}

ava::core::Result<std::unique_ptr<McpStdioClient>> McpStdioClient::start(McpServerConfig server, McpStdioClientOptions options, CancelCallback cancel_requested)
{
  if (server.command.empty())
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP server command must not be empty", server));
  }
  if (auto valid_env = validate_environment(server); !valid_env)
    return std::unexpected(std::move(valid_env.error()));
  if (options.workspace_dir.empty())
    options.workspace_dir = std::filesystem::current_path();
  if (options.startup_timeout < std::chrono::milliseconds(50) || options.startup_timeout > std::chrono::seconds(30))
  {
    auto error = mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP startup timeout is out of bounds", server);
    error.with_context("min_ms", "50");
    error.with_context("max_ms", "30000");
    return std::unexpected(std::move(error));
  }
  if (options.request_timeout < std::chrono::milliseconds(50) || options.request_timeout > std::chrono::seconds(30))
  {
    auto error = mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP request timeout is out of bounds", server);
    error.with_context("min_ms", "50");
    error.with_context("max_ms", "30000");
    return std::unexpected(std::move(error));
  }
  if (options.max_message_bytes == 0 || options.max_stderr_bytes == 0)
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP client byte limits must be non-zero", server));
  }
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP startup canceled", server));
  }

  auto client = std::make_unique<McpStdioClient>(std::move(server), std::move(options));
  if (auto launched = client->launch(); !launched)
    return std::unexpected(std::move(launched.error()));
  if (auto initialized = client->initialize(cancel_requested); !initialized)
  {
    return std::unexpected(std::move(initialized.error()));
  }
  return client;
}

McpServerConfig const& McpStdioClient::server() const noexcept
{
  return server_;
}

McpInitialization const& McpStdioClient::initialization() const noexcept
{
  return initialization_;
}

std::string const& McpStdioClient::stderr_tail() const noexcept
{
  return stderr_tail_;
}

bool McpStdioClient::stderr_truncated() const noexcept
{
  return stderr_truncated_;
}

ava::core::VoidResult McpStdioClient::launch()
{
  auto stdin_pipe = make_pipe(server_);
  if (!stdin_pipe)
    return std::unexpected(std::move(stdin_pipe.error()));
  UniqueFd stdin_read((*stdin_pipe)[0]);
  UniqueFd stdin_write((*stdin_pipe)[1]);

  auto stdout_pipe = make_pipe(server_);
  if (!stdout_pipe)
    return std::unexpected(std::move(stdout_pipe.error()));
  UniqueFd stdout_read((*stdout_pipe)[0]);
  UniqueFd stdout_write((*stdout_pipe)[1]);

  auto stderr_pipe = make_pipe(server_);
  if (!stderr_pipe)
    return std::unexpected(std::move(stderr_pipe.error()));
  UniqueFd stderr_read((*stderr_pipe)[0]);
  UniqueFd stderr_write((*stderr_pipe)[1]);

  auto argv_strings = mcp_argv(server_);
  std::vector<char*> argv;
  argv.reserve(argv_strings.size() + 1);
  for (auto& arg : argv_strings) argv.push_back(arg.data());
  argv.push_back(nullptr);
  auto const explicit_path = std::ranges::find_if(server_.env, [](auto const& variable) { return variable.first == "PATH"; });
  std::string_view const execution_path = explicit_path == server_.env.end() ? std::string_view(kTrustedExecPath) : std::string_view(explicit_path->second);

  std::vector<std::string> environment_strings;
  std::vector<std::string> inherited_names;
  if (!options_.clean_environment)
  {
    for (char** inherited = ::environ; inherited != nullptr && *inherited != nullptr; ++inherited)
    {
      std::string_view const variable(*inherited);
      auto const separator = variable.find('=');
      if (separator == std::string_view::npos || separator == 0)
        continue;
      auto const name = variable.substr(0, separator);
      if (name == "PATH" || name == "PWD" || std::ranges::any_of(server_.env, [&](auto const& override) { return override.first == name; }) ||
          std::ranges::find(inherited_names, name) != inherited_names.end())
        continue;
      inherited_names.emplace_back(name);
      environment_strings.emplace_back(variable);
    }
  }
  environment_strings.reserve(environment_strings.size() + server_.env.size() + 1);
  if (explicit_path == server_.env.end())
    environment_strings.emplace_back(std::string("PATH=") + kTrustedExecPath);
  for (auto const& [name, value] : server_.env) environment_strings.push_back(name + "=" + value);

  std::vector<char*> environment;
  environment.reserve(environment_strings.size() + 1);
  for (auto& value : environment_strings) environment.push_back(value.data());
  environment.push_back(nullptr);

  auto executable = server_.command;
  if (executable.find('/') == std::string::npos)
  {
    std::string_view paths(execution_path);
    while (!paths.empty())
    {
      auto const separator = paths.find(':');
      auto const directory = paths.substr(0, separator);
      auto candidate = std::filesystem::path(directory) / executable;
      if (::access(candidate.c_str(), X_OK) == 0)
      {
        executable = candidate.string();
        break;
      }
      if (separator == std::string_view::npos)
        break;
      paths.remove_prefix(separator + 1);
    }
    if (executable.find('/') == std::string::npos)
      return std::unexpected(mcp_error(ava::core::ErrorCategory::NotFound, "MCP executable was not found on the configured PATH", server_));
  }

  auto const cwd = child_working_dir(server_, options_).string();
  // Set PWD to the logical cwd so that child processes preserve symlinked
  // path components instead of the physical path that getcwd() would return.
  environment_strings.emplace_back(std::string("PWD=") + cwd);
  // Rebuild environment pointers to include PWD.
  environment.clear();
  environment.reserve(environment_strings.size() + 1);
  for (auto& value : environment_strings) environment.push_back(value.data());
  environment.push_back(nullptr);
  pid_t const pid = fork();
  if (pid < 0)
    return std::unexpected(errno_error("failed to fork MCP server process", server_));

  if (pid == 0)
  {
    setpgid(0, 0);
    stdin_write.reset();
    stdout_read.reset();
    stderr_read.reset();
    if (dup2(stdin_read.get(), STDIN_FILENO) < 0)
      _exit(127);
    if (dup2(stdout_write.get(), STDOUT_FILENO) < 0)
      _exit(127);
    if (dup2(stderr_write.get(), STDERR_FILENO) < 0)
      _exit(127);
    stdin_read.reset();
    stdout_write.reset();
    stderr_write.reset();
    if (chdir(cwd.c_str()) != 0)
      _exit(127);
    close_nonstandard_fds();
    // argv/environment storage is fully prepared before fork. The child does
    // no allocator-backed environment mutation in the post-fork path.
    execve(executable.c_str(), argv.data(), environment.data());
    _exit(127);
  }

  pid_ = static_cast<int>(pid);
  can_signal_group_ = set_child_process_group(pid);
  stdin_read.reset();
  stdout_write.reset();
  stderr_write.reset();
  stdin_fd_ = stdin_write.release();
  stdout_fd_ = stdout_read.release();
  stderr_fd_ = stderr_read.release();

  if (auto nonblocking = set_pipe_nonblocking(stdin_fd_); !nonblocking)
  {
    terminate_child();
    close_fds();
    return std::unexpected(std::move(nonblocking.error()));
  }
  if (auto nonblocking = set_pipe_nonblocking(stdout_fd_); !nonblocking)
  {
    terminate_child();
    close_fds();
    return std::unexpected(std::move(nonblocking.error()));
  }
  if (auto nonblocking = set_pipe_nonblocking(stderr_fd_); !nonblocking)
  {
    terminate_child();
    close_fds();
    return std::unexpected(std::move(nonblocking.error()));
  }
  return {};
}

ava::core::VoidResult McpStdioClient::initialize(CancelCallback cancel_requested)
{
  std::string const params =
      "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"ava\","
      "\"version\":\"" +
      std::string(ava::core::version::kFullVersion) + "\"}}";
  auto response = request("initialize", params, options_.startup_timeout, "timed out waiting for MCP initialization", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  auto const protocol_version = ava::core::json::string_field(response->result_json, "protocolVersion");
  auto const server_info = ava::core::json::object_field(response->result_json, "serverInfo");
  auto const capabilities = ava::core::json::object_field(response->result_json, "capabilities");
  if (!protocol_version || *protocol_version != "2024-11-05" || !server_info || !capabilities || !ava::core::json::is_valid_object(*capabilities))
  {
    auto error = protocol_error("MCP initialize response is malformed", server_);
    error.with_context("response_bytes", std::to_string(response->raw_json.size()));
    return std::unexpected(std::move(error));
  }
  initialization_ = McpInitialization{.server_name = ava::core::json::string_field(*server_info, "name").value_or(server_.id),
                                      .server_version = ava::core::json::string_field(*server_info, "version").value_or(""),
                                      .capabilities_json = *capabilities,
                                      .raw_json = response->raw_json};

  std::string const notification = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
  auto const deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  return write_message(notification, deadline, options_.request_timeout, "timed out writing MCP initialized notification",
                       "MCP server closed before initialized notification", cancel_requested);
}

ava::core::Result<std::vector<McpToolDescription>> McpStdioClient::list_tools(CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP tools/list canceled", server_));
  }

  std::vector<McpToolDescription> tools;
  std::optional<std::string> cursor;
  for (std::size_t page = 0; page < kMaxMcpListPages; ++page)
  {
    std::string const params = cursor ? ("{\"cursor\":" + json_string(*cursor) + "}") : "{}";
    auto response = request("tools/list", params, options_.request_timeout, "timed out waiting for MCP tools/list", cancel_requested);
    if (!response)
      return std::unexpected(std::move(response.error()));
    auto const tools_start = ava::core::json::field_value_start(response->result_json, "tools");
    if (!tools_start || *tools_start >= response->result_json.size() || response->result_json[*tools_start] != '[')
    {
      auto error = protocol_error("MCP tools/list result has invalid tools field", server_);
      error.with_context("response_bytes", std::to_string(response->raw_json.size()));
      return std::unexpected(std::move(error));
    }

    for (auto const& tool_json : ava::core::json::objects_in_array_field(response->result_json, "tools"))
    {
      auto name = ava::core::json::string_field(tool_json, "name");
      if (!name || !ava::mcp::is_valid_mcp_tool_name(*name))
      {
        auto error = protocol_error("MCP tool has invalid name", server_);
        error.with_context("response_bytes", std::to_string(tool_json.size()));
        return std::unexpected(std::move(error));
      }
      auto input_schema = ava::core::json::object_field(tool_json, "inputSchema").value_or("{\"type\":\"object\"}");
      if (!ava::core::json::is_valid_object(input_schema))
      {
        return std::unexpected(protocol_error("MCP tool inputSchema is invalid", server_));
      }
      tools.push_back(McpToolDescription{.name = std::move(*name),
                                         .description = ava::core::json::string_field(tool_json, "description").value_or(""),
                                         .input_schema_json = std::move(input_schema)});
    }
    auto next_cursor = pagination_cursor(response->result_json, server_);
    if (!next_cursor)
      return std::unexpected(std::move(next_cursor.error()));
    if (!*next_cursor)
      return tools;
    cursor = std::move(**next_cursor);
  }
  auto error = protocol_error("MCP tools/list exceeded pagination limit", server_);
  error.with_context("max_pages", std::to_string(kMaxMcpListPages));
  return std::unexpected(std::move(error));
}

ava::core::Result<McpToolCallResult> McpStdioClient::call_tool(std::string_view tool_name, std::string_view arguments_json, CancelCallback cancel_requested)
{
  if (!ava::mcp::is_valid_mcp_tool_name(tool_name))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP tool name is invalid", server_));
  }
  if (!ava::core::json::is_valid_object(arguments_json))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP tool arguments must be a JSON object", server_));
  }
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP tools/call canceled", server_));
  }
  std::string const params = "{\"name\":" + json_string(tool_name) + ",\"arguments\":" + std::string(arguments_json) + "}";
  auto response = request("tools/call", params, options_.request_timeout, "timed out waiting for MCP tools/call", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  bool const is_error = mcp_bool_field(response->result_json, "isError").value_or(false);
  if (is_error)
  {
    Dout(dc::mcp, "operation=tool_call state=remote_failure response_bytes=" << response->raw_json.size());
    auto const failure = ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Mcp);
    return McpToolCallResult{.is_error = true, .content = ava::diagnostics::serialize_safe_failure_json(failure), .raw_json = {}};
  }
  return McpToolCallResult{.is_error = false, .content = mcp_text_content_from_result(response->result_json), .raw_json = response->raw_json};
}

ava::core::Result<std::vector<McpPromptDescription>> McpStdioClient::list_prompts(CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("MCP prompts/list canceled", server_));

  std::vector<McpPromptDescription> prompts;
  std::optional<std::string> cursor;
  for (std::size_t page = 0; page < kMaxMcpListPages; ++page)
  {
    std::string const params = cursor ? ("{\"cursor\":" + json_string(*cursor) + "}") : "{}";
    auto response = request("prompts/list", params, options_.request_timeout, "timed out waiting for MCP prompts/list", cancel_requested);
    if (!response)
      return std::unexpected(std::move(response.error()));
    auto const prompts_start = ava::core::json::field_value_start(response->result_json, "prompts");
    if (!prompts_start || *prompts_start >= response->result_json.size() || response->result_json[*prompts_start] != '[')
    {
      auto error = protocol_error("MCP prompts/list result has invalid prompts field", server_);
      error.with_context("response_bytes", std::to_string(response->raw_json.size()));
      return std::unexpected(std::move(error));
    }

    for (auto const& prompt_json : ava::core::json::objects_in_array_field(response->result_json, "prompts"))
    {
      auto name = ava::core::json::string_field(prompt_json, "name");
      if (!name || !ava::mcp::is_valid_mcp_tool_name(*name))
      {
        auto error = protocol_error("MCP prompt has invalid name", server_);
        error.with_context("response_bytes", std::to_string(prompt_json.size()));
        return std::unexpected(std::move(error));
      }
      McpPromptDescription prompt{
          .name = std::move(*name), .description = ava::core::json::string_field(prompt_json, "description").value_or(""), .arguments = {}};
      for (auto const& argument_json : ava::core::json::objects_in_array_field(prompt_json, "arguments"))
      {
        auto argument_name = ava::core::json::string_field(argument_json, "name");
        if (!argument_name || !ava::mcp::is_valid_mcp_tool_name(*argument_name))
        {
          auto error = protocol_error("MCP prompt argument has invalid name", server_);
          error.with_context("response_bytes", std::to_string(argument_json.size()));
          return std::unexpected(std::move(error));
        }
        prompt.arguments.push_back(McpPromptArgumentDescription{.name = std::move(*argument_name),
                                                                .description = ava::core::json::string_field(argument_json, "description").value_or(""),
                                                                .required = mcp_bool_field(argument_json, "required").value_or(false)});
      }
      prompts.push_back(std::move(prompt));
    }

    auto next_cursor = pagination_cursor(response->result_json, server_);
    if (!next_cursor)
      return std::unexpected(std::move(next_cursor.error()));
    if (!*next_cursor)
      return prompts;
    cursor = std::move(**next_cursor);
  }
  auto error = protocol_error("MCP prompts/list exceeded pagination limit", server_);
  error.with_context("max_pages", std::to_string(kMaxMcpListPages));
  return std::unexpected(std::move(error));
}

ava::core::Result<McpPromptGetResult> McpStdioClient::get_prompt(std::string_view prompt_name, std::string_view arguments_json, CancelCallback cancel_requested)
{
  if (!ava::mcp::is_valid_mcp_tool_name(prompt_name))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP prompt name is invalid", server_));
  }
  if (!ava::core::json::is_valid_object(arguments_json))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP prompt arguments must be a JSON object", server_));
  }
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP prompts/get canceled", server_));
  }
  std::string const params = "{\"name\":" + json_string(prompt_name) + ",\"arguments\":" + std::string(arguments_json) + "}";
  auto response = request("prompts/get", params, options_.request_timeout, "timed out waiting for MCP prompts/get", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  return McpPromptGetResult{.content = mcp_prompt_text_from_result(response->result_json), .raw_json = response->raw_json};
}

ava::core::Result<std::vector<McpResourceDescription>> McpStdioClient::list_resources(CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP resources/list canceled", server_));
  }

  std::vector<McpResourceDescription> resources;
  std::optional<std::string> cursor;
  for (std::size_t page = 0; page < kMaxMcpListPages; ++page)
  {
    std::string const params = cursor ? ("{\"cursor\":" + json_string(*cursor) + "}") : "{}";
    auto response = request("resources/list", params, options_.request_timeout, "timed out waiting for MCP resources/list", cancel_requested);
    if (!response)
      return std::unexpected(std::move(response.error()));
    auto const resources_start = ava::core::json::field_value_start(response->result_json, "resources");
    if (!resources_start || *resources_start >= response->result_json.size() || response->result_json[*resources_start] != '[')
    {
      auto error = protocol_error("MCP resources/list result has invalid resources field", server_);
      error.with_context("response_bytes", std::to_string(response->raw_json.size()));
      return std::unexpected(std::move(error));
    }
    for (auto const& resource_json : ava::core::json::objects_in_array_field(response->result_json, "resources"))
    {
      auto uri = ava::core::json::string_field(resource_json, "uri");
      if (!uri || !ava::mcp::is_valid_mcp_resource_uri(*uri))
      {
        auto error = protocol_error("MCP resource has invalid uri", server_);
        error.with_context("response_bytes", std::to_string(resource_json.size()));
        return std::unexpected(std::move(error));
      }
      resources.push_back(McpResourceDescription{.uri = std::move(*uri),
                                                 .name = ava::core::json::string_field(resource_json, "name").value_or(""),
                                                 .description = ava::core::json::string_field(resource_json, "description").value_or(""),
                                                 .mime_type = ava::core::json::string_field(resource_json, "mimeType").value_or("")});
    }
    auto next_cursor = pagination_cursor(response->result_json, server_);
    if (!next_cursor)
      return std::unexpected(std::move(next_cursor.error()));
    if (!*next_cursor)
      return resources;
    cursor = std::move(**next_cursor);
  }
  auto error = protocol_error("MCP resources/list exceeded pagination limit", server_);
  error.with_context("max_pages", std::to_string(kMaxMcpListPages));
  return std::unexpected(std::move(error));
}

ava::core::Result<McpResourceReadResult> parse_resource_read_result(std::string_view result_json, std::string_view raw_json, McpServerConfig const& server)
{
  auto const contents_start = ava::core::json::field_value_start(result_json, "contents");
  if (!contents_start || *contents_start >= result_json.size() || result_json[*contents_start] != '[')
  {
    auto error = protocol_error("MCP resources/read result has invalid contents field", server);
    error.with_context("response_bytes", std::to_string(raw_json.size()));
    return std::unexpected(std::move(error));
  }

  McpResourceReadResult result;
  result.raw_json = std::string(raw_json);
  bool saw_text = false;
  for (auto const& item : ava::core::json::objects_in_array_field(result_json, "contents"))
  {
    auto text = ava::core::json::string_field(item, "text");
    if (!text)
      continue;
    if (!saw_text)
    {
      result.uri = ava::core::json::string_field(item, "uri").value_or("");
      result.mime_type = ava::core::json::string_field(item, "mimeType").value_or("");
    }
    else
    {
      result.content += '\n';
    }
    saw_text = true;
    result.content += *text;
  }
  if (!saw_text)
  {
    auto error = protocol_error("MCP resources/read returned no text content", server);
    error.with_context("response_bytes", std::to_string(raw_json.size()));
    return std::unexpected(std::move(error));
  }
  return result;
}

ava::core::Result<McpResourceReadResult> McpStdioClient::read_resource(std::string_view uri, CancelCallback cancel_requested)
{
  if (!ava::mcp::is_valid_mcp_resource_uri(uri))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP resource uri is invalid", server_));
  }
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP resources/read canceled", server_));
  }
  std::string const params = "{\"uri\":" + json_string(uri) + "}";
  auto response = request("resources/read", params, options_.request_timeout, "timed out waiting for MCP resources/read", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  return parse_resource_read_result(response->result_json, response->raw_json, server_);
}

ava::core::Result<McpStdioClient::JsonRpcResponse> McpStdioClient::request(std::string_view method, std::string_view params_json,
                                                                           std::chrono::milliseconds timeout, std::string_view timeout_message,
                                                                           CancelCallback cancel_requested)
{
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  auto const request_id = "ava_mcp_" + std::to_string(next_request_id_++);
  std::string const request_json =
      "{\"jsonrpc\":\"2.0\",\"id\":" + json_string(request_id) + ",\"method\":" + json_string(method) + ",\"params\":" + std::string(params_json) + "}";
  Dout(dc::mcp, "operation=request state=write request_bytes=" << request_json.size());
  if (auto written = write_message(request_json, deadline, timeout, "timed out writing MCP request", "MCP server closed before response", cancel_requested);
      !written)
  {
    return std::unexpected(std::move(written.error()));
  }

  while (true)
  {
    auto message = read_message(deadline, timeout, timeout_message, "MCP server closed before response", cancel_requested);
    if (!message)
      return std::unexpected(std::move(message.error()));
    Dout(dc::mcp, "operation=request state=read response_bytes=" << message->size());
    if (!mcp_json_depth_within_limit(*message, kMaxMcpJsonDepth) ||
        ava::core::validate_strict_json(*message, kMaxMcpJsonDepth) != ava::core::StrictJsonStatus::Valid || !ava::core::json::is_valid_object(*message))
    {
      auto error = protocol_error("MCP response is not a valid JSON object with strict bounds", server_);
      error.with_context("response_bytes", std::to_string(message->size()));
      return std::unexpected(std::move(error));
    }
    auto const id = mcp_response_id(*message);
    if (!id)
      continue;
    if (*id != request_id)
    {
      auto error = protocol_error("MCP response id did not match request", server_);
      error.with_context("response_bytes", std::to_string(message->size()));
      return std::unexpected(std::move(error));
    }
    if (auto error_json = ava::core::json::object_field(*message, "error"))
    {
      Dout(dc::mcp, "operation=request state=remote_error error_bytes=" << error_json->size());
      auto error = protocol_error("MCP request returned a remote protocol error", server_);
      error.with_context("response_bytes", std::to_string(message->size()));
      error.with_context("error_bytes", std::to_string(error_json->size()));
      return std::unexpected(std::move(error));
    }
    auto result_json = ava::core::json::object_field(*message, "result");
    if (!result_json)
    {
      auto error = protocol_error("MCP response is missing result object", server_);
      error.with_context("response_bytes", std::to_string(message->size()));
      return std::unexpected(std::move(error));
    }
    return JsonRpcResponse{.result_json = std::move(*result_json), .raw_json = std::move(*message)};
  }
}

ava::core::VoidResult McpStdioClient::write_message(std::string_view message, std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                    std::string_view timeout_message, std::string_view closed_message, CancelCallback cancel_requested)
{
  if (auto reaped = reap_child(); !reaped)
    return std::unexpected(std::move(reaped.error()));
  if (stdin_fd_ < 0)
    return std::unexpected(peer_closed_error(closed_message));
  auto const frame = compact_json_line(message);
  if (frame.size() - 1 > options_.max_message_bytes)
  {
    auto error = protocol_error("MCP request exceeds message size cap", server_);
    error.with_context("max_bytes", std::to_string(options_.max_message_bytes));
    return std::unexpected(std::move(error));
  }
  std::size_t offset = 0;
  while (offset < frame.size())
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("MCP request canceled", server_));
    }
    auto const bytes = write_retry(stdin_fd_, frame.data() + offset, frame.size() - offset);
    if (bytes > 0)
    {
      offset += static_cast<std::size_t>(bytes);
      continue;
    }
    if (bytes == 0 || errno == EPIPE)
      return std::unexpected(peer_closed_error(closed_message));
    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      if (auto writable = wait_for_writable(deadline, timeout, timeout_message, closed_message, cancel_requested); !writable)
      {
        return std::unexpected(std::move(writable.error()));
      }
      continue;
    }
    return std::unexpected(errno_error("failed to write MCP request", server_));
  }
  return {};
}

ava::core::Result<std::optional<std::string>> McpStdioClient::try_extract_message()
{
  auto const newline = stdout_buffer_.find('\n');
  if (newline == std::string::npos)
  {
    if (stdout_buffer_.size() > options_.max_message_bytes)
    {
      auto error = protocol_error("MCP message exceeds size cap before newline", server_);
      error.with_context("max_bytes", std::to_string(options_.max_message_bytes));
      terminate_child();
      return std::unexpected(std::move(error));
    }
    return std::optional<std::string>{};
  }
  if (newline > options_.max_message_bytes)
  {
    auto error = protocol_error("MCP message exceeds size cap", server_);
    error.with_context("max_bytes", std::to_string(options_.max_message_bytes));
    terminate_child();
    return std::unexpected(std::move(error));
  }
  auto message = stdout_buffer_.substr(0, newline);
  stdout_buffer_.erase(0, newline + 1);
  if (!message.empty() && message.back() == '\r')
    message.pop_back();
  return std::optional<std::string>{std::move(message)};
}

ava::core::Result<std::string> McpStdioClient::read_message(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                            std::string_view timeout_message, std::string_view closed_message, CancelCallback cancel_requested)
{
  while (true)
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("MCP request canceled", server_));
    }
    auto extracted = try_extract_message();
    if (!extracted)
      return std::unexpected(std::move(extracted.error()));
    if (*extracted)
      return std::move(**extracted);

    if (stdout_fd_ < 0)
    {
      auto const reap_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
      do
      {
        if (auto reaped = reap_child(); !reaped)
          return std::unexpected(std::move(reaped.error()));
        if (child_exited_)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < reap_deadline);
      return std::unexpected(peer_closed_error(closed_message, stdout_buffer_.size()));
    }
    if (auto reaped = reap_child(); !reaped)
      return std::unexpected(std::move(reaped.error()));
    if (std::chrono::steady_clock::now() >= deadline)
    {
      auto error = protocol_error(std::string(timeout_message), server_);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      error.with_context("response_bytes", std::to_string(stdout_buffer_.size()));
      error.with_context("stderr_bytes", std::to_string(stderr_tail_.size()));
      error.with_context("stderr_truncated", stderr_truncated_ ? "true" : "false");
      terminate_child();
      return std::unexpected(std::move(error));
    }

    std::array<pollfd, 2> fds{pollfd{.fd = stdout_fd_, .events = POLLIN, .revents = 0}, pollfd{.fd = stderr_fd_, .events = POLLIN, .revents = 0}};
    int const poll_timeout = static_cast<int>(std::min<std::size_t>(remaining_ms(deadline), 100));
    int const polled = poll(fds.data(), fds.size(), poll_timeout);
    if (polled < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(errno_error("failed to poll MCP process pipes", server_));
    }
    if (polled == 0)
      continue;
    if (fds[1].revents != 0)
    {
      if (auto drained = drain_stderr(); !drained)
        return std::unexpected(std::move(drained.error()));
    }
    if (fds[0].revents != 0)
    {
      if (auto drained = drain_stdout(); !drained)
        return std::unexpected(std::move(drained.error()));
    }
  }
}

ava::core::VoidResult McpStdioClient::wait_for_writable(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                        std::string_view timeout_message, std::string_view closed_message, CancelCallback cancel_requested)
{
  while (true)
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("MCP request canceled", server_));
    }
    if (auto reaped = reap_child(); !reaped)
      return std::unexpected(std::move(reaped.error()));
    if (stdin_fd_ < 0)
      return std::unexpected(peer_closed_error(closed_message));
    if (std::chrono::steady_clock::now() >= deadline)
    {
      auto error = protocol_error(std::string(timeout_message), server_);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      terminate_child();
      return std::unexpected(std::move(error));
    }
    std::array<pollfd, 2> fds{pollfd{.fd = stdin_fd_, .events = POLLOUT, .revents = 0}, pollfd{.fd = stderr_fd_, .events = POLLIN, .revents = 0}};
    int const poll_timeout = static_cast<int>(std::min<std::size_t>(remaining_ms(deadline), 100));
    int const polled = poll(fds.data(), fds.size(), poll_timeout);
    if (polled < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(errno_error("failed to poll MCP request pipe", server_));
    }
    if (fds[1].revents != 0)
    {
      if (auto drained = drain_stderr(); !drained)
        return std::unexpected(std::move(drained.error()));
    }
    if ((fds[0].revents & POLLOUT) != 0)
      return {};
    if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      return std::unexpected(peer_closed_error(closed_message));
  }
}

ava::core::Error McpStdioClient::peer_closed_error(std::string_view closed_message, std::optional<std::size_t> response_bytes) const
{
  auto error = protocol_error(std::string(closed_message), server_);
  if (child_exited_)
    error.with_context("status", exit_detail(child_status_));
  if (response_bytes)
    error.with_context("response_bytes", std::to_string(*response_bytes));
  error.with_context("stderr_bytes", std::to_string(stderr_tail_.size()));
  error.with_context("stderr_truncated", stderr_truncated_ ? "true" : "false");
  return error;
}

ava::core::VoidResult McpStdioClient::drain_stdout()
{
  if (stdout_fd_ < 0)
    return {};
  std::array<char, 4096> buffer{};
  int reads = 0;
  while (true)
  {
    auto const bytes = read_retry(stdout_fd_, buffer.data(), buffer.size());
    if (bytes > 0)
    {
      Dout(dc::mcp, "operation=stdout state=read bytes=" << bytes);
      stdout_buffer_.append(buffer.data(), static_cast<std::size_t>(bytes));
      if (++reads >= kMaxDrainReadsPerPoll)
        return {};
      continue;
    }
    if (bytes == 0)
    {
      close_fd(stdout_fd_);
      return {};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return {};
    return std::unexpected(errno_error("failed to read MCP stdout", server_));
  }
}

ava::core::VoidResult McpStdioClient::drain_stderr()
{
  if (stderr_fd_ < 0)
    return {};
  std::array<char, 4096> buffer{};
  int reads = 0;
  while (true)
  {
    auto const bytes = read_retry(stderr_fd_, buffer.data(), buffer.size());
    if (bytes > 0)
    {
      Dout(dc::mcp, "operation=stderr state=read bytes=" << bytes << " retained_bytes=" << stderr_tail_.size());
      append_stderr(std::string_view(buffer.data(), static_cast<std::size_t>(bytes)));
      if (++reads >= kMaxDrainReadsPerPoll)
        return {};
      continue;
    }
    if (bytes == 0)
    {
      close_fd(stderr_fd_);
      return {};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return {};
    return std::unexpected(errno_error("failed to read MCP stderr", server_));
  }
}

ava::core::VoidResult McpStdioClient::reap_child()
{
  if (pid_ <= 0 || child_exited_)
    return {};
  int status = 0;
  auto const waited = waitpid_retry(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (waited < 0)
  {
    if (errno == ECHILD)
    {
      pid_ = -1;
      child_exited_ = true;
      return {};
    }
    return std::unexpected(errno_error("failed to reap MCP server process", server_));
  }
  if (waited == 0)
    return {};
  child_status_ = status;
  child_exited_ = true;
  close_fd(stdin_fd_);
  return {};
}

ava::core::VoidResult McpStdioClient::set_pipe_nonblocking(int fd)
{
  int const flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return std::unexpected(errno_error("failed to inspect MCP pipe flags", server_));
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    return std::unexpected(errno_error("failed to set MCP pipe nonblocking", server_));
  return {};
}

void McpStdioClient::append_stderr(std::string_view chunk)
{
  if (chunk.size() >= options_.max_stderr_bytes)
  {
    stderr_tail_ = std::string(chunk.substr(chunk.size() - options_.max_stderr_bytes));
    stderr_truncated_ = true;
    return;
  }
  if (stderr_tail_.size() + chunk.size() > options_.max_stderr_bytes)
  {
    auto const drop = stderr_tail_.size() + chunk.size() - options_.max_stderr_bytes;
    stderr_tail_.erase(0, drop);
    stderr_truncated_ = true;
  }
  stderr_tail_.append(chunk);
}

void McpStdioClient::close_fds() noexcept
{
  close_fd(stdin_fd_);
  close_fd(stdout_fd_);
  close_fd(stderr_fd_);
}

void McpStdioClient::terminate_child() noexcept
{
  if (pid_ <= 0 || child_exited_)
    return;
  close_fd(stdin_fd_);
  if (can_signal_group_)
  {
    kill(-static_cast<pid_t>(pid_), SIGTERM);
  }
  else
  {
    kill(static_cast<pid_t>(pid_), SIGTERM);
  }
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (std::chrono::steady_clock::now() < deadline)
  {
    int status = 0;
    auto const waited = waitpid_retry(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (waited == pid_)
    {
      child_status_ = status;
      child_exited_ = true;
      return;
    }
    if (waited < 0)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (can_signal_group_)
  {
    kill(-static_cast<pid_t>(pid_), SIGKILL);
  }
  else
  {
    kill(static_cast<pid_t>(pid_), SIGKILL);
  }
  int status = 0;
  if (waitpid_retry(static_cast<pid_t>(pid_), &status, 0) == pid_)
  {
    child_status_ = status;
    child_exited_ = true;
  }
}

ava::core::VoidResult McpStdioClient::shutdown(std::chrono::milliseconds grace)
{
  close_fd(stdin_fd_);
  auto const deadline = std::chrono::steady_clock::now() + grace;
  while (pid_ > 0 && !child_exited_ && std::chrono::steady_clock::now() < deadline)
  {
    if (auto drained = drain_stdout(); !drained)
      return std::unexpected(std::move(drained.error()));
    if (auto drained = drain_stderr(); !drained)
      return std::unexpected(std::move(drained.error()));
    if (auto reaped = reap_child(); !reaped)
      return std::unexpected(std::move(reaped.error()));
    if (!child_exited_)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (pid_ > 0 && !child_exited_)
    terminate_child();
  close_fds();
  return {};
}

}  // namespace ava::mcp
