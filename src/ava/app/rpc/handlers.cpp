#include "sys.h"
#include "protocol.h"
#include "runtime_navigation.h"
#include "serialization.h"
#include "session_operators.h"
#include "ava/http/transport.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_catalog.h"
#include "ava/app/runtime_credentials.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace ava::app::rpc {

ava::core::Result<runtime::RunOptions> ensure_prompt_runtime_options(ava::config::XdgPaths const& paths, std::string_view provider_id,
                                                                     runtime::RunOptions options, ava::http::Transport& auth_transport,
                                                                     std::string_view purpose, std::shared_ptr<ava::provider::ProviderCatalog const> catalog)
{
  return prepare_runtime_credentials(paths, provider_id, std::move(options), auth_transport, std::string("RPC ") + std::string(purpose), std::move(catalog));
}

ava::core::Result<ava::config::ModelInfo> resolve_requested_model(runtime::session_ts::rat const& session_r, RpcCommand const& command)
{
  if (!command.model || command.model->empty())
    return std::unexpected(invalid_rpc("set_model requires model"));
  if (command.provider && !command.provider->empty())
  {
    return resolve_runtime_model(session_r->paths(), session_r->provider_catalog(), *command.provider, *command.model);
  }

  return select_runtime_model(session_r, std::nullopt, *command.model);
}

ava::core::Result<ava::config::ModelInfo> next_runtime_model(ava::app::runtime::session_ts::rat const& session_r)
{
  return cycle_runtime_model(session_r, 1);
}

ava::core::Result<ava::config::ModelInfo> previous_runtime_model(ava::app::runtime::session_ts::rat const& session_r)
{
  return cycle_runtime_model(session_r, -1);
}

ava::core::Result<ProviderHandle> provider_for_session_model(runtime::session_ts const& unlocked_session, std::string_view injected_provider_id,
                                                             ava::provider::Provider const& injected_provider)
{
  auto const provider_id = runtime::session_ts::crat(unlocked_session)->model_selection_r()->model.provider_id;
  if (provider_id == injected_provider_id)
  {
    return ProviderHandle{.provider = &injected_provider, .owned = nullptr};
  }
  auto provider = create_runtime_provider(unlocked_session, provider_id);
  if (!provider)
    return std::unexpected(std::move(provider.error()));
  return ProviderHandle{.provider = nullptr, .owned = std::move(*provider)};
}

bool is_plugin_rpc_command(std::string_view type)
{
  return type == "list_plugins" || type == "plugin_failures" || type == "inspect_plugin" || type == "install_plugin" || type == "remove_plugin" ||
         type == "enable_plugin" || type == "disable_plugin" || type == "validate_plugin" || type == "list_plugin_prompts" || type == "get_plugin_prompt" ||
         type == "list_plugin_skills" || type == "get_plugin_skill" || type == "run_plugin_command";
}

bool is_mcp_rpc_command(std::string_view type)
{
  return type == "list_mcp_servers" || type == "inspect_mcp_server" || type == "list_mcp_tools" || type == "restart_mcp_server";
}

ava::core::Result<std::string> plugin_rpc_slash_command(RpcCommand const& command)
{
  if (command.type == "list_plugins")
    return std::string("/plugins list");
  if (command.type == "plugin_failures")
    return std::string("/plugins failures");
  if (command.type == "validate_plugin")
  {
    if (!command.path || command.path->empty())
      return std::unexpected(invalid_rpc("validate_plugin requires path"));
    return "/plugins validate " + *command.path;
  }
  if (command.type == "install_plugin")
  {
    if (!command.path || command.path->empty())
      return std::unexpected(invalid_rpc("install_plugin requires path"));
    return "/plugins install " + *command.path;
  }

  if (!command.plugin_id || command.plugin_id->empty())
  {
    return std::unexpected(invalid_rpc(command.type + " requires plugin_id"));
  }
  if (command.type == "inspect_plugin")
    return "/plugins inspect " + *command.plugin_id;
  if (command.type == "enable_plugin")
    return "/plugins enable " + *command.plugin_id;
  if (command.type == "disable_plugin")
    return "/plugins disable " + *command.plugin_id;
  if (command.type == "remove_plugin")
    return "/plugins remove " + *command.plugin_id;
  if (command.type == "list_plugin_prompts")
    return "/plugins prompts " + *command.plugin_id;
  if (command.type == "list_plugin_skills")
    return "/plugins skills " + *command.plugin_id;
  if (command.type == "get_plugin_prompt" || command.type == "get_plugin_skill" || command.type == "run_plugin_command")
  {
    if (!command.name || command.name->empty())
      return std::unexpected(invalid_rpc(command.type + " requires name"));
  }
  if (command.type == "get_plugin_prompt")
    return "/plugins prompt " + *command.plugin_id + " " + *command.name;
  if (command.type == "get_plugin_skill")
    return "/plugins skill " + *command.plugin_id + " " + *command.name;
  if (command.type == "run_plugin_command")
  {
    return "/plugin run " + *command.plugin_id + " " + *command.name + " " + command.arguments.value_or("{}");
  }
  return std::unexpected(invalid_rpc("unsupported plugin RPC command"));
}

ava::core::Result<std::string> mcp_rpc_slash_command(RpcCommand const& command)
{
  if (command.type == "list_mcp_servers")
    return std::string("/mcp list");
  if (!command.server_id || command.server_id->empty())
  {
    return std::unexpected(invalid_rpc(command.type + " requires server_id"));
  }
  if (command.type == "inspect_mcp_server")
    return "/mcp inspect " + *command.server_id;
  if (command.type == "list_mcp_tools")
    return "/mcp tools " + *command.server_id;
  if (command.type == "restart_mcp_server")
    return "/mcp restart " + *command.server_id;
  return std::unexpected(invalid_rpc("unsupported MCP RPC command"));
}

}  // namespace ava::app::rpc
