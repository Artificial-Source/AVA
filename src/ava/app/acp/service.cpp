#include "sys.h"
#include "ava/app/acp/codec.h"
#include "ava/app/acp/service.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "debug.h"

namespace ava::app::acp {
namespace {

using Json = nlohmann::json;

std::string request_id_key(JsonRpcId const& id)
{
  if (std::holds_alternative<NullJsonRpcId>(id))
    return "n:";
  if (auto const* number = std::get_if<std::int64_t>(&id))
    return "i:" + std::to_string(*number);
  return "s:" + std::get<std::string>(id);
}

JsonRpcError rpc_error(int code, std::string message)
{
  return JsonRpcError{.code = code,
                      .message = std::move(message),
                      .data_json = std::nullopt,
                      .id = std::nullopt,
                      .intent = EnvelopeIntent::Unknown,
                      .suppress_response = false};
}

RequestResult service_error(int code, std::string message)
{
  return std::unexpected(rpc_error(code, std::move(message)));
}

RequestResult core_error(ava::core::Error const& error, int code = -32603)
{
  return service_error(code, error.format());
}

AcpSessionOptions session_options(AgentServiceOptions const& options, std::weak_ptr<SessionUpdateGateway> updates,
                                  std::weak_ptr<ClientRequestGateway> client_requests)
{
  auto open_context = options.open_context;
  open_context.paths = options.paths;
  return AcpSessionOptions{.launch_root = options.launch_root,
                           .paths = options.paths,
                           .open_context = std::move(open_context),
                           .run_options = options.run_options,
                           .provider_bundle_factory = options.provider_bundle_factory,
                           .client_capabilities = nullptr,
                           .updates = std::move(updates),
                           .client_requests = std::move(client_requests),
                           .permission_timeout = options.permission_timeout,
                           .close_grace = options.close_grace,
                           .max_session_list_snapshot_bytes = options.max_session_list_snapshot_bytes};
}

std::expected<Json, JsonRpcError> params_object(Request const& request, bool empty_allowed = false)
{
  if (!request.params_json)
  {
    if (empty_allowed)
      return Json::object();
    return std::unexpected(rpc_error(-32602, request.method + " requires params"));
  }
  auto params = Json::parse(*request.params_json, nullptr, false, true);
  if (empty_allowed && params.is_null())
    return Json::object();
  if (params.is_discarded() || !params.is_object())
    return std::unexpected(rpc_error(-32602, request.method + " params must be an object"));
  return params;
}

std::expected<std::string, JsonRpcError> required_string(Json const& params, std::string_view field, std::string_view method)
{
  auto found = params.find(field);
  if (found == params.end() || !found->is_string() || found->get_ref<std::string const&>().empty())
    return std::unexpected(rpc_error(-32602, std::string(method) + " requires non-empty " + std::string(field)));
  return found->get<std::string>();
}

constexpr std::size_t kMaxAcpMcpServers = 16;
constexpr std::size_t kMaxAcpMcpArgs = 64;
constexpr std::size_t kMaxAcpMcpEnv = 64;
constexpr std::size_t kMaxAcpMcpArgumentBytes = 4096;
constexpr std::size_t kMaxAcpMcpEnvValueBytes = 16 * 1024;

bool has_control_byte(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7f;
  });
}

bool valid_env_name(std::string_view name)
{
  if (name.empty() || name.size() > 128 || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_'))
    return false;
  return std::ranges::all_of(name.substr(1), [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '_';
  });
}

std::optional<JsonRpcError> reject_additional_directories(Json const& params)
{
  auto additional = params.find("additionalDirectories");
  if (additional == params.end() || !additional->is_array())
    return std::nullopt;
  if (std::ranges::any_of(*additional, [](Json const& item) { return item.is_string(); }))
    return rpc_error(-32602, "additionalDirectories are not advertised or supported");
  return std::nullopt;
}

bool valid_unsupported_mcp_transport(Json const& value)
{
  auto name = value.find("name");
  auto url = value.find("url");
  auto headers = value.find("headers");
  if (name == value.end() || !name->is_string() || url == value.end() || !url->is_string() || headers == value.end() || !headers->is_array())
    return false;
  return std::ranges::all_of(*headers, [](Json const& header) {
    if (!header.is_object())
      return false;
    auto name = header.find("name");
    auto value = header.find("value");
    return name != header.end() && name->is_string() && value != header.end() && value->is_string();
  });
}

std::optional<ava::mcp::McpServerConfig> decode_stdio_mcp_server(Json const& value)
{
  if (!value.is_object())
    return std::nullopt;
  auto name = value.find("name");
  auto command = value.find("command");
  auto args = value.find("args");
  auto env = value.find("env");
  if (name == value.end() || !name->is_string() || name->get_ref<std::string const&>().empty() || name->get_ref<std::string const&>().size() > 128 ||
      has_control_byte(name->get_ref<std::string const&>()) || command == value.end() || !command->is_string() ||
      command->get_ref<std::string const&>().empty() || command->get_ref<std::string const&>().size() > kMaxAcpMcpArgumentBytes ||
      has_control_byte(command->get_ref<std::string const&>()) || !std::filesystem::path(command->get_ref<std::string const&>()).is_absolute() ||
      args == value.end() || !args->is_array() || args->size() > kMaxAcpMcpArgs || env == value.end() || !env->is_array() || env->size() > kMaxAcpMcpEnv)
    return std::nullopt;

  std::vector<std::string> parsed_args;
  for (auto const& arg : *args)
  {
    if (!arg.is_string() || arg.get_ref<std::string const&>().size() > kMaxAcpMcpArgumentBytes || has_control_byte(arg.get_ref<std::string const&>()))
      return std::nullopt;
    parsed_args.push_back(arg.get<std::string>());
  }

  std::vector<std::pair<std::string, std::string>> parsed_env;
  std::set<std::string> env_names;
  for (auto const& variable : *env)
  {
    if (!variable.is_object())
      return std::nullopt;
    auto env_name = variable.find("name");
    auto env_value = variable.find("value");
    if (env_name == variable.end() || !env_name->is_string() || !valid_env_name(env_name->get_ref<std::string const&>()) || env_value == variable.end() ||
        !env_value->is_string() || env_value->get_ref<std::string const&>().size() > kMaxAcpMcpEnvValueBytes ||
        env_value->get_ref<std::string const&>().find('\0') != std::string::npos || !env_names.insert(env_name->get<std::string>()).second)
      return std::nullopt;
    parsed_env.emplace_back(env_name->get<std::string>(), env_value->get<std::string>());
  }

  return ava::mcp::McpServerConfig{.id = name->get<std::string>(),
                                   .name = name->get<std::string>(),
                                   .command = command->get<std::string>(),
                                   .args = std::move(parsed_args),
                                   .env = std::move(parsed_env),
                                   .enabled = true,
                                   .scope = ava::mcp::McpServerScope::Project,
                                   .source_path = {}};
}

std::expected<std::shared_ptr<ava::mcp::McpConfig const>, JsonRpcError> decode_mcp_servers(Json const& params, std::string_view method)
{
  auto field = params.find("mcpServers");
  if (field == params.end() || !field->is_array())
    return std::make_shared<ava::mcp::McpConfig const>();
  ava::mcp::McpConfig config;
  std::set<std::string> ids;
  for (auto const& value : *field)
  {
    if (!value.is_object())
      continue;
    auto type = value.find("type");
    if (type != value.end() && type->is_string() && (type->get_ref<std::string const&>() == "http" || type->get_ref<std::string const&>() == "sse"))
    {
      if (valid_unsupported_mcp_transport(value))
        return std::unexpected(rpc_error(-32602, "only the implicit ACP stdio MCP transport is supported"));
      continue;
    }

    auto server = decode_stdio_mcp_server(value);
    if (!server)
      continue;
    if (type != value.end())
      return std::unexpected(rpc_error(-32602, "only the implicit ACP stdio MCP transport is supported"));
    if (config.servers.size() >= kMaxAcpMcpServers)
      return std::unexpected(rpc_error(-32602, std::string(method) + " mcpServers exceeds the supported server-count bound"));
    if (!ids.insert(server->id).second)
      return std::unexpected(rpc_error(-32602, "duplicate stdio MCP server name"));
    config.servers.push_back(std::move(*server));
  }
  return std::make_shared<ava::mcp::McpConfig const>(std::move(config));
}

PromptContentDecodeResult prompt_content(Json const& params)
{
  auto prompt = params.find("prompt");
  if (prompt == params.end())
    return std::unexpected(rpc_error(-32602, "session/prompt requires prompt"));
  return decode_prompt_content(prompt->dump(-1, ' ', false, Json::error_handler_t::strict));
}

bool model_accepts_images(ava::config::ModelInfo const& model)
{
  return std::ranges::find(model.input_modalities, "image") != model.input_modalities.end();
}

ava::core::VoidResult validate_pinned_provider(AgentServiceOptions const& options, ava::config::ModelInfo const& model)
{
  auto catalog = options.open_context.provider_catalog ? options.open_context.provider_catalog : ava::provider::ProviderCatalog::build_builtins_only();
  if (options.provider_bundle_factory || catalog->contains(model.provider_id))
    return {};
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "ACP startup provider is not registered");
  error.with_context("provider", model.provider_id);
  error.with_context("model", model.model_id);
  error.with_context("hint", "configure a built-in provider or supply an explicit provider bundle before starting ava --acp");
  return std::unexpected(std::move(error));
}

AgentServiceOptions default_options(std::string agent_version)
{
  auto root = ava::core::launch_workspace_root();
  AgentServiceOptions options;
  options.agent_version = std::move(agent_version);
  // If we can't use PWD then `launch_root` becomes the cannonical path returned by std::filesystem::current_path(), and later we'll
  // try to compare that against logical paths... so, basically everything will break if this path contains symbolic links.
  Dout(dc::warning(!root), "Failed to resolve a logical workspace launch root from PWD. Symbolic links that are part of the launch directory will not work.");
  options.launch_root = root ? *root : std::filesystem::current_path();
  return options;
}

}  // namespace

ava::core::Result<AgentServiceOptions> pin_agent_service_model(AgentServiceOptions options)
{
  if (options.open_context.default_model_override)
  {
    auto const& model = *options.open_context.default_model_override;
    if (model.provider_id.empty() || model.model_id.empty())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "ACP startup model override is incomplete");
      error.with_context("hint", "configure an exact non-empty provider and model before starting ava --acp");
      return std::unexpected(std::move(error));
    }
    if (auto available = validate_pinned_provider(options, model); !available)
      return std::unexpected(std::move(available.error()));
    options.open_context.pin_model_override = true;
    return options;
  }

  auto registry = ava::config::load_model_registry(options.paths);
  if (!registry)
  {
    auto error = registry.error();
    error.with_context("startup", "ACP effective model resolution");
    error.with_context("hint", "fix the model configuration and restart ava --acp");
    return std::unexpected(std::move(error));
  }
  auto model = ava::config::find_model(*registry, registry->default_provider_id, registry->default_model_id);
  if (!model)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "ACP startup default model cannot be resolved exactly");
    error.with_context("provider", registry->default_provider_id);
    error.with_context("model", registry->default_model_id);
    error.with_context("config", options.paths.models_file.string());
    error.with_context("hint", "declare the configured default provider/model in the model registry and restart ava --acp");
    return std::unexpected(std::move(error));
  }
  if (auto available = validate_pinned_provider(options, *model); !available)
    return std::unexpected(std::move(available.error()));
  options.open_context.default_model_override = std::move(*model);
  options.open_context.pin_model_override = true;
  return options;
}

struct AgentService::PinnedOptions
{
  AgentServiceOptions options;
  std::optional<ava::core::Error> error = std::nullopt;
};

AgentService::PinnedOptions AgentService::pin_options(AgentServiceOptions options)
{
  auto pinned = pin_agent_service_model(options);
  if (!pinned)
    return PinnedOptions{.options = std::move(options), .error = std::move(pinned.error())};
  return PinnedOptions{.options = std::move(*pinned), .error = std::nullopt};
}

AgentService::AgentService(std::string agent_version) : AgentService(default_options(std::move(agent_version)))
{
}

AgentService::AgentService(AgentServiceOptions options) : AgentService(pin_options(std::move(options)))
{
}

AgentService::AgentService(PinnedOptions pinned)
    : agent_version_(pinned.options.agent_version),
      image_prompt_capability_(!pinned.error && pinned.options.open_context.default_model_override &&
                               model_accepts_images(*pinned.options.open_context.default_model_override)),
      startup_error_(std::move(pinned.error)),
      updates_(std::make_shared<SessionUpdateGateway>()),
      client_requests_(std::make_shared<ClientRequestGateway>()),
      session_options_(session_options(pinned.options, updates_, client_requests_))
{
}

AgentService::~AgentService()
{
  shutdown();
}

void AgentService::bind_update_sender(SessionUpdateSender sender)
{
  updates_->bind(std::move(sender));
}

void AgentService::unbind_update_sender()
{
  updates_->unbind();
}

void AgentService::bind_client_request_sender(ClientRequestSender sender, ClientRequestCanceler canceler, ClientConnectionAborter aborter)
{
  client_requests_->bind(std::move(sender), std::move(canceler), std::move(aborter));
}

void AgentService::unbind_client_request_sender()
{
  client_requests_->unbind();
}

void AgentService::bind_request_terminal_committer(RequestTerminalCommitter committer)
{
  std::lock_guard lock(mutex_);
  request_terminal_committer_ = std::move(committer);
}

void AgentService::unbind_request_terminal_committer()
{
  std::lock_guard lock(mutex_);
  request_terminal_committer_ = {};
}

std::expected<std::function<void()>, JsonRpcError> AgentService::pre_admit_request(Request const& request)
{
  if (request.method != "session/prompt")
    return std::function<void()>{};
  {
    std::lock_guard lock(mutex_);
    if (!initialized_)
      return std::function<void()>{};
  }
  auto params = params_object(request);
  if (!params)
    return std::function<void()>{};
  auto session_id = required_string(*params, "sessionId", request.method);
  auto content = prompt_content(*params);
  if (!session_id || !content)
    return std::function<void()>{};
  auto host = registry_->find(*session_id).lock();
  if (!host)
    return std::function<void()>{};
  auto reserved = host->reserve_prompt();
  if (!reserved)
    return std::unexpected(rpc_error(-32600, reserved.error().format()));

  auto admission = std::make_shared<PromptAdmission>(PromptAdmission{.host = std::move(host), .reservation = *reserved});
  auto const key = request_id_key(request.id);
  {
    std::lock_guard lock(admissions_mutex_);
    if (prompt_admissions_.contains(key))
    {
      admission->host->rollback_prompt_reservation(admission->reservation);
      return std::unexpected(rpc_error(-32600, "duplicate prompt admission id"));
    }
    prompt_admissions_.emplace(key, admission);
  }
  return [this, key, admission] { rollback_prompt_admission(key, admission); };
}

std::shared_ptr<AgentService::PromptAdmission> AgentService::take_prompt_admission(JsonRpcId const& id)
{
  std::lock_guard lock(admissions_mutex_);
  auto const found = prompt_admissions_.find(request_id_key(id));
  if (found == prompt_admissions_.end())
    return {};
  auto admission = std::move(found->second);
  prompt_admissions_.erase(found);
  return admission;
}

void AgentService::rollback_prompt_admission(std::string const& key, std::shared_ptr<PromptAdmission> const& admission) noexcept
{
  bool owned = false;
  {
    std::lock_guard lock(admissions_mutex_);
    auto const found = prompt_admissions_.find(key);
    if (found != prompt_admissions_.end() && found->second == admission)
    {
      prompt_admissions_.erase(found);
      owned = true;
    }
  }
  if (owned)
    admission->host->rollback_prompt_reservation(admission->reservation);
}

std::expected<void, JsonRpcError> AgentService::commit_request_terminal(JsonRpcId const& id)
{
  RequestTerminalCommitter committer;
  {
    std::lock_guard lock(mutex_);
    committer = request_terminal_committer_;
  }
  if (committer && !committer(id))
    return std::unexpected(rpc_error(-32800, "Request cancelled"));
  return {};
}

RequestResult AgentService::handle_request(Request const& request, std::stop_token stop_token)
{
  if (stop_token.stop_requested())
    return service_error(-32800, "Request cancelled");

  if (request.method == "initialize")
  {
    auto initialize = decode_initialize_params(request);
    if (!initialize)
      return std::unexpected(std::move(initialize.error()));
    {
      std::lock_guard lock(mutex_);
      if (initialized_ || initializing_)
        return service_error(-32600, "initialize may only be called once");
      if (startup_error_)
        return core_error(*startup_error_);
      initializing_ = true;
    }
    auto abandon_initialize = [this] {
      std::lock_guard lock(mutex_);
      initializing_ = false;
    };
    auto result = initialize_result_json(agent_version_, image_prompt_capability_);
    if (!result)
    {
      abandon_initialize();
      return core_error(result.error());
    }
    // One immutable capability object is created before initialized state is
    // published. Every registry/host copy shares this connection snapshot.
    auto immutable_capabilities = std::make_shared<ClientCapabilities const>(std::move(initialize->client_capabilities));
    auto initialized_options = session_options_;
    initialized_options.client_capabilities = std::move(immutable_capabilities);
    auto registry = std::make_unique<AcpSessionRegistry>(std::move(initialized_options));
    auto committed = commit_request_terminal(request.id);
    if (!committed)
    {
      abandon_initialize();
      return std::unexpected(std::move(committed.error()));
    }
    {
      std::lock_guard lock(mutex_);
      registry_ = std::move(registry);
      initialized_ = true;
      initializing_ = false;
    }
    return std::move(*result);
  }

  {
    std::lock_guard lock(mutex_);
    if (!initialized_)
      return service_error(-32600, "connection must be initialized first");
  }

  if (request.method == "session/new")
  {
    auto params = params_object(request);
    if (!params)
      return std::unexpected(std::move(params.error()));
    if (auto rejected = reject_additional_directories(*params))
      return std::unexpected(std::move(*rejected));
    auto mcp_config = decode_mcp_servers(*params, request.method);
    if (!mcp_config)
      return std::unexpected(std::move(mcp_config.error()));
    auto cwd_text = required_string(*params, "cwd", request.method);
    if (!cwd_text)
      return std::unexpected(std::move(cwd_text.error()));
    auto cwd = registry_->resolve_cwd(*cwd_text);
    if (!cwd)
      return core_error(cwd.error(), -32602);
    auto committed = commit_request_terminal(request.id);
    if (!committed)
      return std::unexpected(std::move(committed.error()));
    auto host = registry_->create(*cwd, std::move(*mcp_config));
    if (!host)
      return core_error(host.error());
    return Json{{"sessionId", (*host)->session_id()}}.dump();
  }

  if (request.method == "session/prompt")
  {
    auto admission = take_prompt_admission(request.id);
    auto params = params_object(request);
    if (!params)
      return std::unexpected(std::move(params.error()));
    auto session_id = required_string(*params, "sessionId", request.method);
    if (!session_id)
      return std::unexpected(std::move(session_id.error()));
    auto content = prompt_content(*params);
    if (!content)
      return std::unexpected(std::move(content.error()));
    auto host = registry_->find(*session_id).lock();
    if (!host || (admission && admission->host != host))
    {
      if (admission)
        admission->host->rollback_prompt_reservation(admission->reservation);
      return service_error(-32002, "session is not active on this connection: " + *session_id);
    }
    if (!content->images.empty() && !host->accepts_images())
    {
      if (admission)
        admission->host->rollback_prompt_reservation(admission->reservation);
      return service_error(-32602, "session/prompt image content is not supported by the session model");
    }
    RequestTerminalCommitter committer;
    {
      std::lock_guard lock(mutex_);
      committer = request_terminal_committer_;
    }
    auto request_terminal_commit =
        committer ? std::function<bool()>([committer = std::move(committer), id = request.id] { return committer(id); }) : std::function<bool()>{};
    return host->prompt(std::move(*content), stop_token, admission ? std::optional<std::uint64_t>(admission->reservation) : std::nullopt,
                        std::move(request_terminal_commit));
  }

  if (request.method == "session/load")
    return service_error(-32601, "session/load is not supported because exact rich-history replay is deferred");

  if (request.method == "session/resume")
  {
    auto params = params_object(request);
    if (!params)
      return std::unexpected(std::move(params.error()));
    if (auto rejected = reject_additional_directories(*params))
      return std::unexpected(std::move(*rejected));
    auto mcp_config = decode_mcp_servers(*params, request.method);
    if (!mcp_config)
      return std::unexpected(std::move(mcp_config.error()));
    auto session_id = required_string(*params, "sessionId", request.method);
    auto cwd_text = required_string(*params, "cwd", request.method);
    if (!session_id)
      return std::unexpected(std::move(session_id.error()));
    if (!cwd_text)
      return std::unexpected(std::move(cwd_text.error()));
    auto cwd = registry_->resolve_cwd(*cwd_text);
    if (!cwd)
      return core_error(cwd.error(), -32602);
    auto committed = commit_request_terminal(request.id);
    if (!committed)
      return std::unexpected(std::move(committed.error()));
    auto host = registry_->load(*session_id, *cwd, std::move(*mcp_config));
    if (!host)
      return core_error(host.error(), host.error().category() == ava::core::ErrorCategory::NotFound          ? -32002
                                      : host.error().category() == ava::core::ErrorCategory::InvalidArgument ? -32602
                                                                                                             : -32603);
    return std::string("{}");
  }

  if (request.method == "session/list")
  {
    auto params = params_object(request, true);
    if (!params)
      return std::unexpected(std::move(params.error()));
    std::optional<std::filesystem::path> cwd;
    if (auto field = params->find("cwd"); field != params->end() && !field->is_null())
    {
      if (!field->is_string())
        return service_error(-32602, "session/list cwd must be a string or null");
      auto resolved_cwd = registry_->resolve_cwd(field->get_ref<std::string const&>());
      if (!resolved_cwd)
        return core_error(resolved_cwd.error(), -32602);
      cwd = std::move(*resolved_cwd);
    }
    std::optional<std::string> cursor;
    if (auto field = params->find("cursor"); field != params->end() && !field->is_null())
    {
      if (!field->is_string() || field->get_ref<std::string const&>().empty() || field->get_ref<std::string const&>().size() > kMaxIdStringBytes)
        return service_error(-32602, "session/list cursor must be a bounded non-empty string or null");
      cursor = field->get<std::string>();
    }
    auto listed = registry_->list_json(cwd, cursor, [stop_token] { return stop_token.stop_requested(); });
    if (!listed)
      return core_error(listed.error(), listed.error().message().find("canceled") != std::string::npos           ? -32800
                                        : listed.error().category() == ava::core::ErrorCategory::InvalidArgument ? -32602
                                                                                                                 : -32603);
    return std::move(*listed);
  }

  if (request.method == "session/close")
  {
    auto params = params_object(request);
    if (!params)
      return std::unexpected(std::move(params.error()));
    auto session_id = required_string(*params, "sessionId", request.method);
    if (!session_id)
      return std::unexpected(std::move(session_id.error()));
    auto committed = commit_request_terminal(request.id);
    if (!committed)
      return std::unexpected(std::move(committed.error()));
    auto closed = registry_->close(*session_id);
    if (!closed)
      return core_error(closed.error(), closed.error().category() == ava::core::ErrorCategory::NotFound ? -32002 : -32603);
    return std::string("{}");
  }

  return service_error(-32601, "Method not found");
}

void AgentService::handle_notification(Notification const& notification, std::stop_token stop_token)
{
  if (stop_token.stop_requested())
    return;
  handle_control_notification(notification);
}

void AgentService::handle_control_notification(Notification const& notification)
{
  if (notification.method != "session/cancel")
    return;
  {
    std::lock_guard lock(mutex_);
    if (!initialized_)
      return;
  }
  if (!notification.params_json)
    return;
  auto params = Json::parse(*notification.params_json, nullptr, false, true);
  if (!params.is_object())
    return;
  auto session_id = params.find("sessionId");
  if (session_id == params.end() || !session_id->is_string())
    return;
  registry_->cancel(session_id->get_ref<std::string const&>());
}

bool AgentService::initialized() const noexcept
{
  std::lock_guard lock(mutex_);
  return initialized_;
}

void AgentService::shutdown() noexcept
{
  std::vector<std::shared_ptr<PromptAdmission>> admissions;
  {
    std::lock_guard lock(admissions_mutex_);
    admissions.reserve(prompt_admissions_.size());
    for (auto& [key, admission] : prompt_admissions_)
    {
      static_cast<void>(key);
      admissions.push_back(std::move(admission));
    }
    prompt_admissions_.clear();
  }
  for (auto const& admission : admissions) admission->host->rollback_prompt_reservation(admission->reservation);
  if (registry_)
    registry_->shutdown();
  unbind_request_terminal_committer();
  client_requests_->unbind();
  updates_->unbind();
}

}  // namespace ava::app::acp
