#include "sys.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/app/acp/client_tools.h"
#include "ava/app/acp/session.h"
#include "ava/app/runtime_credentials.h"
#include "ava/app/session_title_coordinator.h"
#include "ava/app/subagent_delivery_manager.h"
#include "ava/config/session_title_config.h"
#include "ava/session/attachments.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/open_beneath.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <future>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace ava::app::acp {
namespace {

using Json = nlohmann::json;

ava::core::Error session_error(std::string message, std::string_view session_id = {})
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Session, std::move(message));
  if (!session_id.empty())
    error.with_context("session_id", std::string(session_id));
  return error;
}

JsonRpcError wire_error(ava::core::Error const& error, int code = -32603)
{
  return JsonRpcError{
      .code = code, .message = error.format(), .data_json = std::nullopt, .id = std::nullopt, .intent = EnvelopeIntent::Unknown, .suppress_response = false};
}

bool path_is_within(std::filesystem::path const& root, std::filesystem::path const& candidate)
{
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it)
    if (candidate_it == candidate.end() || *candidate_it != *root_it)
      return false;
  return true;
}

std::filesystem::path normalized_absolute(std::filesystem::path const& base, std::filesystem::path const& path)
{
  return (path.is_absolute() ? path : base / path).lexically_normal();
}

bool acp_path_scoped_operation(ava::permissions::Operation operation)
{
  using ava::permissions::Operation;
  return operation == Operation::ReadFile || operation == Operation::SearchFiles || operation == Operation::EditFile || operation == Operation::LspQuery;
}

bool acp_mcp_operation(ava::permissions::Operation operation)
{
  using ava::permissions::Operation;
  return operation == Operation::McpServerLaunch || operation == Operation::McpServerConnect || operation == Operation::McpToolCall ||
         operation == Operation::McpResourceRead;
}

std::vector<std::string> acp_builtin_tool_names(ClientCapabilities const& capabilities)
{
  std::vector<std::string> names{"read_file", "list_directory", "glob", "grep", "write_file"};
  if (capabilities.read_text_file == capabilities.write_text_file)
  {
    names.push_back("edit_file");
    names.push_back("apply_patch");
  }
  if (capabilities.terminal)
    names.push_back("bash");
  return names;
}

class ScopeExit
{
 public:
  explicit ScopeExit(std::function<void()> callback) : callback_(std::move(callback)) { }
  ~ScopeExit() { callback_(); }
  ScopeExit(ScopeExit const&) = delete;
  ScopeExit& operator=(ScopeExit const&) = delete;

 private:
  std::function<void()> callback_;
};

}  // namespace

void SessionUpdateGateway::bind(SessionUpdateSender sender)
{
  std::lock_guard lock(mutex_);
  sender_ = std::move(sender);
}

void SessionUpdateGateway::unbind()
{
  std::lock_guard lock(mutex_);
  sender_ = {};
}

ava::core::VoidResult SessionUpdateGateway::send(std::string_view session_id, std::string_view update_json) const
{
  SessionUpdateSender sender;
  {
    std::lock_guard lock(mutex_);
    sender = sender_;
  }
  if (!sender)
    return std::unexpected(protocol_error("ACP connection is closing"));
  return sender(session_id, update_json);
}

namespace {

ava::core::Result<std::filesystem::path> resolve_session_cwd_lexically(std::filesystem::path const& launch_root, std::string_view requested)
{
  if (requested.empty())
    return std::unexpected(protocol_error("cwd is required"));
  std::filesystem::path path(requested);
  if (!path.is_absolute())
    return std::unexpected(protocol_error("cwd must be an absolute path"));
  auto normalized = path.lexically_normal();
  if (normalized != path)
    return std::unexpected(protocol_error("cwd must not contain traversal or redundant path components"));

  // Containment is checked lexically against the launch root so a workspace
  // configured through a symlink matches the paths the client and model use.
  // AcpSessionRegistry::resolve_cwd performs the filesystem check against the
  // retained launch-root descriptor before any service operation can commit.
  if (!path_is_within(launch_root, path))
  {
    auto error = protocol_error("cwd is outside the launch-approved workspace root");
    error.with_context("cwd", path.string());
    error.with_context("workspace_root", launch_root.string());
    return std::unexpected(std::move(error));
  }
  return path;
}

}  // namespace

ava::core::Result<std::string_view> acp_stop_reason(ava::core::RuntimeTerminalOutcome outcome)
{
  switch (outcome)
  {
    case ava::core::RuntimeTerminalOutcome::Completed:
      return "end_turn";
    case ava::core::RuntimeTerminalOutcome::MaxTokens:
      return "max_tokens";
    case ava::core::RuntimeTerminalOutcome::MaxTurnRequests:
      return "max_turn_requests";
    case ava::core::RuntimeTerminalOutcome::Refusal:
      return "refusal";
    case ava::core::RuntimeTerminalOutcome::Cancelled:
      return "cancelled";
    case ava::core::RuntimeTerminalOutcome::Error:
      break;
  }
  auto error = protocol_error("runtime returned an abnormal terminal outcome");
  error.with_context("outcome", std::string(ava::core::to_string(outcome)));
  return std::unexpected(std::move(error));
}

AcpSessionHost::AcpSessionHost(runtime::session_ts&& unlocked_session, AcpSessionOptions options)
    : unlocked_session_(std::move(unlocked_session)), options_(std::move(options)), session_id_(session_r()->store.session_id())
{
  options_.run_options.exact_file_access.reset();
  options_.run_options.command_executor.reset();
  if (!options_.client_capabilities)
    options_.client_capabilities = std::make_shared<ClientCapabilities const>();
  if (options_.client_capabilities->read_text_file || options_.client_capabilities->write_text_file)
    options_.run_options.exact_file_access = make_client_exact_file_access(session_id_, options_.client_requests, options_.client_capabilities->read_text_file,
                                                                           options_.client_capabilities->write_text_file);
  if (options_.client_capabilities->terminal)
    options_.run_options.command_executor = make_client_command_executor(session_id_, options_.client_requests);
}

AcpSessionHost::~AcpSessionHost()
{
  cancel();
}

std::string const& AcpSessionHost::session_id() const noexcept
{
  return session_id_;
}

std::filesystem::path const& AcpSessionHost::current_dir(runtime::session_ts::crat const& session_r) const noexcept
{
  return session_r->current_dir();
}

bool AcpSessionHost::accepts_images() const noexcept
{
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session_);
  return std::ranges::find(session_r->model().input_modalities, "image") != session_r->model().input_modalities.end();
}

ava::core::VoidResult AcpSessionHost::send_text_update(std::string_view kind, std::string_view text, std::string_view message_id) const
{
  auto gateway = options_.updates.lock();
  if (!gateway)
    return std::unexpected(protocol_error("ACP update gateway is unavailable"));
  Json update{{"sessionUpdate", kind}, {"content", Json{{"type", "text"}, {"text", text}}}};
  if (!message_id.empty())
    update["messageId"] = message_id;
  return gateway->send(session_id_, update.dump(-1, ' ', false, Json::error_handler_t::strict));
}

ava::core::Result<std::uint64_t> AcpSessionHost::reserve_prompt()
{
  std::lock_guard lock(mutex_);
  if (closing_)
    return std::unexpected(session_error("session is closing", session_id_));
  if (active_prompt_)
    return std::unexpected(session_error("session already has an active prompt", session_id_));

  CRITICAL_AREA_BEGIN_R(session_);

  if (!session__r->run_controller())
    return std::unexpected(session_error("session run controller is unavailable", session_id_));
  ++next_prompt_reservation_;
  if (next_prompt_reservation_ == 0)
    ++next_prompt_reservation_;
  auto const request_id = ava::core::make_id("acp-run");
  auto admitted = session__r->run_controller()->admit(RunRequest{.request_id = request_id});
  if (!admitted)
    return std::unexpected(std::move(admitted.error()));
  active_prompt_ = true;
  active_prompt_reservation_ = next_prompt_reservation_;
  active_prompt_cancel_generation_ = cancel_generation_;
  active_run_guard_.emplace(std::move(*admitted));
  active_run_request_id_ = request_id;
  return active_prompt_reservation_;
}

void AcpSessionHost::rollback_prompt_reservation(std::uint64_t reservation) noexcept
{
  std::optional<ActiveRunGuard> abandoned_run;
  {
    std::lock_guard lock(mutex_);
    if (active_prompt_ && active_prompt_reservation_ == reservation)
    {
      abandoned_run = std::move(active_run_guard_);
      active_run_guard_.reset();
      active_run_request_id_.clear();
      active_prompt_ = false;
      active_prompt_reservation_ = 0;
      active_prompt_cancel_generation_ = 0;
      idle_.notify_all();
    }
  }
}

bool AcpSessionHost::prompt_cancel_pending(std::uint64_t reservation) const noexcept
{
  std::lock_guard lock(mutex_);
  return active_prompt_ && active_prompt_reservation_ == reservation && cancel_generation_ != active_prompt_cancel_generation_;
}

AcpSessionHost::PermissionGrantKey AcpSessionHost::permission_grant_key(ava::permissions::PermissionPrompt const& prompt) const
{
  bool const command_recipe = prompt.operation == ava::permissions::Operation::RunCommand && prompt.command_metadata &&
                              ava::permissions::command_permission_allows_reusable_grant(*prompt.command_metadata);
  return PermissionGrantKey{.operation = prompt.operation,
                            .mode = prompt.mode,
                            .workspace = normalized_absolute(options_.launch_root, prompt.workspace_dir).string(),
                            .target = prompt.target_path.empty() ? std::string{} : normalized_absolute(prompt.workspace_dir, prompt.target_path).string(),
                            .command = command_recipe ? std::string{} : prompt.command,
                            .command_recipe_key = command_recipe ? prompt.command_metadata->workspace_recipe_key : std::string{},
                            .tool_name = prompt.tool_name};
}

ava::permissions::PermissionResolver AcpSessionHost::permission_resolver(std::uint64_t reservation, std::stop_token stop_token)
{
  return [this, reservation, stop_token](ava::permissions::PermissionPrompt const& prompt) { return resolve_permission(prompt, reservation, stop_token); };
}

ava::core::Result<ava::permissions::PermissionResolutionDecision> AcpSessionHost::resolve_permission(ava::permissions::PermissionPrompt const& prompt,
                                                                                                     std::uint64_t reservation, std::stop_token stop_token)
{
  using ava::permissions::PermissionAction;
  using ava::permissions::PermissionResolution;
  using ava::permissions::PermissionResolutionDecision;

  if (stop_token.stop_requested() || prompt_cancel_pending(reservation))
  {
    PermissionResolutionDecision decision(PermissionResolution::Cancel, "ACP prompt was cancelled while awaiting permission");
    decision.resolution_source = "client_cancel";
    return decision;
  }

  if (acp_path_scoped_operation(prompt.operation))
  {
    auto const target = normalized_absolute(prompt.workspace_dir, prompt.target_path);
    if (prompt.target_path.empty() || !path_is_within(options_.launch_root, target))
    {
      PermissionResolutionDecision decision(PermissionResolution::Deny, "ACP client permission cannot expand the launch-approved workspace root");
      decision.resolution_source = "hard_scope";
      decision.authoritative = true;
      return decision;
    }
  }

  auto const rule_store = session_r()->permission_rule_store();
  auto persistent = ava::permissions::match_persistent_permission_rule(rule_store, prompt);
  if (!persistent)
  {
    PermissionResolutionDecision decision(PermissionResolution::Deny, persistent.error().format());
    decision.resolution_source = "persistent_rule_error";
    decision.authoritative = true;
    return decision;
  }
  if (*persistent)
  {
    // match_persistent_permission_rule is the shared authoritative matcher: it
    // already rejected non-authoritative Allow rules (unverified executors,
    // unavailable containment, critical commands, legacy schema, and
    // repository-controlled build/test text). The MCP/session restriction
    // below is the only ACP-local gate that remains.
    bool const session_mcp_allow_requires_exact_command =
        session_r()->mcp_config() && acp_mcp_operation(prompt.operation) && (*persistent)->action == PermissionAction::Allow;
    bool const exact_session_mcp_allow =
        !session_mcp_allow_requires_exact_command || (!(*persistent)->command.empty() && (*persistent)->command == prompt.command);
    if ((*persistent)->action == PermissionAction::Deny || exact_session_mcp_allow)
    {
      auto const resolution = (*persistent)->action == PermissionAction::Allow ? PermissionResolution::Allow : PermissionResolution::Deny;
      PermissionResolutionDecision decision(resolution, (*persistent)->reason);
      decision.resolution_source = "persistent_rule";
      decision.rule_id = (*persistent)->rule_id;
      decision.authoritative = true;
      return decision;
    }
  }

  // Session MCP configuration originates from the same untrusted ACP client.
  // Only an exact protected persistent operator rule may authorize it; client
  // permission responses and in-memory session grants must never do so.
  if (acp_mcp_operation(prompt.operation) && session_r()->mcp_config())
  {
    PermissionResolutionDecision decision(
        PermissionResolution::Deny, "persistent operator authorization is required: add an exact protected persistent Allow for this session MCP operation");
    decision.resolution_source = "session_config";
    decision.authoritative = true;
    return decision;
  }

  auto const key = permission_grant_key(prompt);
  {
    std::lock_guard lock(mutex_);
    auto const found = std::ranges::find_if(permission_grants_, [&](PermissionGrant const& grant) { return grant.key == key; });
    if (found != permission_grants_.end())
    {
      PermissionResolutionDecision decision(found->allow ? PermissionResolution::AllowSessionGrant : PermissionResolution::Deny,
                                            "exact ACP host-session permission grant");
      decision.resolution_source = "session_grant";
      return decision;
    }
  }

  auto gateway = options_.client_requests.lock();
  if (!gateway)
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "ACP permission request failed closed because the client request gateway is unavailable"));
  auto params = encode_permission_request_params(session_id_, prompt, options_.launch_root);
  if (!params)
    return std::unexpected(std::move(params.error()));
  auto pending = gateway->send("session/request_permission", std::move(*params), options_.permission_timeout + kSessionCloseGrace);
  if (!pending)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "ACP permission request failed closed before delivery");
    error.with_context("cause", pending.error().format());
    error.with_context("tool", prompt.tool_name);
    error.with_context("permission_request_id", prompt.permission_request_id);
    return std::unexpected(std::move(error));
  }

  auto const permission_deadline = std::chrono::steady_clock::now() + options_.permission_timeout;
  while (pending->completion.wait_for(kAcpPermissionPollInterval) != std::future_status::ready)
  {
    if ((stop_token.stop_requested() || prompt_cancel_pending(reservation)) && gateway->cancel(pending->id, "ACP prompt cancelled while awaiting permission"))
    {
      PermissionResolutionDecision decision(PermissionResolution::Cancel, "ACP prompt was cancelled while awaiting client permission");
      decision.resolution_source = "client_cancel";
      return decision;
    }
    if (std::chrono::steady_clock::now() >= permission_deadline && gateway->cancel(pending->id, "ACP client permission request exceeded its local deadline"))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "ACP permission request timed out and failed closed");
      error.with_context("tool", prompt.tool_name);
      error.with_context("permission_request_id", prompt.permission_request_id);
      return std::unexpected(std::move(error));
    }
  }
  auto response = pending->completion.get();
  if (!response)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "ACP permission request timed out or failed closed");
    error.with_context("cause", response.error().message);
    error.with_context("tool", prompt.tool_name);
    error.with_context("permission_request_id", prompt.permission_request_id);
    error.with_context("hint", "the ACP client must return a selected offered option or cancelled outcome before the finite deadline");
    return std::unexpected(std::move(error));
  }
  auto selected = decode_permission_response(*response);
  if (!selected)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "ACP client returned an invalid permission response; request failed closed");
    error.with_context("cause", selected.error().message);
    error.with_context("tool", prompt.tool_name);
    error.with_context("permission_request_id", prompt.permission_request_id);
    return std::unexpected(std::move(error));
  }

  if (*selected == AcpPermissionSelection::Cancelled)
  {
    cancel();
    PermissionResolutionDecision decision(PermissionResolution::Cancel, "ACP client cancelled the permission request");
    decision.resolution_source = "client_cancel";
    return decision;
  }
  if (!permission_request_offers_session_decisions(prompt) &&
      (*selected == AcpPermissionSelection::AllowAlways || *selected == AcpPermissionSelection::RejectAlways))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                  "ACP client selected a session-wide option that was not offered for this bounded permission display; request failed closed");
    error.with_context("tool", prompt.tool_name);
    error.with_context("permission_request_id", prompt.permission_request_id);
    return std::unexpected(std::move(error));
  }

  bool const allow = *selected == AcpPermissionSelection::AllowOnce || *selected == AcpPermissionSelection::AllowAlways;
  bool const always = *selected == AcpPermissionSelection::AllowAlways || *selected == AcpPermissionSelection::RejectAlways;
  if (always)
  {
    std::lock_guard lock(mutex_);
    if (stop_token.stop_requested() || !active_prompt_ || active_prompt_reservation_ != reservation || cancel_generation_ != active_prompt_cancel_generation_)
    {
      PermissionResolutionDecision decision(PermissionResolution::Cancel, "ACP prompt was cancelled before the permission grant could be applied");
      decision.resolution_source = "client_cancel";
      return decision;
    }
    auto found = std::ranges::find_if(permission_grants_, [&](PermissionGrant const& grant) { return grant.key == key; });
    if (found == permission_grants_.end())
    {
      if (permission_grants_.size() >= kMaxAcpSessionPermissionGrants)
        return std::unexpected(
            ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "ACP exact session permission grant limit reached; request failed closed"));
      permission_grants_.push_back(PermissionGrant{.key = key, .allow = allow});
    }
    else
      found->allow = allow;
  }

  PermissionResolutionDecision decision(
      always && allow ? PermissionResolution::AllowSessionGrant : (allow ? PermissionResolution::Allow : PermissionResolution::Deny),
      always ? "ACP client selected an exact host-session grant" : "ACP client selected a one-shot decision");
  decision.resolution_source = always ? "session_grant" : "client";
  return decision;
}

RequestResult AcpSessionHost::finish_prompt(std::uint64_t reservation, RequestResult result)
{
  bool canceled = false;
  std::optional<ActiveRunGuard> abandoned_run;
  {
    std::lock_guard lock(mutex_);
    if (active_prompt_ && active_prompt_reservation_ == reservation)
    {
      canceled = cancel_generation_ != active_prompt_cancel_generation_;
      abandoned_run = std::move(active_run_guard_);
      active_run_guard_.reset();
      active_run_request_id_.clear();
      active_prompt_ = false;
      active_prompt_reservation_ = 0;
      active_prompt_cancel_generation_ = 0;
      idle_.notify_all();
    }
  }
  if (canceled)
    return std::string(R"({"stopReason":"cancelled"})");
  return result;
}

RequestResult AcpSessionHost::prompt(AcpPromptContent content, std::stop_token stop_token, std::optional<std::uint64_t> reservation,
                                     std::function<bool()> request_terminal_commit)
{
  if (!content.images.empty() && !accepts_images())
  {
    if (reservation)
      rollback_prompt_reservation(*reservation);
    return std::unexpected(wire_error(session_error("session model does not support image prompt content", session_id_), -32602));
  }

  std::uint64_t prompt_cancel_generation = 0;
  std::uint64_t prompt_reservation = 0;
  if (!reservation)
  {
    auto reserved = reserve_prompt();
    if (!reserved)
      return std::unexpected(wire_error(reserved.error(), -32600));
    reservation = *reserved;
  }

  ActiveRunGuard run_guard;
  std::string request_id;
  {
    std::lock_guard lock(mutex_);
    if (!active_prompt_ || active_prompt_reservation_ != *reservation || !active_run_guard_)
      return std::unexpected(wire_error(session_error("prompt admission is no longer active", session_id_), -32600));
    prompt_reservation = *reservation;
    prompt_cancel_generation = active_prompt_cancel_generation_;
    run_guard = std::move(*active_run_guard_);
    active_run_guard_.reset();
    request_id = active_run_request_id_;
  }
  ScopeExit scope([this, prompt_reservation] { rollback_prompt_reservation(prompt_reservation); });
  auto finish = [this, prompt_reservation](RequestResult result) { return finish_prompt(prompt_reservation, std::move(result)); };
  auto complete_setup_failure = [&run_guard](ava::core::Error const& error) {
    return run_guard.complete(RunOutcome{.run_id = {}, .reason = StopReason::ProviderError, .error = error});
  };

  if (prompt_cancel_pending(prompt_reservation))
  {
    static_cast<void>(run_guard.complete(RunOutcome{.run_id = {}, .reason = StopReason::UserCanceled}));
    return finish(std::string(R"({"stopReason":"cancelled"})"));
  }

  auto factory = options_.provider_bundle_factory;
  if (!factory)
    factory = create_runtime_provider_run_bundle;
  auto provider_options = options_.run_options;
  provider_options.request_id = request_id;
  provider_options.exact_builtin_tool_names = acp_builtin_tool_names(*options_.client_capabilities);
  provider_options.isolate_ambient_extensions = true;
  provider_options.require_descriptor_secure_workspace = true;
  provider_options.announce_execution_after_permission = true;
  provider_options.redact_permission_audit_arguments = true;
  provider_options.require_explicit_file_permissions = true;
  auto bundle = factory(unlocked_session_, std::move(provider_options), "ACP prompt");
  if (!bundle)
  {
    auto error = bundle.error();
    static_cast<void>(complete_setup_failure(error));
    bool const authentication_required = error.message().find("requires auth for provider") != std::string::npos;
    int const code = authentication_required ? -32000 : -32603;
    return finish(std::unexpected(wire_error(error, code)));
  }
  if (!bundle->provider || !bundle->transport || !bundle->auth_transport)
  {
    auto error = session_error("provider run bundle is incomplete", session_id_);
    static_cast<void>(complete_setup_failure(error));
    return finish(std::unexpected(wire_error(error)));
  }

  std::vector<ava::session::ImageAttachmentRef> image_attachments;
  image_attachments.reserve(content.images.size());
  for (auto const& image : content.images)
  {
    auto imported = ava::session::import_image_attachment_bytes(session_r()->store, image.bytes, image.mime_type);
    if (!imported)
    {
      auto error = imported.error();
      static_cast<void>(complete_setup_failure(error));
      return finish(std::unexpected(wire_error(error, -32602)));
    }
    image_attachments.push_back(std::move(*imported));
  }

  auto run_options = std::move(bundle->options);
  run_options.request_id = request_id;
  run_options.exact_builtin_tool_names = acp_builtin_tool_names(*options_.client_capabilities);
  run_options.exact_file_access = options_.run_options.exact_file_access;
  run_options.command_executor = options_.run_options.command_executor;
  run_options.isolate_ambient_extensions = true;
  run_options.require_descriptor_secure_workspace = true;
  run_options.announce_execution_after_permission = true;
  run_options.redact_permission_audit_arguments = true;
  run_options.require_explicit_file_permissions = true;
  run_options.question_resolver = nullptr;
  run_options.permission_resolver = permission_resolver(prompt_reservation, stop_token);
  run_options.on_terminal_commit =
      request_terminal_commit ? std::function<ava::core::VoidResult()>([commit = std::move(request_terminal_commit)]() -> ava::core::VoidResult {
        if (commit())
          return {};
        auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled", ava::core::ErrorCode::Canceled);
        error.with_context("boundary", "before_terminal_commit");
        return std::unexpected(std::move(error));
      })
                              : std::function<ava::core::VoidResult()>{};
  run_options.image_attachments = std::move(image_attachments);
  run_options.expand_prompt_file_references = false;
  RuntimeSessionUpdateMapper mapper(RuntimeSessionUpdateMapperOptions{.workspace_root = options_.launch_root, .message_id = request_id});
  run_options.event_sink = [this, &mapper](ava::event::RuntimeEvent const& event) -> ava::core::VoidResult {
    auto updates = mapper.map_coalesced_and_encode(event);
    if (!updates)
      return std::unexpected(std::move(updates.error()));
    if (updates->empty())
      return {};
    auto gateway = options_.updates.lock();
    if (!gateway)
      return std::unexpected(protocol_error("ACP update gateway is unavailable"));
    for (auto const& update : *updates)
      if (auto sent = gateway->send(session_id_, update); !sent)
        return sent;
    return {};
  };
  auto caller_cancel = run_options.cancel_requested;
  run_options.cancel_requested = [this, prompt_cancel_generation, stop_token, caller_cancel] {
    bool canceled = false;
    {
      std::lock_guard lock(mutex_);
      canceled = cancel_generation_ != prompt_cancel_generation;
    }
    return canceled || stop_token.stop_requested() || (caller_cancel && caller_cancel());
  };

  auto result = run_admitted_prompt(unlocked_session_, content.text, *bundle->provider, *bundle->transport, run_options, std::move(run_guard));
  if (!result)
    return finish(std::unexpected(wire_error(result.error())));
  auto stop_reason = acp_stop_reason(result->outcome);
  if (!stop_reason)
    return finish(std::unexpected(wire_error(stop_reason.error())));
  return finish(std::string("{\"stopReason\":\"") + std::string(*stop_reason) + "\"}");
}

void AcpSessionHost::cancel() noexcept
{
  // Lock order is host -> controller. Runtime/controller completion and its
  // stop callbacks never acquire the host mutex; no host-owned I/O or wait is
  // performed while arbitration is locked.
  std::lock_guard lock(mutex_);
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session_);
  if (!active_prompt_ || !session_r->run_controller())
    return;
  auto accepted = session_r->run_controller()->request_stop(StopReason::UserCanceled);
  if (accepted && *accepted)
  {
    ++cancel_generation_;
    if (cancel_generation_ == 0)
      ++cancel_generation_;
  }
}

ava::core::VoidResult AcpSessionHost::close()
{
  {
    std::lock_guard lock(mutex_);
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session_);
    closing_ = true;
    permission_grants_.clear();
    if (active_prompt_ && session_r->run_controller())
    {
      auto accepted = session_r->run_controller()->request_stop(StopReason::UserCanceled);
      if (accepted && *accepted)
      {
        ++cancel_generation_;
        if (cancel_generation_ == 0)
          ++cancel_generation_;
      }
    }
  }
  std::unique_lock lock(mutex_);
  if (!idle_.wait_for(lock, options_.close_grace, [&] { return !active_prompt_; }))
    return std::unexpected(session_error("timed out waiting for active prompt to stop", session_id_));
  return {};
}

AcpSessionRegistry::AcpSessionRegistry(AcpSessionOptions options) : options_(std::move(options))
{
  auto service_anchor = options_.open_context.anchor_set;
  if (!service_anchor)
  {
    std::error_code directory_error;
    for (auto const& directory : {options_.paths.ava_config_dir, options_.paths.ava_state_dir})
    {
      std::filesystem::create_directories(directory, directory_error);
      if (!directory_error)
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, directory_error);
      if (directory_error)
      {
        coordinator_startup_error_ = ava::core::Error(ava::core::ErrorCategory::Io, "failed to prepare ACP service anchors");
        return;
      }
    }
    // AnchorSet preserves its first root as the immutable launch workspace
    // identity used by command containment and ACP tool negotiation.
    auto opened = ava::core::AnchorSet::open({options_.launch_root, options_.paths.ava_config_dir, options_.paths.ava_state_dir, options_.paths.sessions_dir});
    if (!opened)
    {
      coordinator_startup_error_ = std::move(opened.error());
      return;
    }
    service_anchor = std::move(*opened);
  }
  options_.open_context.anchor_set = service_anchor;
  if (options_.open_context.diagnostics)
  {
    if (auto bound = options_.open_context.diagnostics->bind_anchor_set(service_anchor); !bound)
    {
      coordinator_startup_error_ = std::move(bound.error());
      return;
    }
  }

  if (options_.open_context.subagent_delivery_manager)
  {
    options_.open_context.subagent_coordinator = options_.open_context.subagent_delivery_manager->coordinator();
  }
  else
  {
    auto coordinator = options_.open_context.subagent_coordinator
                           ? ava::core::Result<std::shared_ptr<ava::agent::SubagentCoordinator>>(options_.open_context.subagent_coordinator)
                           : ava::agent::SubagentCoordinator::create();
    if (!coordinator)
    {
      coordinator_startup_error_ = std::move(coordinator.error());
      return;
    }
    options_.open_context.subagent_coordinator = std::move(*coordinator);
    auto manager = SubagentDeliveryManager::create(
        {.coordinator = options_.open_context.subagent_coordinator, .provider_bundle_factory = options_.provider_bundle_factory});
    if (manager)
      options_.open_context.subagent_delivery_manager = std::move(*manager);
    else
    {
      coordinator_startup_error_ = std::move(manager.error());
      return;
    }
  }

  if (!options_.open_context.session_title_coordinator)
  {
    auto config = ava::config::load_session_title_config(options_.paths, *service_anchor);
    if (!config)
    {
      coordinator_startup_error_ = std::move(config.error());
      return;
    }
    auto titles = SessionTitleCoordinator::create({.config = std::move(*config)});
    if (!titles)
    {
      coordinator_startup_error_ = std::move(titles.error());
      return;
    }
    options_.open_context.session_title_coordinator = std::move(*titles);
  }
}

AcpSessionRegistry::~AcpSessionRegistry()
{
  shutdown();
}

ava::core::Result<std::filesystem::path> AcpSessionRegistry::resolve_cwd(std::string_view requested) const
{
  auto cwd = resolve_session_cwd_lexically(options_.launch_root, requested);
  if (!cwd)
    return std::unexpected(std::move(cwd.error()));
  if (coordinator_startup_error_ || !options_.open_context.anchor_set || options_.open_context.anchor_set->anchors().empty())
    return std::unexpected(coordinator_startup_error_.value_or(protocol_error("ACP launch workspace authority is unavailable")));

  auto const& launch_anchor = options_.open_context.anchor_set->anchors().front();
  if (launch_anchor.root != options_.launch_root.lexically_normal())
    return std::unexpected(protocol_error("ACP launch workspace authority does not match its logical root"));
  auto relative = cwd->lexically_relative(launch_anchor.root);
  if (relative == "." || relative.empty())
    relative = ".";
  int const fd = ava::core::open_beneath(launch_anchor.fd, relative, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
  {
    int const error_number = errno;
    auto error = protocol_error("cwd must be an existing non-symlink directory contained by the launch workspace");
    error.with_context("cwd", cwd->string());
    error.with_context("workspace_root", launch_anchor.root.string());
    error.with_context("cause", std::generic_category().message(error_number));
    return std::unexpected(std::move(error));
  }
  ::close(fd);
  return *cwd;
}

ava::core::VoidResult AcpSessionRegistry::reserve_insertion(std::optional<std::string_view> session_id)
{
  std::lock_guard lock(mutex_);
  if (closing_)
    return std::unexpected(session_error("ACP connection is closing"));
  if (hosts_.size() + pending_insertions_ >= kMaxConnectionSessions)
    return std::unexpected(session_error("ACP connection session limit reached"));
  if (session_id && hosts_.contains(std::string(*session_id)))
    return std::unexpected(session_error("session is already active on this connection", *session_id));
  ++pending_insertions_;
  return {};
}

void AcpSessionRegistry::release_insertion() noexcept
{
  std::lock_guard lock(mutex_);
  if (pending_insertions_ > 0)
    --pending_insertions_;
}

ava::core::Result<std::shared_ptr<AcpSessionHost>> AcpSessionRegistry::insert_reserved(runtime::session_ts&& unlocked_session)
{
  auto const session_id = runtime::session_ts::rat(unlocked_session)->store.session_id();
  auto host = std::make_shared<AcpSessionHost>(std::move(unlocked_session), options_);
  std::lock_guard lock(mutex_);
  if (pending_insertions_ == 0)
    return std::unexpected(session_error("ACP session insertion lost its capacity reservation", session_id));
  --pending_insertions_;
  if (closing_)
    return std::unexpected(session_error("ACP connection is closing"));
  if (hosts_.size() >= kMaxConnectionSessions)
    return std::unexpected(session_error("ACP connection session limit reached"));
  if (hosts_.contains(session_id))
    return std::unexpected(session_error("session is already active on this connection", session_id));
  hosts_.emplace(session_id, host);
  return host;
}

ava::core::Result<std::shared_ptr<AcpSessionHost>> AcpSessionRegistry::create(std::filesystem::path const& cwd,
                                                                              std::shared_ptr<ava::mcp::McpConfig const> mcp_config)
{
  if (coordinator_startup_error_)
    return std::unexpected(*coordinator_startup_error_);
  if (auto reserved = reserve_insertion(); !reserved)
    return std::unexpected(std::move(reserved.error()));
  bool release_reservation = true;
  ScopeExit release([this, &release_reservation] {
    if (release_reservation)
      release_insertion();
  });

  auto options = options_.open_context;
  options.paths = options_.paths;
  options.tool_visibility.mode = ava::agent::ToolVisibilityMode::Default;
  auto unlocked_session_result = ava::app::runtime::Session::create_at(std::move(options), options_.launch_root, cwd);
  if (!unlocked_session_result)
    return std::unexpected(std::move(unlocked_session_result.error()));
  {
    ava::app::runtime::session_ts::wat(*unlocked_session_result)->resources().mcp_config = std::move(mcp_config);
    auto inserted = insert_reserved(std::move(*unlocked_session_result));
    release_reservation = false;
    return inserted;
  }
}

ava::core::Result<std::shared_ptr<AcpSessionHost>> AcpSessionRegistry::load(std::string_view session_id, std::filesystem::path const& cwd,
                                                                            std::shared_ptr<ava::mcp::McpConfig const> mcp_config)
{
  if (coordinator_startup_error_)
    return std::unexpected(*coordinator_startup_error_);
  if (auto reserved = reserve_insertion(session_id); !reserved)
    return std::unexpected(std::move(reserved.error()));
  bool release_reservation = true;
  ScopeExit release([this, &release_reservation] {
    if (release_reservation)
      release_insertion();
  });

  auto options = options_.open_context;
  options.paths = options_.paths;
  options.tool_visibility.mode = ava::agent::ToolVisibilityMode::Default;
  options.exact_session_id = true;
  options.session_read_limits = kAcpSessionReadLimits;
  runtime::SessionLifecycleRequest request;
  request.requested_session_id = std::string(session_id);
  request.expected_original_cwd = cwd;
  auto unlocked_session_result = ava::app::runtime::Session::open_at(std::move(options), options_.launch_root, cwd, std::move(request));
  if (!unlocked_session_result)
    return std::unexpected(std::move(unlocked_session_result.error()));
  {
    ava::app::runtime::session_ts::wat(*unlocked_session_result)->resources().mcp_config = std::move(mcp_config);
    auto inserted = insert_reserved(std::move(*unlocked_session_result));
    release_reservation = false;
    return inserted;
  }
}

std::weak_ptr<AcpSessionHost> AcpSessionRegistry::find(std::string_view session_id) const
{
  std::lock_guard lock(mutex_);
  auto found = hosts_.find(std::string(session_id));
  return found == hosts_.end() ? std::weak_ptr<AcpSessionHost>{} : std::weak_ptr<AcpSessionHost>(found->second);
}

ava::core::VoidResult AcpSessionRegistry::close(std::string_view session_id)
{
  std::shared_ptr<AcpSessionHost> host;
  {
    std::lock_guard lock(mutex_);
    auto found = hosts_.find(std::string(session_id));
    if (found == hosts_.end())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "session is not active on this connection");
      error.with_context("session_id", std::string(session_id));
      return std::unexpected(std::move(error));
    }
    host = found->second;
  }
  auto closed = host->close();
  {
    std::lock_guard lock(mutex_);
    auto found = hosts_.find(std::string(session_id));
    if (found != hosts_.end() && found->second == host)
      hosts_.erase(found);
  }
  return closed;
}

ava::core::Result<std::string> AcpSessionRegistry::list_json(std::optional<std::filesystem::path> const& cwd, std::optional<std::string> const& cursor,
                                                             ava::session::SessionCancelCallback cancel_requested)
{
  std::string cursor_token;
  std::vector<ListRecord> page;
  bool has_more = false;
  auto const cwd_key = cwd ? std::optional<std::string>(cwd->string()) : std::nullopt;

  if (cursor)
  {
    if (!cursor->starts_with("ava-acp-list-v1-"))
      return std::unexpected(protocol_error("session/list cursor is invalid or uses an unsupported schema"));
    std::lock_guard lock(mutex_);
    auto found = list_snapshots_.find(*cursor);
    if (found == list_snapshots_.end() || found->second.cwd != cwd_key)
      return std::unexpected(protocol_error("session/list cursor is unknown, expired, or does not match cwd"));
    auto& snapshot = found->second;
    auto const end = std::min(snapshot.records.size(), snapshot.offset + kSessionListPageSize);
    page.assign(snapshot.records.begin() + static_cast<std::ptrdiff_t>(snapshot.offset), snapshot.records.begin() + static_cast<std::ptrdiff_t>(end));
    snapshot.offset = end;
    has_more = end < snapshot.records.size();
    if (has_more)
      cursor_token = *cursor;
    else
    {
      list_snapshot_bytes_ -= snapshot.retained_bytes;
      list_snapshots_.erase(found);
      std::erase(list_snapshot_order_, *cursor);
    }
  }
  else
  {
    ava::session::SessionListLimits limits;
    limits.per_session = kAcpSessionReadLimits;
    auto summaries = ava::session::SessionStore::list_sessions_bounded(options_.launch_root, options_.paths.sessions_dir, limits, cancel_requested);
    if (!summaries)
      return std::unexpected(std::move(summaries.error()));
    std::vector<ListRecord> records;
    records.reserve(summaries->size());
    for (auto const& summary : *summaries)
    {
      auto const reported_cwd = summary.original_cwd.empty() ? options_.launch_root : summary.original_cwd;
      if (cwd && reported_cwd != *cwd)
        continue;
      records.push_back(ListRecord{.session_id = summary.session_id, .cwd = reported_cwd.string(), .title = summary.title, .updated_at = summary.last_updated});
    }
    auto const end = std::min(records.size(), kSessionListPageSize);
    page.assign(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(end));
    has_more = end < records.size();
    if (has_more)
    {
      auto retained_bytes = sizeof(ListSnapshot) + records.capacity() * sizeof(ListRecord) + 256U;
      if (cwd_key)
        retained_bytes += cwd_key->capacity();
      for (auto const& record : records)
        retained_bytes += record.session_id.capacity() + record.cwd.capacity() + record.title.capacity() + record.updated_at.capacity();
      if (retained_bytes > kMaxSessionListSnapshotBytes)
        return std::unexpected(session_error("session/list cursor snapshot exceeds retained byte budget"));

      std::lock_guard lock(mutex_);
      while (!list_snapshot_order_.empty() &&
             (list_snapshots_.size() >= kMaxSessionListSnapshots || list_snapshot_bytes_ + retained_bytes > kMaxSessionListSnapshotBytes))
      {
        auto const evicted_token = std::move(list_snapshot_order_.front());
        list_snapshot_order_.pop_front();
        auto const evicted = list_snapshots_.find(evicted_token);
        if (evicted != list_snapshots_.end())
        {
          list_snapshot_bytes_ -= evicted->second.retained_bytes;
          list_snapshots_.erase(evicted);
        }
      }
      cursor_token = "ava-acp-list-v1-" + ava::core::make_id("cursor");
      list_snapshot_bytes_ += retained_bytes;
      list_snapshot_order_.push_back(cursor_token);
      list_snapshots_.emplace(cursor_token, ListSnapshot{.cwd = cwd_key, .records = std::move(records), .offset = end, .retained_bytes = retained_bytes});
    }
  }

  Json sessions = Json::array();
  for (auto const& record : page)
  {
    Json item{{"sessionId", record.session_id}, {"cwd", record.cwd}};
    if (!record.title.empty())
      item["title"] = record.title;
    if (!record.updated_at.empty())
      item["updatedAt"] = record.updated_at;
    sessions.push_back(std::move(item));
  }
  Json result{{"sessions", std::move(sessions)}};
  if (has_more)
    result["nextCursor"] = cursor_token;
  auto encoded = result.dump(-1, ' ', false, Json::error_handler_t::strict);
  if (encoded.size() > kMaxSessionListResultBytes || encoded.size() + 1024 > kMaxRecordBytes)
    return std::unexpected(session_error("session/list page exceeds bounded result size"));
  return encoded;
}

void AcpSessionRegistry::cancel(std::string_view session_id) noexcept
{
  if (auto host = find(session_id).lock())
    host->cancel();
}

void AcpSessionRegistry::shutdown() noexcept
{
  std::vector<std::shared_ptr<AcpSessionHost>> hosts;
  {
    std::lock_guard lock(mutex_);
    if (closing_ && hosts_.empty())
      return;
    closing_ = true;
    hosts.reserve(hosts_.size());
    for (auto& [_, host] : hosts_) hosts.push_back(std::move(host));
    hosts_.clear();
    list_snapshots_.clear();
    list_snapshot_order_.clear();
    list_snapshot_bytes_ = 0;
  }
  for (auto const& host : hosts) host->cancel();
  for (auto const& host : hosts) static_cast<void>(host->close());
  if (options_.open_context.session_title_coordinator)
    options_.open_context.session_title_coordinator->shutdown();
  if (options_.open_context.subagent_delivery_manager)
    options_.open_context.subagent_delivery_manager->shutdown();
  else if (options_.open_context.subagent_coordinator)
    options_.open_context.subagent_coordinator->shutdown();
}

std::filesystem::path const& AcpSessionRegistry::launch_root() const noexcept
{
  return options_.launch_root;
}

ava::config::XdgPaths const& AcpSessionRegistry::paths() const noexcept
{
  return options_.paths;
}

}  // namespace ava::app::acp
