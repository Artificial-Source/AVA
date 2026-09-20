#include "sys.h"
#ifdef CWDEBUG
#include "ava/debug/debug_ostream_operators.h"
#endif
#include "plugin_event_hooks.h"
#include "runtime/ExtensionResourcePolicy.h"
#include "runtime/Session.h"
#include "runtime_compaction.h"
#include "runtime_event_adapters.h"
#include "runtime_prompt.h"
#include "runtime_prompt_file_references.h"
#include "runtime_reasoning.h"
#include "runtime_retry.h"
#include "runtime_run_outcomes.h"
#include "session_title_coordinator.h"
#include "subagent_delivery_manager.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/event/events.h"
#include "ava/http/curl_transport.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/subagent_config.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission_rules.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"
#include "ava/lsp/configured_provider.h"
#include "ava/core/error.h"
#include "ava/core/ids.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef CWDEBUG
#include <utils/at_scope_end.h>
#endif

namespace ava::app {

ava::core::Result<runtime::PromptState> select_runtime_prompt_state(runtime::session_ts const& unlocked_session, ava::agent::Mode mode)
{
  ava::config::XdgPaths paths;
  ava::config::ModelInfo model;
  std::filesystem::path workspace_dir;
  std::filesystem::path current_dir;
  bool include_project_resources;
  runtime::PromptOverrides prompt_overrides;
  std::optional<ava::agent::SubagentDefinition> selected_primary_agent;
  {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    paths = session_r->paths();
    model = session_r->model();
    workspace_dir = session_r->workspace_dir();
    current_dir = session_r->current_dir();
    include_project_resources = project_resources_trusted(session_r->project_trust());
    prompt_overrides = session_r->prompt_overrides();
    selected_primary_agent = session_r->selected_primary_agent();
  }
  return runtime::load_runtime_prompt_state(paths, model, mode, workspace_dir, current_dir, include_project_resources, prompt_overrides,
                                            selected_primary_agent);
}

ava::core::Error offline_provider_error(std::string_view action)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "offline mode is enabled; provider model calls are disabled");
  if (!action.empty())
    error.with_context("action", std::string(action));
  error.with_context("hint", "rerun without --offline to send prompts to the provider");
  return error;
}

ava::core::Result<ava::agent::AgentLoopResult> run_prompt(runtime::session_ts& unlocked_session, std::string const& user_message,
                                                          ava::provider::Provider const& provider, ava::http::Transport& transport,
                                                          runtime::RunOptions const& options)
{
  DoutEntering(dc::runtime, "run_prompt(prompt_bytes=" << user_message.size() << ")");
#ifdef CWDEBUG
  auto&& f = at_scope_end([] { Dout(dc::runtime, "Leaving run_prompt()"); });
#endif
  AVA_ASSERT_SESSION_UNLOCKED(unlocked_session, "calling run_prompt");

  CRITICAL_AREA_BEGIN_R(session);

  if (session_r->is_offline() || options.offline)
    return std::unexpected(offline_provider_error("prompt"));
  if (!session_r->run_controller())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime session controller is unavailable"));
  auto const request_id = options.request_id.value_or(ava::core::make_id("request"));
  auto const admission = session_r->run_controller()->inspect_admission(RunRequest{.request_id = request_id});
  if (admission == AdmissionDisposition::JoinExistingOutcome)
  {
    auto joined = session_r->run_controller()->wait_outcome(request_id);
    if (!joined)
      return std::unexpected(std::move(joined.error()));
    if (joined->reason == StopReason::PersistenceError && joined->error)
    {
      if (session_r->diagnostics())
        session_r->diagnostics()->record_terminal_failure(ava::diagnostics::RuntimeFailureClass::Session, *joined->error);
      return std::unexpected(*joined->error);
    }
    return ava::agent::AgentLoopResult{.final_text = {},
                                       .usage = std::nullopt,
                                       .cost_usd = std::nullopt,
                                       .provider_iterations = 0,
                                       .tool_calls = 0,
                                       .initial_context_messages = 0,
                                       .used_compacted_context = false,
                                       .tool_iterations = 0,
                                       .outcome = runtime_outcome_for_stop_reason(joined->reason),
                                       .tool_timeline = {}};
  }
  if (admission == AdmissionDisposition::RejectDifferentRequest)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session already has an active run for a different request");
    error.with_context("request_id", request_id);
    return std::unexpected(std::move(error));
  }
  if (admission == AdmissionDisposition::RejectMaintenanceReservation)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session is reserved for exclusive maintenance");
    error.with_context("request_id", request_id);
    return std::unexpected(std::move(error));
  }
  auto admitted = session_r->run_controller()->admit(RunRequest{.request_id = request_id});
  if (!admitted)
    return std::unexpected(std::move(admitted.error()));

  CRITICAL_AREA_END_R(session);

  return run_admitted_prompt(unlocked_session, user_message, provider, transport, options, std::move(*admitted));
}

ava::core::Result<ava::agent::AgentLoopResult> run_admitted_prompt(runtime::session_ts& unlocked_session, std::string const& user_message,
                                                                   ava::provider::Provider const& provider, ava::http::Transport& transport,
                                                                   runtime::RunOptions const& options, ActiveRunGuard guard)
{
  DoutEntering(dc::notice, "run_admitted_prompt(prompt_bytes=" << user_message.size() << ")");
#ifdef CWDEBUG
  auto&& f = at_scope_end([] { Dout(dc::notice, "Leaving run_admitted_prompt()"); });
#endif
  AVA_ASSERT_SESSION_UNLOCKED(unlocked_session, "calling run_admitted_prompt");

  std::shared_ptr<ava::diagnostics::RuntimeDiagnostics> diagnostics_copy;
  std::optional<ava::process::ProcessScopeV1> session_process_scope_copy;
  std::optional<ava::process::ProcessScopeV1> run_process_scope;
  std::optional<ava::core::Error> run_process_scope_error;
  {
    SCOPED_CRITICAL_AREA_R(admitted_session_r, unlocked_session);
    diagnostics_copy = admitted_session_r->diagnostics();
    session_process_scope_copy = admitted_session_r->session_process_scope();
    if (session_process_scope_copy)
    {
      auto derived = session_process_scope_copy->run();
      if (derived)
      {
        run_process_scope = std::move(*derived);
      }
      else
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::Configuration, "failed to derive runtime tool process authority");
        error.with_context("cause", derived.error().message());
        run_process_scope_error = std::move(error);
      }
    }
  }

  // Record a terminal error through stable run resources without retaining or
  // reacquiring Session while controller completion may wait for queued appends.
  auto fail_run = [&guard, &diagnostics = diagnostics_copy](ava::core::Error error) -> ava::core::Result<ava::agent::AgentLoopResult> {
    if (diagnostics)
      if (auto failure_class = diagnostic_failure_class(error))
        diagnostics->record_terminal_failure(*failure_class, error);
    auto completed = guard.complete(RunOutcome{.run_id = {}, .reason = outcome_reason_for_error(error), .error = error});
    if (completed && completed->reason == StopReason::PersistenceError && completed->error)
    {
      if (diagnostics)
        diagnostics->record_terminal_failure(ava::diagnostics::RuntimeFailureClass::Session, *completed->error);
      return std::unexpected(*completed->error);
    }
    return std::unexpected(std::move(error));
  };

  if (run_process_scope_error)
    return fail_run(std::move(*run_process_scope_error));

  std::function<bool()> const caller_cancel_requested = options.cancel_requested;
  std::function<bool()> const run_cancel_requested = [guard_token = guard.stop_token(), caller_cancel_requested] {
    return guard_token.stop_requested() || (caller_cancel_requested && caller_cancel_requested());
  };

  CRITICAL_AREA_BEGIN_R(session);

  std::optional<std::string_view> launch_reasoning_level = std::nullopt;
  if (session_r->reasoning())
    launch_reasoning_level = session_r->reasoning()->level;
  // Normalize immutable private presentation before parent/coordinator locks.
  // Model/provider ids and provider-specific reasoning fields never enter it.
  auto const subagent_launch_display =
      ava::agent::SubagentLaunchDisplay::normalized(ava::config::proven_configured_model_display_name(session_r->model()), launch_reasoning_level);

  auto const run_controller_copy = session_r->run_controller();

  std::string const session_id_copy = session_r->store.session_id();
  std::string const provider_id_copy = session_r->model().provider_id;
  bool const offline_copy = session_r->is_offline() || options.offline;

  // Scope of `refresh`.
  {
    struct ParentRefresh final
    {
      std::shared_ptr<SubagentDeliveryManager> manager;
      std::string session_id;
      std::optional<SubagentDeliveryManager::CapsuleGeneration> generation = std::nullopt;
      ~ParentRefresh()
      {
        if (manager && generation)
          manager->release_parent_if_unused(session_id, *generation);
      }
    } refresh{options.synthetic_subagent_delivery ? nullptr : session_r->subagent_delivery_manager(), session_r->store.session_id()};

    CRITICAL_AREA_END_R(session);

    if (refresh.manager)
    {
      auto retained = refresh.manager->refresh_parent(unlocked_session, options);
      if (!retained)
        return fail_run(std::move(retained.error()));
      refresh.generation = *retained;
    }

    // No session lock may be held when refresh is destructed.
  }

  if (offline_copy)
    return fail_run(offline_provider_error("prompt"));

  if (!run_controller_copy)
    return fail_run(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime session controller is unavailable"));
  auto const subagent_launch_correlation_id_copy = run_controller_copy->snapshot().run_id;
  if (!guard.active())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime prompt admission is inactive"));

  auto session_read_authority = runtime::session_ts::rat(unlocked_session)->read_authority_1();
  if (!session_read_authority)
    return fail_run(std::move(session_read_authority.error()));
  if (auto transitioned = guard.transition(RunPhase::BuildingContext); !transitioned)
    return fail_run(std::move(transitioned.error()));

  // Hook permission audits can overlap provider/tool work, so use this run's
  // immutable route rather than the legacy direct store callback. Isolated
  // runs bypass ambient plugin event hooks entirely.
  ava::agent::SessionAppendSink const append_route = guard.append_route();
  ava::agent::SessionAppendBatchSink const append_batch_route = guard.append_batch_route();
  ava::session::SessionCompactionAppendSink const compaction_append_route = guard.compaction_append_route();
  auto runtime_options = options;
  if (diagnostics_copy)
  {
    auto production_observation = diagnostics_copy->observation();
    if (production_observation && production_observation->enabled())
      runtime_options.observation = std::move(production_observation);
  }
  runtime_options.active_append_route = append_route;
  runtime_options.active_append_batch_route = append_batch_route;
  runtime_options.active_compaction_append_route = compaction_append_route;
  runtime_options.cancel_requested = run_cancel_requested;

  ava::event::RuntimeEventSink event_sink = options.event_sink;
  if (!options.isolate_ambient_extensions)
  {
    auto plugin_observer_options = plugin_event_observer_options(unlocked_session, options.permission_resolver);
    plugin_observer_options.permission_audit_sink = [&append_route](ava::tools::PermissionAuditEvent const& event) {
      return ava::agent::append_permission_decision(append_route, event);
    };
    plugin_observer_options.cancel_requested = run_cancel_requested;
    plugin_observer_options.process_scope = run_process_scope;
    event_sink = make_plugin_event_observer_sink(std::move(plugin_observer_options), options.event_sink);
  }
  runtime_options.event_sink = event_sink;
  // Steering belongs to the controller that admitted this run, even if the
  // frontend later replaces its visible Session object.
  using TakeSteeringMessages = std::function<ava::core::Result<std::vector<std::string>>()>;
  TakeSteeringMessages const take_steering_messages_copy = runtime_options.take_steering_messages;
  runtime_options.take_steering_messages = [&run_controller = run_controller_copy,
                                            &take_steering_messages = take_steering_messages_copy]() -> ava::core::Result<std::vector<std::string>> {
    if (!take_steering_messages)
      return std::vector<std::string>{};
    auto messages_result = take_steering_messages();
    if (!messages_result)
      return std::unexpected(std::move(messages_result.error()));
    auto const run_id = run_controller->snapshot().run_id;
    for (auto const& message : *messages_result)
    {
      // Frontends retain their own bounded visible queues. Controller overflow
      // rejects only the extra steering item at this adapter boundary; it must
      // not abort an otherwise healthy run or discard already admitted input.
      static_cast<void>(run_controller->wake(RunCommand{.kind = RunCommand::Kind::Steering, .correlation_id = run_id, .message = message}));
    }
    auto commands = run_controller->take_commands(run_id);
    if (!commands)
      return std::unexpected(std::move(commands.error()));
    std::vector<std::string> accepted;
    accepted.reserve(commands->size());
    for (auto& command : *commands)
      if (command.kind == RunCommand::Kind::Steering)
        accepted.push_back(std::move(command.message));
    return accepted;
  };
  if (runtime_options.observation && runtime_options.observation->enabled())
  {
    if (runtime_options.trace_context.run_id.empty())
      runtime_options.trace_context.run_id = runtime_options.observation->next_id("run");
    if (runtime_options.trace_context.turn_id.empty())
      runtime_options.trace_context.turn_id = runtime_options.observation->next_id("turn");
    if (runtime_options.trace_context.session_id.empty())
      runtime_options.trace_context.session_id = session_id_copy;
    if (runtime_options.trace_context.provider_id.empty())
      runtime_options.trace_context.provider_id = provider_id_copy;
  }
  std::optional<ava::http::RetryTransport> retry_transport;
  ava::http::Transport* runtime_transport = &transport;
  if (runtime_options.enable_transport_retries)
  {
    retry_transport.emplace(transport, runtime::runtime_retry_options(unlocked_session, runtime_options));
    runtime_transport = &*retry_transport;
    runtime_options.enable_transport_retries = false;
  }
  ava::permissions::PermissionRuleStore const permission_rule_store_copy = runtime::session_ts::rat(unlocked_session)->permission_rule_store();
  ava::permissions::register_enforceable_permission_rule_files(permission_rule_store_copy);

  ava::core::Result<std::string> expanded_user_message = user_message;
  if (runtime_options.expand_prompt_file_references)
  {
    expanded_user_message = expand_prompt_file_references(unlocked_session, user_message, runtime_options);
  }
  if (!expanded_user_message)
    return fail_run(std::move(expanded_user_message.error()));

  // Run events retain the admitted session identity and can therefore be
  // emitted without consulting the replaceable visible Session. The callbacks
  // are owned by the local AgentLoop and cannot outlive this stack copy.
  auto runtime_event_metadata = [&session_id = session_id_copy] {
    return ava::event::RuntimeEventMetadata{.timestamp = ava::session::now_timestamp(), .session_id = session_id};
  };

  ava::event::RuntimeEvent const start_event = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return ava::event::RuntimeEvent{
        ava::event::RuntimeEventMetadata{.timestamp = ava::session::now_timestamp(), .session_id = session_id_copy},
        ava::event::SessionStartEvent{
            .payload = ava::event::SessionPayload{.mode = session_r->mode(), .provider = provider_id_copy, .model = session_r->model().model_id}}};
  }();

  if (auto emitted = ava::event::emit_event(event_sink, start_event); !emitted)
    return fail_run(std::move(emitted.error()));

  {
    ava::event::MessagePayload user_payload;
    user_payload.text = *expanded_user_message;
    if (auto emitted = ava::event::emit_event(
            event_sink, ava::event::RuntimeEvent{runtime_event_metadata(), ava::event::UserMessageEvent{.payload = std::move(user_payload)}});
        !emitted)
    {
      return fail_run(std::move(emitted.error()));
    }
  }

  auto const resource_policy = [&] { return make_extension_resource_policy_1(unlocked_session); }();
  bool const include_ambient = !runtime_options.isolate_ambient_extensions;
  bool const include_project_resources = include_ambient && resource_policy.include_project_resources;

  std::shared_ptr<ava::lsp::DiagnosticsProvider> configured_lsp_provider;
  std::vector<ava::agent::SubagentDefinition> subagents;
  if (include_ambient)
  {
    std::filesystem::path workspace_dir_copy;
    std::shared_ptr<ava::core::AnchorSet> anchor_set_copy;
    ava::agent::Mode mode_copy;
    {
      SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
      workspace_dir_copy = session_r->workspace_dir();
      anchor_set_copy = session_r->anchor_set();
      mode_copy = session_r->mode();
    }
    auto lsp_provider = ava::lsp::make_configured_lsp_provider(ava::lsp::ConfiguredLspProviderFiles{
        .global_config_file = resource_policy.global_lsp_config_file,
        .project_config_file = resource_policy.project_lsp_config_file,
        .workspace_root = workspace_dir_copy,
        .anchor_set = anchor_set_copy,
        .mode = mode_copy,
        .permission_resolver = runtime_options.permission_resolver,
    });
    configured_lsp_provider = lsp_provider ? *lsp_provider : nullptr;
    subagents = ava::agent::load_subagents(ava::agent::SubagentLoadOptions{.workspace_root = std::move(workspace_dir_copy),
                                                                           .include_project_agents = resource_policy.include_project_resources})
                    .subagents;
  }

  auto effective_session_mcp_config = runtime::session_ts::rat(unlocked_session)->mcp_config();
  if (runtime_options.disable_session_mcp ||
      (!effective_session_mcp_config && (runtime_options.isolate_ambient_extensions || runtime_options.exact_builtin_tool_names.has_value())))
  {
    effective_session_mcp_config = std::make_shared<ava::mcp::McpConfig const>();
  }

  std::optional<ava::core::Error> sink_error;
  CRITICAL_AREA_CONTINUE_R(session);
  auto run_store = session_r->store;
  auto const paths_copy = session_r->paths();
  auto const provider_catalog_copy = session_r->provider_catalog();
  CRITICAL_AREA_END_R(session);

  ava::http::TransportFactory session_transport_factory;
  if (session_process_scope_copy)
  {
    session_transport_factory = [scope = *session_process_scope_copy]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
      std::unique_ptr<ava::http::Transport> transport = std::make_unique<ava::http::CurlCliTransport>(scope);
      return transport;
    };
  }

  auto child_compaction_config = ava::session::load_compaction_config(paths_copy);
  if (!child_compaction_config)
    return fail_run(std::move(child_compaction_config.error()));
  child_compaction_config->provider_id = provider_id_copy;
  child_compaction_config->model_id = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return session_r->model().model_id;
  }();
  child_compaction_config->provider_explicit = false;
  child_compaction_config->model_explicit = false;

  std::optional<ava::agent::AgentLoop> loop;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    loop.emplace(ava::agent::AgentLoopOptions{
        .workspace_dir = session_r->workspace_dir(),
        .current_dir = session_r->current_dir(),
        .additional_writable_dirs = session_r->additional_writable_dirs(),
        .anchor_set = session_r->anchor_set(),
        .mode = session_r->mode(),
        .model =
            ava::agent::ModelInvocationOptions{
                .provider_id = provider_id_copy,
                .model_id = session_r->model().model_id,
                .system_prompt = runtime_options.isolate_ambient_extensions ? session_r->ambient_extension_free_system_prompt() : session_r->system_prompt(),
                .stream = runtime_options.stream,
                .supports_tools = session_r->model().supports_tools.value_or(true),
                .supports_streaming = session_r->model().supports_streaming.value_or(true),
                .input_modalities = session_r->model().input_modalities,
                .max_output_tokens = session_r->model().max_output_tokens,
                .reasoning = session_r->reasoning() ? std::optional(runtime::provider_reasoning_options(*session_r->reasoning())) : std::nullopt,
                .pricing = session_r->model().pricing,
                .api_family = session_r->model().api_family,
                .reasoning_format = session_r->model().reasoning_format,
                .compatibility_quirks = session_r->model().compatibility_quirks,
            },
        .access_token = options.access_token,
        .credential_type = options.openai_oauth && options.credential_type == "bearer" ? "oauth" : options.credential_type,
        .openai_oauth = options.openai_oauth,
        .openai_account_id = options.openai_account_id,
        .tool_resources =
            ava::agent::ToolResourceOptions{
                .include_project_resources = include_project_resources,
                .lsp_diagnostics_provider = configured_lsp_provider,
                .plugin_global_plugins_dir = resource_policy.plugin_discovery.global_plugins_dir,
                .plugin_project_plugins_dir = resource_policy.plugin_discovery.project_plugins_dir,
                .plugin_enablement_file = resource_policy.plugin_enablement_file,
                .include_plugin_tools = include_ambient,
                .mcp_global_config_file = resource_policy.mcp_config.global_config_file,
                .mcp_project_config_file = resource_policy.mcp_config.project_config_file,
                .include_global_mcp_config = include_ambient,
                .session_mcp_config = std::move(effective_session_mcp_config),
                .skill_global_dirs = resource_policy.global_skill_dirs,
                .skill_project_dirs = resource_policy.project_skill_dirs,
                .include_global_skills = include_ambient,
                .exact_builtin_tool_names = runtime_options.exact_builtin_tool_names,
            },
        .tool_execution =
            ava::agent::ToolExecutionOptions{
                .require_descriptor_secure_workspace = runtime_options.require_descriptor_secure_workspace,
                .announce_execution_after_permission = runtime_options.announce_execution_after_permission,
                .redact_permission_audit_arguments = runtime_options.redact_permission_audit_arguments,
                .require_explicit_file_permissions = runtime_options.require_explicit_file_permissions,
                .ava_authority_roots = session_r->ava_authority_roots_1(),
                .exact_file_access = runtime_options.exact_file_access,
                .command_executor = runtime_options.command_executor,
                .cancel_requested = run_cancel_requested,
                .process_scope = run_process_scope,
            },
        .subagents = std::move(subagents),
        .tool_visibility = session_r->tool_visibility(),
        .on_tool_event =
            [&runtime_event_metadata, &event_sink, &sink_error](ava::agent::ToolTimelineEntry const& entry) {
              if (sink_error)
                return;
            // Capture metadata before mapping payload fields, matching the old base-event path.
              auto metadata = runtime_event_metadata();
              if (auto emitted = ava::event::emit_event(event_sink, runtime_event_from_tool_timeline_entry(std::move(metadata), entry)); !emitted)
              {
                sink_error = std::move(emitted.error());
              }
            },
        .on_tool_progress = [&runtime_event_metadata, &event_sink, &sink_error](ava::agent::ToolProgressEntry const& entry) -> ava::core::VoidResult {
          if (sink_error)
            return std::unexpected(*sink_error);
          auto metadata = runtime_event_metadata();
          if (auto emitted = ava::event::emit_event(event_sink, runtime_event_from_tool_progress_entry(std::move(metadata), entry)); !emitted)
          {
            sink_error = std::move(emitted.error());
            return std::unexpected(*sink_error);
          }
          return {};
        },
        .on_stream_event = [&runtime_event_metadata, &event_sink, &sink_error](ava::provider::StreamEvent const& stream_event) -> ava::core::VoidResult {
          if (sink_error)
            return std::unexpected(*sink_error);
          auto metadata = runtime_event_metadata();
          if (auto emitted = ava::event::emit_event(event_sink, runtime_event_from_provider_stream_event(std::move(metadata), stream_event)); !emitted)
          {
            sink_error = std::move(emitted.error());
            return std::unexpected(*sink_error);
          }
          return {};
        },
        .subagent_launch = {.display = subagent_launch_display,
                            .request_id = subagent_launch_correlation_id_copy,
                            .correlation_id = subagent_launch_correlation_id_copy,
                            .sink = runtime_options.on_subagent_launch},
        .permission_resolver = runtime_options.permission_resolver,
        .auto_allow_deny_preflight = ava::permissions::build_persistent_permission_deny_preflight(permission_rule_store_copy),
        .question_resolver = runtime_options.question_resolver,
        .cancel_requested = [&runtime_options,
                             &sink_error] { return sink_error.has_value() || (runtime_options.cancel_requested && runtime_options.cancel_requested()); },
        .take_steering_messages = runtime_options.take_steering_messages,
        .compact_context = (runtime_options.access_token.empty() && runtime_options.credential_type != "none")
                               ? decltype(ava::agent::AgentLoopOptions{}.compact_context){}
                               : [&](ava::session::SessionReadAuthority read_authority, std::string_view trigger,
                                     std::vector<std::string> const& replayed_user_messages) -> ava::core::Result<bool> {
          return runtime::compact_runtime_context(unlocked_session, std::move(read_authority), trigger, provider, *runtime_transport, runtime_options,
                                                  replayed_user_messages);
        },
        .child_compaction_blueprint =
            ava::agent::ChildContextCompactionBlueprint{.config = *child_compaction_config, .context_window_tokens = session_r->model().context_window_tokens},
        .background_provider_factory = [&provider_catalog = provider_catalog_copy, &paths = paths_copy,
                                        &provider_id = provider_id_copy]() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
          auto ensured = ava::provider::ensure_provider_catalog(provider_catalog, paths);
          if (!ensured)
            return std::unexpected(std::move(ensured.error()));
          return (*ensured)->create(provider_id);
        },
        .transport_factory = session_transport_factory,
        .background_transport_factory = session_transport_factory,
        .subagent_coordinator = session_r->subagent_coordinator(),
        .append_entry = append_route,
        .append_batch = std::move(append_batch_route),
        .session_read_authority = std::move(*session_read_authority),
        .session_read_limits = session_r->session_read_limits(),
        .synthetic_user_message_provenance = runtime_options.synthetic_subagent_delivery ? runtime_options.synthetic_user_message_provenance : std::nullopt,
        .on_phase = [&guard, &runtime_options](ava::agent::RunPhase phase) -> ava::core::VoidResult {
          if (phase == ava::agent::RunPhase::Completing && runtime_options.on_terminal_commit)
          {
            if (auto committed = runtime_options.on_terminal_commit(); !committed)
              return committed;
          }
          if (auto transitioned = guard.transition(phase); !transitioned)
            return transitioned;
          if (runtime_options.on_phase)
            return runtime_options.on_phase(phase);
          return {};
        },
        .observation = runtime_options.observation,
        .trace_context = runtime_options.trace_context});
  }

  auto result = loop->run_turn(*expanded_user_message, runtime_options.image_attachments, run_store, provider, *runtime_transport);
  if (sink_error)
  {
    return fail_run(std::move(*sink_error));
  }
  if (!result)
  {
    auto metadata = runtime_event_metadata();
    if (is_agent_loop_canceled_error(result.error()))
    {
      ava::event::CancellationPayload cancel_payload;
      cancel_payload.text = "stopped by user";
      cancel_payload.error_category = ava::core::to_string(result.error().category());
      cancel_payload.error_message = result.error().message();
      cancel_payload.error_details = result.error().format();
      cancel_payload.reason = result.error().message();
      static_cast<void>(ava::event::emit_event(
          event_sink, ava::event::RuntimeEvent{std::move(metadata), ava::event::CancellationEvent{.payload = std::move(cancel_payload)}}));
    }
    else
    {
      ava::event::ErrorPayload error_payload;
      error_payload.error_category = ava::core::to_string(result.error().category());
      error_payload.error_message = result.error().message();
      error_payload.error_details = result.error().format();
      static_cast<void>(
          ava::event::emit_event(event_sink, ava::event::RuntimeEvent{std::move(metadata), ava::event::ErrorEvent{.payload = std::move(error_payload)}}));
    }
    return fail_run(result.error());
  }

  {
    ava::event::MessagePayload assistant_payload;
    assistant_payload.text = result->final_text;
    if (auto emitted = ava::event::emit_event(
            event_sink, ava::event::RuntimeEvent{runtime_event_metadata(), ava::event::AssistantMessageEvent{.payload = std::move(assistant_payload)}});
        !emitted)
    {
      return fail_run(std::move(emitted.error()));
    }
  }

  {
    ava::event::CompletionPayload completion_payload;
    completion_payload.stop_reason = std::string(ava::core::to_string(result->outcome));
    completion_payload.provider_iterations = result->provider_iterations;
    completion_payload.tool_calls = result->tool_calls;
    if (auto emitted = ava::event::emit_event(
            event_sink, ava::event::RuntimeEvent{runtime_event_metadata(), ava::event::CompletionEvent{.payload = std::move(completion_payload)}});
        !emitted)
    {
      return fail_run(std::move(emitted.error()));
    }
  }
  if (auto transitioned = guard.transition(RunPhase::Completing); !transitioned)
    return fail_run(std::move(transitioned.error()));
  auto const proposed_reason = stop_reason_for_runtime_outcome(result->outcome);
  auto completed = guard.complete(RunOutcome{.run_id = {}, .reason = proposed_reason});
  if (!completed)
    return std::unexpected(std::move(completed.error()));
  if (completed->reason == StopReason::PersistenceError && completed->error)
  {
    if (diagnostics_copy)
      diagnostics_copy->record_terminal_failure(ava::diagnostics::RuntimeFailureClass::Session, *completed->error);
    return std::unexpected(*completed->error);
  }
  if (completed->reason != proposed_reason)
    result->outcome = runtime_outcome_for_stop_reason(completed->reason);

  // This boundary is deliberately after AdmissionGuard completion, not the
  // earlier Done event. The coordinator is best-effort and cannot change the
  // already committed ordinary user turn.
  if (result->committed_turn_id && !options.synthetic_subagent_delivery)
  {
    auto title_coordinator = runtime::session_ts::rat(unlocked_session)->session_title_coordinator();
    if (title_coordinator)
      title_coordinator->schedule(unlocked_session, user_message, *result->committed_turn_id, options);
  }

  return result;
}

}  // namespace ava::app
