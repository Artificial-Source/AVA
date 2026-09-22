#include "sys.h"
#include "ava/diagnostics/safe_failure.h"
#include "ava/mcp/config.h"
#include "ava/mcp/stdio_client.h"
#include "ava/mcp/stdio_client_support.h"
#include "ava/mcp/tool_broker.h"
#include "ava/permissions/permission.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace ava::mcp {
namespace {

struct McpToolBinding
{
  McpServerConfig server;
  McpToolDescription tool;
  std::string model_tool_name;
};

struct McpResourceBinding
{
  McpServerConfig server;
  McpResourceDescription resource;
  std::string model_tool_name;
};

bool is_canceled_error(ava::core::Error const& error)
{
  return error.code() == ava::core::ErrorCode::Canceled;
}

bool is_canceled(ava::tools::ToolContext const& context)
{
  return context.cancel_requested && context.cancel_requested();
}

ava::tools::ToolDispatchResult safe_tool_failure_result(ava::tools::ProviderToolCall const& call, ava::diagnostics::SafeFailure const& failure, bool canceled)
{
  auto const json = ava::diagnostics::serialize_safe_failure_json(failure);
  auto const message = ava::diagnostics::serialize_safe_failure_human(failure);
  ava::tools::ToolResultPayload payload;
  payload.status = canceled ? ava::tools::ToolResultStatus::Canceled : ava::tools::ToolResultStatus::Error;
  payload.content = json;
  payload.content_type = "application/json";
  payload.error_category = std::string(ava::diagnostics::to_string(failure.category));
  payload.error_code = std::string(ava::diagnostics::to_string(failure.code));
  payload.error_message = message;
  return ava::tools::ToolDispatchResult{.call_id = call.id, .name = call.name, .success = false, .result_text = json, .payload = std::move(payload)};
}

ava::tools::ToolDispatchResult tool_error_result(ava::tools::ProviderToolCall const& call, ava::core::Error const& error)
{
  bool const canceled = is_canceled_error(error);
  auto const failure = canceled ? ava::diagnostics::canceled_failure(ava::diagnostics::ComponentClass::Mcp)
                                : ava::diagnostics::safe_failure_from_error(ava::diagnostics::ComponentClass::Mcp, error);
  return safe_tool_failure_result(call, failure, canceled);
}

ava::tools::ToolDispatchResult remote_tool_error_result(ava::tools::ProviderToolCall const& call)
{
  return safe_tool_failure_result(call, ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Mcp), false);
}

ava::core::Error mcp_tool_error(ava::core::ErrorCategory category, std::string message, McpToolBinding const& binding,
                                ava::core::ErrorCode code = ava::core::ErrorCode::Unspecified)
{
  auto error = ava::core::Error(category, std::move(message), code);
  error.with_context("mcp_server", binding.server.id);
  error.with_context("mcp_tool", binding.tool.name);
  error.with_context("tool", binding.model_tool_name);
  if (!binding.server.source_path.empty())
    error.with_context("config", binding.server.source_path.string());
  return error;
}

ava::core::Error mcp_resource_error(ava::core::ErrorCategory category, std::string message, McpResourceBinding const& binding,
                                    ava::core::ErrorCode code = ava::core::ErrorCode::Unspecified)
{
  auto error = ava::core::Error(category, std::move(message), code);
  error.with_context("mcp_server", binding.server.id);
  error.with_context("mcp_resource", binding.resource.uri);
  error.with_context("tool", binding.model_tool_name);
  if (!binding.server.source_path.empty())
    error.with_context("config", binding.server.source_path.string());
  return error;
}

ava::core::Error mcp_server_error(ava::core::ErrorCategory category, std::string message, McpServerConfig const& server,
                                  ava::core::ErrorCode code = ava::core::ErrorCode::Unspecified)
{
  auto error = ava::core::Error(category, std::move(message), code);
  error.with_context("mcp_server", server.id);
  if (!server.source_path.empty())
    error.with_context("config", server.source_path.string());
  return error;
}

ava::core::Error mcp_tool_canceled_error(McpToolBinding const& binding)
{
  auto error = mcp_tool_error(ava::core::ErrorCategory::Unknown, "MCP tool call canceled", binding, ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  return error;
}

ava::core::Error mcp_resource_canceled_error(McpResourceBinding const& binding)
{
  auto error = mcp_resource_error(ava::core::ErrorCategory::Unknown, "MCP resource read canceled", binding, ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  return error;
}

std::string command_text(McpServerConfig const& server)
{
  std::string text = server.command;
  for (auto const& arg : server.args) text += " " + arg;
  return text;
}

McpStdioClientOptions client_options_for_context(ava::tools::ToolContext const& context);

std::string launch_permission_command(ava::tools::ToolContext const& context, McpServerConfig const& server)
{
  if (!context.session_mcp_config)
    return command_text(server);
  auto const options = client_options_for_context(context);
  return session_mcp_launch_identity(server, child_working_dir(server, options));
}

ava::core::VoidResult ensure_mcp_server_permission(ava::tools::ToolContext const& context, McpServerConfig const& server, std::string_view tool_name)
{
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpServerLaunch, server.source_path,
                                                      launch_permission_command(context, server), tool_name, "MCP server launch requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpServerConnect, server.source_path, server.id, tool_name,
                                                      "MCP server connection requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  return {};
}

McpStdioClientOptions client_options_for_context(ava::tools::ToolContext const& context)
{
  McpStdioClientOptions options;
  // Adapter-supplied MCP is session scoped and follows the persisted cwd.
  // Legacy global/project configuration retains its established workspace/config cwd and environment rules.
  options.workspace_dir = context.session_mcp_config && !context.current_dir.empty() ? context.current_dir : context.workspace_dir;
  options.clean_environment = context.session_mcp_config != nullptr;
  return options;
}

std::string result_json(ava::tools::ProviderToolCall const& call, McpToolBinding const& binding, McpToolCallResult const& result)
{
  return "{\"tool\":\"" + ava::core::json::escape(call.name) + "\",\"ok\":true,\"server\":\"" + ava::core::json::escape(binding.server.id) +
         "\",\"mcp_tool\":\"" + ava::core::json::escape(binding.tool.name) + "\",\"content\":\"" + ava::core::json::escape(result.content) + "\"}";
}

std::string resource_result_json(ava::tools::ProviderToolCall const& call, McpResourceBinding const& binding, McpResourceReadResult const& result)
{
  auto const uri = result.uri.empty() ? binding.resource.uri : result.uri;
  auto const mime_type = result.mime_type.empty() ? binding.resource.mime_type : result.mime_type;
  return "{\"tool\":\"" + ava::core::json::escape(call.name) + "\",\"ok\":true,\"server\":\"" + ava::core::json::escape(binding.server.id) +
         "\",\"mcp_resource\":\"" + ava::core::json::escape(uri) + "\",\"mime_type\":\"" + ava::core::json::escape(mime_type) + "\",\"content\":\"" +
         ava::core::json::escape(result.content) + "\"}";
}

ava::tools::ToolResultPayload result_payload(std::string const& text)
{
  ava::tools::ToolResultPayload payload;
  payload.status = ava::tools::ToolResultStatus::Success;
  payload.content_type = "application/json";
  payload.content = text;
  return payload;
}

ava::tools::ToolResultPayload resource_result_payload(std::string const& text)
{
  ava::tools::ToolResultPayload payload;
  payload.status = ava::tools::ToolResultStatus::Success;
  payload.content_type = "application/json";
  payload.content = text;
  return payload;
}

bool json_object_has_no_fields(std::string_view json)
{
  std::size_t index = 0;
  while (index < json.size() && std::isspace(static_cast<unsigned char>(json[index])) != 0) ++index;
  if (index >= json.size() || json[index] != '{')
    return false;
  ++index;
  while (index < json.size() && std::isspace(static_cast<unsigned char>(json[index])) != 0) ++index;
  return index < json.size() && json[index] == '}';
}

std::string fnv1a64_hex(std::string_view value)
{
  std::uint64_t hash = 14695981039346656037ULL;
  for (char const ch : value)
  {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ULL;
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string suffix;
  suffix.reserve(16);
  for (int shift = 60; shift >= 0; shift -= 4) suffix.push_back(kHex[(hash >> shift) & 0x0F]);
  return suffix;
}

ava::tools::ToolDispatchResult dispatch_mcp_tool(ava::tools::ToolContext const& context, ava::tools::ProviderToolCall const& call,
                                                 McpToolBinding const& binding)
{
  if (is_canceled(context))
  {
    return tool_error_result(call, mcp_tool_canceled_error(binding));
  }
  if (!ava::core::json::is_valid_object(call.arguments_json))
  {
    auto error = mcp_tool_error(ava::core::ErrorCategory::InvalidArgument, "MCP tool arguments must be a JSON object", binding);
    return tool_error_result(call, error);
  }

  auto tool_context = context;
  tool_context.permission_tool_name = call.name;
  tool_context.current_tool_name = call.name;
  tool_context.current_call_id = call.id;
  if (auto permission = ensure_mcp_server_permission(tool_context, binding.server, call.name); !permission)
  {
    return tool_error_result(call, permission.error());
  }
  if (is_canceled(tool_context))
  {
    return tool_error_result(call, mcp_tool_canceled_error(binding));
  }
  auto const command = binding.server.id + ":" + binding.tool.name;
  if (auto permission = ava::tools::ensure_permission(tool_context, ava::permissions::Operation::McpToolCall, binding.server.source_path, command, call.name,
                                                      "MCP tool call requires permission");
      !permission)
  {
    return tool_error_result(call, permission.error());
  }
  if (is_canceled(tool_context))
  {
    return tool_error_result(call, mcp_tool_canceled_error(binding));
  }
  if (auto started = ava::tools::announce_tool_execution_start(tool_context); !started)
    return tool_error_result(call, started.error());

  auto client = McpStdioClient::start(binding.server, client_options_for_context(context), context.cancel_requested);
  if (!client)
    return tool_error_result(call, client.error());
  auto result = (*client)->call_tool(binding.tool.name, call.arguments_json, context.cancel_requested);
  auto shutdown = (*client)->shutdown();
  if (!result)
    return tool_error_result(call, result.error());
  if (!shutdown)
    return tool_error_result(call, shutdown.error());

  if (result->is_error)
    return remote_tool_error_result(call);
  auto text = result_json(call, binding, *result);
  return ava::tools::ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = text, .payload = result_payload(text)};
}

ava::tools::ToolDispatchResult dispatch_mcp_resource(ava::tools::ToolContext const& context, ava::tools::ProviderToolCall const& call,
                                                     McpResourceBinding const& binding)
{
  if (is_canceled(context))
  {
    return tool_error_result(call, mcp_resource_canceled_error(binding));
  }
  if (!ava::core::json::is_valid_object(call.arguments_json) || !json_object_has_no_fields(call.arguments_json))
  {
    auto error = mcp_resource_error(ava::core::ErrorCategory::InvalidArgument, "MCP resource read arguments must be an empty JSON object", binding);
    return tool_error_result(call, error);
  }

  auto tool_context = context;
  tool_context.permission_tool_name = call.name;
  tool_context.current_tool_name = call.name;
  tool_context.current_call_id = call.id;
  if (auto permission = ensure_mcp_server_permission(tool_context, binding.server, call.name); !permission)
  {
    return tool_error_result(call, permission.error());
  }
  if (is_canceled(tool_context))
  {
    return tool_error_result(call, mcp_resource_canceled_error(binding));
  }
  auto const command = binding.server.id + ":" + binding.resource.uri;
  if (auto permission = ava::tools::ensure_permission(tool_context, ava::permissions::Operation::McpResourceRead, binding.server.source_path, command,
                                                      call.name, "MCP resource read requires permission");
      !permission)
  {
    return tool_error_result(call, permission.error());
  }
  if (is_canceled(tool_context))
  {
    return tool_error_result(call, mcp_resource_canceled_error(binding));
  }
  if (auto started = ava::tools::announce_tool_execution_start(tool_context); !started)
    return tool_error_result(call, started.error());

  auto client = McpStdioClient::start(binding.server, client_options_for_context(context), context.cancel_requested);
  if (!client)
    return tool_error_result(call, client.error());
  auto result = (*client)->read_resource(binding.resource.uri, context.cancel_requested);
  auto shutdown = (*client)->shutdown();
  if (!result)
    return tool_error_result(call, result.error());
  if (!shutdown)
    return tool_error_result(call, shutdown.error());

  auto text = resource_result_json(call, binding, *result);
  return ava::tools::ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = text, .payload = resource_result_payload(text)};
}

std::string schema_json(std::string_view model_tool_name, McpToolDescription const& tool)
{
  auto const description = tool.description.empty() ? std::string("MCP tool ") + tool.name : tool.description;
  return "{\"type\":\"function\",\"name\":\"" + ava::core::json::escape(model_tool_name) + "\",\"description\":\"" + ava::core::json::escape(description) +
         "\",\"parameters\":" + tool.input_schema_json + '}';
}

std::string resource_schema_json(std::string_view model_tool_name, McpServerConfig const& server)
{
  auto const description = "Read an approved MCP resource from " + server.id;
  return "{\"type\":\"function\",\"name\":\"" + ava::core::json::escape(model_tool_name) + "\",\"description\":\"" + ava::core::json::escape(description) +
         "\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}";
}

McpBrokeredTool brokered_tool(std::shared_ptr<McpToolBinding const> binding)
{
  auto const description =
      binding->tool.description.empty() ? std::string("MCP tool ") + binding->tool.name + " from " + binding->server.id : binding->tool.description;
  return McpBrokeredTool{.model_tool_name = binding->model_tool_name,
                         .description = description,
                         .schema_json = schema_json(binding->model_tool_name, binding->tool),
                         .permission_category = "mcp.tool.call",
                         .output_bound_summary = "MCP tool output is bounded by JSON-RPC message size",
                         .execution_mode = "mcp_stdio_process",
                         .event_rendering_hint = "mcp_tool",
                         .description_family = "mcp",
                         .source_id = binding->server.id,
                         .mcp_server = binding->server.id,
                         .mcp_name = binding->tool.name,
                         .resource_uri = {},
                         .executor = [binding = std::move(binding)](ava::tools::ToolContext const& tool_context, ava::tools::ProviderToolCall const& call) {
                           return dispatch_mcp_tool(tool_context, call, *binding);
                         }};
}

McpBrokeredTool brokered_resource(std::shared_ptr<McpResourceBinding const> binding)
{
  auto const description = "Read an approved MCP resource from " + binding->server.id;
  return McpBrokeredTool{.model_tool_name = binding->model_tool_name,
                         .description = description,
                         .schema_json = resource_schema_json(binding->model_tool_name, binding->server),
                         .permission_category = "mcp.resource.read",
                         .output_bound_summary = "MCP resource output is bounded by JSON-RPC message size",
                         .execution_mode = "mcp_stdio_process",
                         .event_rendering_hint = "mcp_resource",
                         .description_family = "mcp",
                         .source_id = binding->server.id,
                         .mcp_server = binding->server.id,
                         .mcp_name = {},
                         .resource_uri = binding->resource.uri,
                         .executor = [binding = std::move(binding)](ava::tools::ToolContext const& tool_context, ava::tools::ProviderToolCall const& call) {
                           return dispatch_mcp_resource(tool_context, call, *binding);
                         }};
}

McpConfigLoadOptions config_options_for_context(ava::tools::ToolContext const& context)
{
  auto options = default_mcp_config_options(context.workspace_dir);
  if (!context.include_global_mcp_config)
  {
    options.global_config_file.clear();
  }
  else if (!context.mcp_global_config_file.empty())
  {
    options.global_config_file = context.mcp_global_config_file;
  }
  if (!context.include_project_mcp_config)
  {
    options.project_config_file.clear();
  }
  else if (!context.mcp_project_config_file.empty())
  {
    options.project_config_file = context.mcp_project_config_file;
  }
  return options;
}

ava::core::Result<std::vector<McpToolDescription>> discover_server_tools(ava::tools::ToolContext const& context, McpServerConfig const& server)
{
  if (is_canceled(context))
  {
    return std::unexpected(mcp_server_error(ava::core::ErrorCategory::Unknown, "MCP tool discovery canceled", server, ava::core::ErrorCode::Canceled));
  }
  auto tool_context = context;
  tool_context.permission_tool_name = "mcp_discovery";
  tool_context.current_tool_name = "mcp_discovery";
  if (auto permission = ensure_mcp_server_permission(tool_context, server, "mcp_discovery"); !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  if (is_canceled(tool_context))
  {
    return std::unexpected(mcp_server_error(ava::core::ErrorCategory::Unknown, "MCP tool discovery canceled", server, ava::core::ErrorCode::Canceled));
  }
  auto client = McpStdioClient::start(server, client_options_for_context(context), context.cancel_requested);
  if (!client)
    return std::unexpected(std::move(client.error()));
  auto tools = (*client)->list_tools(context.cancel_requested);
  auto shutdown = (*client)->shutdown();
  if (!tools)
    return std::unexpected(std::move(tools.error()));
  if (!shutdown)
    return std::unexpected(std::move(shutdown.error()));
  return tools;
}

ava::core::Result<std::vector<McpResourceDescription>> discover_server_resources(ava::tools::ToolContext const& context, McpServerConfig const& server)
{
  if (is_canceled(context))
  {
    return std::unexpected(mcp_server_error(ava::core::ErrorCategory::Unknown, "MCP resource discovery canceled", server, ava::core::ErrorCode::Canceled));
  }
  auto tool_context = context;
  tool_context.permission_tool_name = "mcp_resource_discovery";
  tool_context.current_tool_name = "mcp_resource_discovery";
  if (auto permission = ensure_mcp_server_permission(tool_context, server, "mcp_resource_discovery"); !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  if (is_canceled(tool_context))
  {
    return std::unexpected(mcp_server_error(ava::core::ErrorCategory::Unknown, "MCP resource discovery canceled", server, ava::core::ErrorCode::Canceled));
  }
  auto client = McpStdioClient::start(server, client_options_for_context(context), context.cancel_requested);
  if (!client)
    return std::unexpected(std::move(client.error()));
  auto resources = (*client)->list_resources(context.cancel_requested);
  auto shutdown = (*client)->shutdown();
  if (!resources)
    return std::unexpected(std::move(resources.error()));
  if (!shutdown)
    return std::unexpected(std::move(shutdown.error()));
  return resources;
}

}  // namespace

std::string session_mcp_launch_identity(McpServerConfig const& server, std::filesystem::path const& canonical_child_cwd)
{
  std::string identity = "{\"argv\":[\"" + ava::core::json::escape(server.command) + "\"";
  for (auto const& arg : server.args) identity += ",\"" + ava::core::json::escape(arg) + "\"";
  identity += "],\"env\":[";

  auto environment = server.env;
  std::ranges::sort(environment, [](auto const& left, auto const& right) {
    if (left.first != right.first)
      return left.first < right.first;
    return left.second < right.second;
  });
  for (std::size_t index = 0; index < environment.size(); ++index)
  {
    if (index > 0)
      identity += ',';
    identity +=
        "{\"name\":\"" + ava::core::json::escape(environment[index].first) + "\",\"value\":\"" + ava::core::json::escape(environment[index].second) + "\"}";
  }
  identity += "],\"cwd\":\"" + ava::core::json::escape(canonical_child_cwd.string()) + "\",\"clean_environment\":true}";
  return identity;
}

std::string mcp_model_tool_name(std::string_view server_id, std::string_view tool_name)
{
  auto sanitize = [](std::string_view value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    bool last_was_separator = false;
    for (char const ch : value)
    {
      auto const byte = static_cast<unsigned char>(ch);
      if (std::isalnum(byte) != 0)
      {
        sanitized.push_back(static_cast<char>(std::tolower(byte)));
        last_was_separator = false;
      }
      else if (!last_was_separator)
      {
        sanitized.push_back('_');
        last_was_separator = true;
      }
    }
    while (!sanitized.empty() && sanitized.back() == '_') sanitized.pop_back();
    if (sanitized.empty())
      return std::string("tool");
    return sanitized;
  };
  return "mcp_" + sanitize(server_id) + "_" + sanitize(tool_name);
}

std::string mcp_model_resource_tool_name(std::string_view server_id, std::string_view resource_uri)
{
  return mcp_model_tool_name(server_id, "resource_" + fnv1a64_hex(server_id) + "_" + fnv1a64_hex(resource_uri));
}

ava::core::VoidResult visit_enabled_mcp_tools(ava::tools::ToolContext const& context, McpBrokeredToolVisitor const& visitor)
{
  auto const exact_composition = context.exact_builtin_tool_names.has_value();
  std::shared_ptr<McpConfig const> config = context.session_mcp_config;
  if (exact_composition && !config)
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "exact tool composition requires an immutable session MCP configuration"));
  if (!config)
  {
    auto loaded = load_mcp_config(config_options_for_context(context));
    // Global/project MCP remains a legacy optional extension.
    if (!loaded)
      return {};
    config = std::make_shared<McpConfig const>(std::move(*loaded));
  }

  for (auto const& server : config->servers)
  {
    if (!server.enabled)
      continue;
    auto tools = discover_server_tools(context, server);
    if (!tools)
    {
      if (exact_composition)
        return std::unexpected(std::move(tools.error()));
    }
    else
    {
      for (auto const& tool : *tools)
      {
        auto model_tool_name = mcp_model_tool_name(server.id, tool.name);
        auto binding = std::make_shared<McpToolBinding const>(McpToolBinding{.server = server, .tool = tool, .model_tool_name = std::move(model_tool_name)});
        auto descriptor = brokered_tool(std::move(binding));
        if (auto visited = visitor(descriptor); !visited)
          return std::unexpected(std::move(visited.error()));
      }
    }

    // Exact composition mirrors MCP tools/list exactly. Resource pseudo-tools
    // are a legacy AVA extension and are not part of that registry.
    if (exact_composition)
      continue;

    auto resources = discover_server_resources(context, server);
    if (!resources)
      continue;
    for (auto const& resource : *resources)
    {
      auto model_tool_name = mcp_model_resource_tool_name(server.id, resource.uri);
      auto binding =
          std::make_shared<McpResourceBinding const>(McpResourceBinding{.server = server, .resource = resource, .model_tool_name = std::move(model_tool_name)});
      auto descriptor = brokered_resource(std::move(binding));
      if (auto visited = visitor(descriptor); !visited)
        return std::unexpected(std::move(visited.error()));
    }
  }
  return {};
}

}  // namespace ava::mcp
