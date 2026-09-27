#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_mcp.h"
#include "ava/app/command_registry_detail.h"
#include "ava/app/command_tools.h"
#include "ava/tools/tool_permission.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime/command_names.h"
#include "ava/mcp/config.h"
#include "ava/mcp/stdio_client.h"
#include "ava/mcp/tool_broker.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/string_utils.h"

#include <sstream>
#include <utility>

namespace ava::app {
namespace {

enum class McpCommandTextMode
{
  Raw,
  Sanitized,
};

std::string mcp_scope_text(ava::mcp::McpServerScope scope)
{
  return std::string(ava::mcp::to_string(scope));
}

std::string mcp_status_text(bool enabled)
{
  return enabled ? "enabled" : "disabled";
}

ava::mcp::McpServerConfig const* find_mcp_server(ava::mcp::McpConfig const& config, std::string_view server_id)
{
  for (auto const& server : config.servers)
  {
    if (server.id == server_id)
      return &server;
  }
  return nullptr;
}

std::string mcp_command_text(ava::mcp::McpServerConfig const& server, McpCommandTextMode mode)
{
  auto render = [mode](std::string const& part) { return mode == McpCommandTextMode::Sanitized ? sanitize_inline_text(part) : part; };
  std::string text = render(server.command);
  for (auto const& arg : server.args) text += " " + render(arg);
  return text;
}

std::string mcp_display_path(std::filesystem::path const& path, runtime::session_ts const& unlocked_session)
{
  auto const current_dir = runtime::session_ts::crat(unlocked_session)->current_dir();
  return sanitize_inline_text(display_path(path, current_dir));
}

std::string mcp_config_path_text(std::filesystem::path const& path, runtime::session_ts const& unlocked_session)
{
  if (path.empty())
    return "none";
  return mcp_display_path(path, unlocked_session);
}

std::string format_mcp_server_not_found_text(ava::mcp::McpConfig const& config, std::string_view server_id)
{
  std::string output = "MCP server not found: " + sanitize_inline_text(std::string(server_id));
  if (!config.servers.empty())
  {
    output += "\nConfigured MCP servers:";
    for (auto const& server : config.servers) output += "\n  " + sanitize_inline_text(server.id);
  }
  return output;
}

std::string format_mcp_list_text(ava::mcp::McpConfig const& config, runtime::session_ts const& unlocked_session)
{
  std::ostringstream output;
  output << "MCP servers:\n";
  output << "  global config: " << mcp_config_path_text(config.global_config_file, unlocked_session) << "\n";
  output << "  project config: " << mcp_config_path_text(config.project_config_file, unlocked_session) << "\n";
  if (config.servers.empty())
  {
    output << "  none";
    return output.str();
  }
  for (auto const& server : config.servers)
  {
    output << "  " << sanitize_inline_text(server.id) << "  " << mcp_status_text(server.enabled) << "  " << mcp_scope_text(server.scope) << "  "
           << sanitize_inline_text(server.name) << "\n";
  }
  auto text = output.str();
  if (!text.empty() && text.back() == '\n')
    text.pop_back();
  return text;
}

std::string format_mcp_inspect_text(ava::mcp::McpServerConfig const& server, runtime::session_ts const& unlocked_session)
{
  std::ostringstream output;
  output << "MCP server " << sanitize_inline_text(server.id) << "\n";
  output << "  name: " << sanitize_inline_text(server.name) << "\n";
  output << "  status: " << mcp_status_text(server.enabled) << "\n";
  output << "  scope: " << mcp_scope_text(server.scope) << "\n";
  output << "  config: " << mcp_config_path_text(server.source_path, unlocked_session) << "\n";
  output << "  command: " << mcp_command_text(server, McpCommandTextMode::Sanitized) << "\n";
  output << "  note: stdio MCP servers are launched per discovery or tool call and are not kept resident.";
  return output.str();
}

std::string format_mcp_tools_text(ava::mcp::McpServerConfig const& server, ava::mcp::McpInitialization const& initialization,
                                  std::vector<ava::mcp::McpToolDescription> const& tools)
{
  std::ostringstream output;
  output << "MCP tools for " << sanitize_inline_text(server.id) << "\n";
  output << "  server: " << sanitize_inline_text(initialization.server_name);
  if (!initialization.server_version.empty())
    output << " " << sanitize_inline_text(initialization.server_version);
  output << "\n";
  if (tools.empty())
  {
    output << "  none";
    return output.str();
  }
  for (auto const& tool : tools)
  {
    output << "  " << sanitize_inline_text(tool.name) << "  " << sanitize_inline_text(ava::mcp::mcp_model_tool_name(server.id, tool.name));
    if (!tool.description.empty())
      output << "  " << sanitize_inline_text(tool.description);
    output << "\n";
  }
  auto text = output.str();
  if (!text.empty() && text.back() == '\n')
    text.pop_back();
  return text;
}

}  // namespace

ava::core::Result<CommandResult> run_mcp_command(runtime::session_ts& unlocked_session, CommandRequest const& request)
{
  CommandResult result;
  result.handled = true;
  auto const usage = [&]() {
    add_output(result, missing_argument("/mcp <list|inspect|tools|restart> [server_id]"));
    return result;
  };

  auto const argument = command_argument(request.command, "/mcp");
  auto const args = split_command_arguments(argument);
  if (args.empty())
    return usage();

  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  auto config = ava::mcp::load_mcp_config(resource_policy.mcp_config);
  if (!config)
  {
    add_output(result, config.error().format());
    return result;
  }

  auto const& subcommand = args[0];
  if (subcommand == "list")
  {
    if (args.size() != 1)
      return usage();
    add_output(result, format_mcp_list_text(*config, unlocked_session));
    return result;
  }

  if ((subcommand == "inspect" || subcommand == "tools" || subcommand == "restart") && args.size() != 2)
  {
    return usage();
  }

  if (subcommand == "inspect")
  {
    auto const* server = find_mcp_server(*config, args[1]);
    if (!server)
    {
      add_output(result, format_mcp_server_not_found_text(*config, args[1]));
      return result;
    }
    add_output(result, format_mcp_inspect_text(*server, unlocked_session));
    return result;
  }

  if (subcommand == "restart")
  {
    auto const* server = find_mcp_server(*config, args[1]);
    if (!server)
    {
      add_output(result, format_mcp_server_not_found_text(*config, args[1]));
      return result;
    }
    add_output(result, "MCP server " + sanitize_inline_text(server->id) +
                           " uses per-request stdio processes; the next discovery or tool call will launch a fresh "
                           "process.");
    return result;
  }

  if (subcommand == "tools")
  {
    auto const* server = find_mcp_server(*config, args[1]);
    if (!server)
    {
      add_output(result, format_mcp_server_not_found_text(*config, args[1]));
      return result;
    }
    if (!server->enabled)
    {
      add_output(result, "MCP server is disabled: " + sanitize_inline_text(server->id));
      return result;
    }

    auto context = make_tool_context(unlocked_session, request.permission_resolver);
    context.permission_tool_name = "mcp_tools";
    auto const call_id = ava::core::make_id("cmd");
    if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "mcp_tools", server->id); !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }

    auto fail = [&](ava::core::Error const& error) -> ava::core::Result<CommandResult> {
      auto const text = error.format();
      if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "mcp_tools", ava::agent::ToolTimelineStatus::Error, text);
          !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
      add_output(result, text);
      return result;
    };

    if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpServerLaunch, server->source_path,
                                                        mcp_command_text(*server, McpCommandTextMode::Sanitized),
                                                        "mcp_tools", "MCP server launch requires permission");
        !permission)
    {
      return fail(permission.error());
    }
    if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpServerConnect, server->source_path, server->id, "mcp_tools",
                                                        "MCP server connection requires permission");
        !permission)
    {
      return fail(permission.error());
    }

    ava::mcp::McpStdioClientOptions options;
    options.workspace_dir = runtime::session_ts::rat(unlocked_session)->workspace_dir();
    auto client = ava::mcp::McpStdioClient::start(*server, options);
    if (!client)
      return fail(client.error());
    auto tools = (*client)->list_tools();
    auto const initialization = (*client)->initialization();
    auto shutdown = (*client)->shutdown();
    if (!tools)
      return fail(tools.error());
    if (!shutdown)
      return fail(shutdown.error());

    if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "mcp_tools", ava::agent::ToolTimelineStatus::Success,
                                           std::to_string(tools->size()) + " tools");
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, format_mcp_tools_text(*server, initialization, *tools));
    return result;
  }

  return usage();
}

namespace {

ava::core::Result<std::string> mcp_prompt_arguments_json(CommandRegistryEntry const& entry, std::string_view argument_text)
{
  auto const trimmed = core::trim_view(argument_text);
  if (!trimmed.empty() && trimmed.front() == '{')
  {
    auto json = std::string(trimmed);
    if (!ava::core::json::is_valid_object(json))
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "MCP prompt arguments must be a JSON object"));
    return json;
  }

  auto tokens = parse_command_argument_tokens(argument_text);
  if (!tokens)
    return std::unexpected(std::move(tokens.error()));
  if (tokens->size() > entry.mcp_arguments.size())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "too many MCP prompt arguments"));
  for (std::size_t index = 0; index < entry.mcp_arguments.size(); ++index)
  {
    if (entry.mcp_arguments[index].required && index >= tokens->size())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "missing required MCP prompt argument");
      error.with_context("argument", entry.mcp_arguments[index].name);
      return std::unexpected(std::move(error));
    }
  }

  std::string json = "{";
  for (std::size_t index = 0; index < tokens->size(); ++index)
  {
    if (index > 0)
      json += ',';
    json += "\"" + ava::core::json::escape(entry.mcp_arguments[index].name) + "\":\"" + ava::core::json::escape((*tokens)[index]) + "\"";
  }
  json += '}';
  return json;
}

ava::core::VoidResult ensure_mcp_prompt_permissions(ava::tools::ToolContext const& context, ava::mcp::McpServerConfig const& server,
                                                    CommandRegistryEntry const& entry)
{
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpServerLaunch, server.source_path,
                                                      mcp_command_text(server, McpCommandTextMode::Raw),
                                                      "mcp_prompt", "MCP server launch requires permission");
      !permission)
    return std::unexpected(std::move(permission.error()));
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpServerConnect, server.source_path, server.id, "mcp_prompt",
                                                      "MCP server connection requires permission");
      !permission)
    return std::unexpected(std::move(permission.error()));
  auto const command = entry.mcp_server_id + ":" + entry.mcp_prompt_name;
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpToolCall, server.source_path, command, "mcp_prompt",
                                                      "MCP prompt retrieval requires permission");
      !permission)
    return std::unexpected(std::move(permission.error()));
  return {};
}

ava::core::VoidResult ensure_mcp_prompt_server_permission(ava::tools::ToolContext const& context, ava::mcp::McpServerConfig const& server)
{
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpServerLaunch, server.source_path,
                                                      mcp_command_text(server, McpCommandTextMode::Raw),
                                                      "mcp_prompts", "MCP server launch requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::McpServerConnect, server.source_path, server.id, "mcp_prompts",
                                                      "MCP server connection requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  return {};
}

}  // namespace

ava::core::Result<std::string> mcp_prompt_message(runtime::session_ts& unlocked_session, CommandRequest const& request, CommandRegistryEntry const& entry,
                                                  std::string_view argument_text)
{
  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  auto config = ava::mcp::load_mcp_config(resource_policy.mcp_config);
  if (!config)
    return std::unexpected(std::move(config.error()));
  auto const server = std::ranges::find_if(config->servers, [&](ava::mcp::McpServerConfig const& item) { return item.id == entry.mcp_server_id; });
  if (server == config->servers.end() || !server->enabled)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "MCP server not found");
    error.with_context("mcp_server", entry.mcp_server_id);
    return std::unexpected(std::move(error));
  }

  auto arguments = mcp_prompt_arguments_json(entry, argument_text);
  if (!arguments)
    return std::unexpected(std::move(arguments.error()));

  auto context = make_tool_context(unlocked_session, request.permission_resolver);
  context.permission_tool_name = "mcp_prompt";
  context.current_tool_name = "mcp_prompt";
  context.cancel_requested = request.cancel_requested;
  if (auto permission = ensure_mcp_prompt_permissions(context, *server, entry); !permission)
    return std::unexpected(std::move(permission.error()));

  std::filesystem::path workspace_dir = runtime::session_ts::rat(unlocked_session)->workspace_dir();
  auto client = ava::mcp::McpStdioClient::start(*server, ava::mcp::McpStdioClientOptions{.workspace_dir = std::move(workspace_dir)}, request.cancel_requested);
  if (!client)
    return std::unexpected(std::move(client.error()));
  auto prompt = (*client)->get_prompt(entry.mcp_prompt_name, *arguments, request.cancel_requested);
  auto shutdown = (*client)->shutdown();
  if (!prompt)
    return std::unexpected(std::move(prompt.error()));
  if (!shutdown)
    return std::unexpected(std::move(shutdown.error()));
  if (prompt->content.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "MCP prompt returned no text");
    error.with_context("mcp_server", entry.mcp_server_id);
    error.with_context("mcp_prompt", entry.mcp_prompt_name);
    return std::unexpected(std::move(error));
  }
  return std::move(prompt->content);
}

void load_mcp_prompt_commands(RegistryBuilder& builder, runtime::session_ts& unlocked_session, CommandRegistryOptions const& options,
                              runtime::ExtensionResourcePolicy const& policy)
{
  auto config = ava::mcp::load_mcp_config(policy.mcp_config);
  if (!config)
  {
    add_diagnostic(builder, CommandRegistryDiagnostic{.source = to_string(UnifiedCommandSource::McpPrompt), .message = config.error().format()});
    return;
  }

  CRITICAL_AREA_BEGIN_R(session);
  auto const workspace_dir = session_r->workspace_dir();
  auto context =
      ava::tools::ToolContext{.workspace_dir = workspace_dir,
                              .mode = session_r->mode(),
                              .permission_resolver = options.permission_resolver,
                              .auto_allow_deny_preflight = ava::permissions::build_persistent_permission_deny_preflight(session_r->permission_rule_store()),
                              .permission_audit_sink = [&unlocked_session](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
                                auto entry = ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                        .parent_id = "",
                                                                        .type = ava::session::EntryType::PermissionDecision,
                                                                        .timestamp = ava::session::now_timestamp(),
                                                                        .data_json = ava::tools::permission_audit_data_json(event)};
                                return runtime::session_ts::wat(unlocked_session)->append_owned(std::move(entry));
                              },
                              .session_id = session_r->store.session_id(),
                              .provider_id = session_r->model().provider_id,
                              .model_id = session_r->model().model_id,
                              .current_dir = session_r->current_dir()};
  CRITICAL_AREA_END_R(session);

  context.permission_tool_name = "mcp_prompts";
  context.current_tool_name = "mcp_prompts";
  context.cancel_requested = options.cancel_requested;

  for (auto const& server : config->servers)
  {
    if (!server.enabled)
      continue;
    if (!runtime::valid_command_segment(server.id))
    {
      add_diagnostic(builder, CommandRegistryDiagnostic{.source = to_string(UnifiedCommandSource::McpPrompt),
                                                        .source_id = server.id,
                                                        .path = server.source_path,
                                                        .message = "MCP server id does not form a safe slash command"});
      continue;
    }
    if (auto permission = ensure_mcp_prompt_server_permission(context, server); !permission)
    {
      add_diagnostic(builder, CommandRegistryDiagnostic{.source = to_string(UnifiedCommandSource::McpPrompt),
                                                        .source_id = server.id,
                                                        .path = server.source_path,
                                                        .message = permission.error().format()});
      continue;
    }
    auto client = ava::mcp::McpStdioClient::start(server, ava::mcp::McpStdioClientOptions{.workspace_dir = workspace_dir}, options.cancel_requested);
    if (!client)
    {
      add_diagnostic(
          builder,
          CommandRegistryDiagnostic{
              .source = to_string(UnifiedCommandSource::McpPrompt), .source_id = server.id, .path = server.source_path, .message = client.error().format()});
      continue;
    }
    auto prompts = (*client)->list_prompts(options.cancel_requested);
    auto shutdown = (*client)->shutdown();
    if (!prompts)
    {
      add_diagnostic(
          builder,
          CommandRegistryDiagnostic{
              .source = to_string(UnifiedCommandSource::McpPrompt), .source_id = server.id, .path = server.source_path, .message = prompts.error().format()});
      continue;
    }
    if (!shutdown)
    {
      add_diagnostic(
          builder,
          CommandRegistryDiagnostic{
              .source = to_string(UnifiedCommandSource::McpPrompt), .source_id = server.id, .path = server.source_path, .message = shutdown.error().format()});
      continue;
    }
    for (auto const& prompt : *prompts)
    {
      if (!runtime::valid_command_segment(prompt.name))
      {
        add_diagnostic(builder, CommandRegistryDiagnostic{.source = to_string(UnifiedCommandSource::McpPrompt),
                                                          .source_id = server.id,
                                                          .path = server.source_path,
                                                          .message = "MCP prompt name does not form a safe slash command"});
        continue;
      }
      CommandRegistryEntry entry{.command = namespaced_command("mcp", server.id, prompt.name),
                                 .description = prompt.description.empty() ? "MCP prompt " + prompt.name : prompt.description,
                                 .category = "MCP",
                                 .source = UnifiedCommandSource::McpPrompt,
                                 .kind = UnifiedCommandKind::McpPrompt,
                                 .source_id = server.id,
                                 .source_path = server.source_path,
                                 .source_scope = std::string(ava::mcp::to_string(server.scope)),
                                 .mcp_server_id = server.id,
                                 .mcp_prompt_name = prompt.name,
                                 .mcp_arguments = prompt.arguments};
      std::vector<std::string> required;
      for (auto const& argument : prompt.arguments)
      {
        if (argument.required)
          required.push_back(argument.name);
      }
      if (!prompt.arguments.empty())
        entry.hint = "<" + joined_strings(required.empty() ? std::vector<std::string>{"args"} : required, "> <") + ">";
      add_entry(builder, entry);
      entry.command = "/" + prompt.name;
      add_entry(builder, std::move(entry));
    }
  }
}

}  // namespace ava::app
