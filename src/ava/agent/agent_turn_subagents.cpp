#include "sys.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_turn_executor_internal.h"
#include "ava/agent/job_control.h"
#include "ava/agent/subagent_inspector_source.h"
#include "ava/session/session_branch.h"
#include "ava/session/session_metadata.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::agent::detail {
namespace {

constexpr std::size_t kMaxAvaAuthorityRoots = 64;

void append_authority_root(std::vector<std::filesystem::path>& roots, std::filesystem::path root)
{
  if (root.empty())
    return;
  root = root.lexically_normal();
  if (std::ranges::find(roots, root) == roots.end())
    roots.push_back(std::move(root));
}

bool contains_tool_name(std::vector<std::string> const& tools, std::string_view name)
{
  return std::find(tools.begin(), tools.end(), name) != tools.end();
}

void add_excluded_tool(ToolVisibilityOptions& visibility, std::string_view name)
{
  if (!contains_tool_name(visibility.excluded_tools, name))
    visibility.excluded_tools.emplace_back(name);
}

ToolVisibilityOptions subagent_tool_visibility(ToolVisibilityOptions parent, SubagentToolPreset tool_preset)
{
  add_excluded_tool(parent, "task");
  add_excluded_tool(parent, "job");
  add_excluded_tool(parent, "todowrite");
  if (tool_preset != SubagentToolPreset::ReadOnly)
    return parent;

  std::vector<std::string> const read_only_tools{"read_file", "list_directory", "glob", "grep"};
  if (parent.included_tools.empty())
  {
    parent.included_tools = read_only_tools;
    return parent;
  }

  std::vector<std::string> narrowed;
  for (auto const& tool : parent.included_tools)
  {
    if (contains_tool_name(read_only_tools, tool))
      narrowed.push_back(tool);
  }
  parent.included_tools = std::move(narrowed);
  return parent;
}

// Start empty: session identity, delivery provenance, and parent callbacks are
// never inherited. Exact child authority and worker routes are installed later.
AgentLoopOptions inherited_child_options(AgentLoopOptions const& parent)
{
  AgentLoopOptions child;
  child.workspace_dir = parent.workspace_dir;
  child.current_dir = parent.current_dir;
  child.additional_writable_dirs = parent.additional_writable_dirs;
  child.anchor_set = parent.anchor_set;
  child.mode = parent.mode;
  child.model = parent.model;
  child.access_token = parent.access_token;
  child.credential_type = parent.credential_type;
  child.openai_oauth = parent.openai_oauth;
  child.openai_account_id = parent.openai_account_id;
  child.max_tool_iterations = parent.max_tool_iterations;
  child.max_provider_events = parent.max_provider_events;
  child.max_assistant_text_bytes = parent.max_assistant_text_bytes;
  child.max_tool_argument_bytes = parent.max_tool_argument_bytes;
  child.max_tool_result_context_bytes = parent.max_tool_result_context_bytes;
  child.tool_resources = parent.tool_resources;
  child.tool_execution.require_descriptor_secure_workspace = parent.tool_execution.require_descriptor_secure_workspace;
  child.tool_execution.announce_execution_after_permission = parent.tool_execution.announce_execution_after_permission;
  child.tool_execution.redact_permission_audit_arguments = parent.tool_execution.redact_permission_audit_arguments;
  child.tool_execution.require_explicit_file_permissions = parent.tool_execution.require_explicit_file_permissions;
  child.tool_execution.ava_authority_roots = parent.tool_execution.ava_authority_roots;
  child.tool_execution.exact_file_access = parent.tool_execution.exact_file_access;
  child.tool_execution.command_executor = parent.tool_execution.command_executor;
  child.tool_execution.cancel_requested = parent.tool_execution.cancel_requested;
  child.subagents = parent.subagents;
  child.tool_visibility = parent.tool_visibility;
  child.permission_resolver = parent.permission_resolver;
  child.auto_allow_deny_preflight = parent.auto_allow_deny_preflight;
  child.question_resolver = parent.question_resolver;
  child.cancel_requested = parent.cancel_requested;
  child.child_compaction_blueprint = parent.child_compaction_blueprint;
  child.transport_factory = parent.transport_factory;
  child.session_read_limits = parent.session_read_limits;
  child.parallel_read_search_tools = parent.parallel_read_search_tools;
  child.parallel_read_search_max_workers = parent.parallel_read_search_max_workers;
  child.observation = parent.observation;
  child.subagent_launch.display = parent.subagent_launch.display;
  // Anchors, resource capabilities and observation deliberately share ownership
  // with the parent; process authority is replaced before child publication.
  child.child_execution = true;
  return child;
}

std::string subagent_system_prompt(std::string base, std::string_view role_prompt)
{
  auto const role = role_prompt.empty()
                        ? std::string("You are AVA's subagent. Complete the delegated task and return only the result needed by the parent agent.")
                        : std::string(role_prompt);
  if (base.empty())
    return role;
  base += "\n\n";
  base += role;
  return base;
}

void append_subagent_error_best_effort(SessionAppendSink const& append_sink, ava::core::Error const& error)
{
  if (!append_sink)
    return;
  auto safe = ava::core::Error(error.category(), safe_subagent_error_message(error), error.code());
  static_cast<void>(append_error(append_sink, safe));
}

bool subagent_terminal(SubagentExecutionState state) noexcept
{
  return state == SubagentExecutionState::Completed || state == SubagentExecutionState::Failed || state == SubagentExecutionState::Canceled ||
         state == SubagentExecutionState::Interrupted;
}

BackgroundJobCompletion background_failure_completion(BackgroundJobContext const& context, ava::core::Error const& error)
{
  if (context.stop_token.stop_requested())
  {
    return BackgroundJobCompletion{.state = BackgroundJobState::Canceled, .final_text = "", .stop_reason = "canceled", .error = error};
  }
  return BackgroundJobCompletion{.state = BackgroundJobState::Failed, .final_text = "", .stop_reason = "failed", .error = error};
}

}  // namespace

std::pair<std::vector<std::filesystem::path>, bool> bounded_deduplicated_authority_roots(std::vector<std::filesystem::path> roots)
{
  std::vector<std::filesystem::path> bounded;
  bounded.reserve(std::min(roots.size(), kMaxAvaAuthorityRoots));
  bool over_limit = false;
  for (auto& root : roots)
  {
    if (root.empty())
      continue;
    root = root.lexically_normal();
    if (std::ranges::find(bounded, root) != bounded.end())
      continue;
    if (bounded.size() == kMaxAvaAuthorityRoots)
    {
      over_limit = true;
      continue;
    }
    bounded.push_back(std::move(root));
  }
  return {std::move(bounded), over_limit};
}

ava::core::Result<TaskSubagentResult> AgentTurnExecutor::run_task_subagent(TaskSubagentRequest const& request)
{
  auto const session_root = store_.session_path().parent_path().parent_path();
  bool const has_provider_factory = static_cast<bool>(options_.background_provider_factory);
  bool const has_transport_factory = static_cast<bool>(options_.background_transport_factory);
  bool const has_coordinator = static_cast<bool>(options_.subagent_coordinator);
  bool const use_coordinator = has_coordinator && has_provider_factory && has_transport_factory;
  if (request.background && !use_coordinator)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "background task subagents are unavailable");
    error.with_context("subagent_type", request.subagent_type);
    return std::unexpected(std::move(error));
  }
  if (!request.background && (has_coordinator || has_provider_factory || has_transport_factory) && !use_coordinator)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "coordinated foreground task subagents are unavailable");
    error.with_context("subagent_type", request.subagent_type);
    return std::unexpected(std::move(error));
  }

  std::optional<ava::process::ProcessScopeV1> child_run_process_scope;
  if (options_.tool_execution.process_scope)
  {
    auto child_session_scope = options_.tool_execution.process_scope->application_scope().session();
    if (!child_session_scope)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Configuration, "failed to derive task subagent session process authority");
      error.with_context("cause", child_session_scope.error().message());
      return std::unexpected(std::move(error));
    }
    auto child_run_scope = child_session_scope->run();
    if (!child_run_scope)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Configuration, "failed to derive task subagent run process authority");
      error.with_context("cause", child_run_scope.error().message());
      return std::unexpected(std::move(error));
    }
    child_run_process_scope = std::move(*child_run_scope);
  }

  auto child_store_result = request.task_id ? ava::session::SessionStore::open(options_.workspace_dir, *request.task_id, session_root)
                                            : ava::session::SessionStore::create(options_.workspace_dir, session_root);
  if (!child_store_result)
    return std::unexpected(std::move(child_store_result.error()));
  auto child_store = std::move(*child_store_result);

  auto child_lease_result = request.task_id ? ava::session::SessionLease::acquire(child_store.session_path())
                                            : ava::session::SessionLease::create_and_acquire(child_store.session_path());
  if (!child_lease_result)
    return std::unexpected(std::move(child_lease_result.error()));
  auto child_lease = std::move(*child_lease_result);
  if (request.task_id)
  {
    // Ownership is checked against the exact leased recoverable prefix before
    // any recovery or append can mutate the requested child session.
    auto ownership_entries = child_store.load_recoverable_prefix_bounded(child_lease, options_.session_read_limits, options_.cancel_requested);
    if (!ownership_entries)
      return std::unexpected(std::move(ownership_entries.error()));
    auto ownership = ava::session::session_metadata_from_entries(child_store.session_id(), *ownership_entries);
    if (!ownership)
      return std::unexpected(std::move(ownership.error()));
    if (ownership->parent_session_id != store_.session_id())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "task child session is not owned by the current parent session");
      error.with_context("task_id", *request.task_id);
      return std::unexpected(std::move(error));
    }
    auto recovered = child_store.recover_torn_tail(child_lease, options_.session_read_limits, options_.cancel_requested);
    if (!recovered)
      return std::unexpected(std::move(recovered.error()));
    auto staged_recovery = child_store.recover_incomplete_assistant_output_suffix(child_lease, options_.session_read_limits, options_.cancel_requested);
    if (!staged_recovery)
      return std::unexpected(std::move(staged_recovery.error()));
  }
  else
  {
    auto name = request.description + " (@" + request.subagent_type + " subagent)";
    if (name.size() > ava::session::kMaxSessionNameBytes)
    {
      name.resize(ava::session::kMaxSessionNameBytes);
    }
    auto metadata = ava::session::append_session_metadata(
        child_store, child_lease, ava::session::SessionMetadataUpdate{.name = std::move(name), .parent_session_id = store_.session_id(), .actor = "subagent"});
    if (!metadata)
      return std::unexpected(std::move(metadata.error()));
  }

  auto child_read_authority = ava::session::SessionReadAuthority::create_persistent(child_store, child_lease, options_.session_read_limits);
  if (!child_read_authority)
    return std::unexpected(std::move(child_read_authority.error()));

  auto child_options = inherited_child_options(options_);
  child_options.tool_execution.process_scope = std::move(child_run_process_scope);
  // A child owns a distinct exact session namespace. Preserve the parent
  // roots and add the child directory before its AgentLoop constructs any
  // model ToolContext; duplicates remain bounded and harmless.
  append_authority_root(child_options.tool_execution.ava_authority_roots, child_store.session_path().parent_path());
  child_options.session_read_authority = std::move(*child_read_authority);
  child_options.model.system_prompt = subagent_system_prompt(options_.model.system_prompt, request.subagent_system_prompt);
  child_options.tool_visibility = subagent_tool_visibility(options_.tool_visibility, request.tool_preset);
  child_options.max_tool_iterations = request.max_tool_iterations.value_or(options_.max_tool_iterations);
  // A child owns a fresh lifecycle/session identity. Parent IDs are typed
  // correlation metadata only and never become child lifecycle IDs.
  child_options.trace_context = {.run_id = {},
                                 .turn_id = {},
                                 .session_id = {},
                                 .provider_id = options_.model.provider_id,
                                 .parent_run_id = trace_context_.run_id,
                                 .parent_turn_id = trace_context_.turn_id,
                                 .parent_session_id = store_.session_id()};

  if (use_coordinator)
  {
    auto const task_id = child_store.session_id();
    auto const session_path = child_store.session_path();
    auto child_provider = options_.background_provider_factory();
    if (!child_provider)
    {
      auto error = std::move(child_provider.error());
      if (!request.task_id)
        ava::session::rollback_created_session_with_context(child_store, child_lease, error);
      return std::unexpected(std::move(error));
    }
    auto child_transport = options_.background_transport_factory();
    if (!child_transport)
    {
      auto error = std::move(child_transport.error());
      if (!request.task_id)
        ava::session::rollback_created_session_with_context(child_store, child_lease, error);
      return std::unexpected(std::move(error));
    }
    auto interaction_gate = SubagentInteractionGate::create(request.background ? SubagentJobMode::Background : SubagentJobMode::Foreground,
                                                            options_.permission_resolver, options_.question_resolver);
    child_options.permission_resolver = interaction_gate->permission_resolver();
    child_options.question_resolver = interaction_gate->question_resolver();
    child_options.tool_resources.lsp_diagnostics_provider = nullptr;
    struct CoordinatedTaskResultState
    {
      std::mutex mutex;
      std::optional<AgentLoopResult> terminal_result = std::nullopt;
    };
    struct CoordinatedTaskRunState
    {
      ava::session::SessionStore child_store;
      ava::session::SessionLease child_lease;
      AgentLoopOptions child_options;
      SessionAppendSink child_append;
      std::string prompt;
      std::shared_ptr<CoordinatedTaskResultState> result_state;
      std::unique_ptr<ava::provider::Provider> provider_instance;
      std::unique_ptr<ava::http::Transport> transport_instance;
      std::shared_ptr<SubagentInteractionGate> interaction_gate;
    };
    auto steering_queue = SubagentSteeringQueue::create();
    child_options.take_steering_messages = [queue = steering_queue] { return queue->take(); };
    auto run_state = std::make_shared<CoordinatedTaskRunState>(CoordinatedTaskRunState{.child_store = std::move(child_store),
                                                                                       .child_lease = std::move(child_lease),
                                                                                       .child_options = std::move(child_options),
                                                                                       .child_append = {},
                                                                                       .prompt = request.prompt,
                                                                                       .result_state = std::make_shared<CoordinatedTaskResultState>(),
                                                                                       .provider_instance = std::move(*child_provider),
                                                                                       .transport_instance = std::move(*child_transport),
                                                                                       .interaction_gate = interaction_gate});
    auto child_target =
        ava::session::SessionAppendTarget::create_persistent(run_state->child_store, run_state->child_lease, run_state->child_options.session_read_limits);
    if (!child_target)
    {
      auto error = std::move(child_target.error());
      if (!request.task_id)
        ava::session::rollback_created_session_with_context(run_state->child_store, run_state->child_lease, error);
      return std::unexpected(std::move(error));
    }
    if (auto reconciled = reconcile_unresolved_committed_function_calls(
            *run_state->child_options.session_read_authority, [target = *child_target](ava::session::SessionEntry entry) { return target->append(entry); },
            run_state->child_options.session_read_limits);
        !reconciled)
    {
      auto error = std::move(reconciled.error());
      if (!request.task_id)
        ava::session::rollback_created_session_with_context(run_state->child_store, run_state->child_lease, error);
      return std::unexpected(std::move(error));
    }
    if (run_state->child_options.child_compaction_blueprint)
    {
      run_state->child_options.child_compaction_binding =
          ChildContextCompactionBinding{.blueprint = *run_state->child_options.child_compaction_blueprint, .append_target = *child_target};
    }
    auto child_append_target = *child_target;
    run_state->child_options.append_entry = [target = child_append_target](ava::session::SessionEntry entry) { return target->append(entry); };
    run_state->child_options.append_batch = [target = std::move(child_append_target)](std::vector<ava::session::SessionEntry> entries) {
      return target->append_batch(std::move(entries));
    };
    run_state->child_append = run_state->child_options.append_entry;
    BackgroundJobStartOptions start_options{.title = request.description,
                                            .description = request.prompt,
                                            .subagent_type = request.subagent_type,
                                            .child_session_id = task_id,
                                            .child_session_path = session_path};
    // Build the inspection source from an exact copy of the child authority
    // before coordinator start/publication. No raw authority escapes start().
    auto inspection_source = SubagentLiveInspectionSource::create(*run_state->child_options.session_read_authority);
    if (!inspection_source)
    {
      auto error = std::move(inspection_source.error());
      if (!request.task_id)
        ava::session::rollback_created_session_with_context(run_state->child_store, run_state->child_lease, error);
      return std::unexpected(std::move(error));
    }
    BackgroundJobWorker worker = [run_state](BackgroundJobContext const& context) mutable {
      struct FinishInteractionGate final
      {
        std::shared_ptr<SubagentInteractionGate> gate;
        ~FinishInteractionGate() { gate->finish(); }
      } finish_gate{run_state->interaction_gate};
      std::function<bool()> child_cancel_requested = [stop_token = context.stop_token] { return stop_token.stop_requested(); };
      run_state->child_options.cancel_requested = child_cancel_requested;
      run_state->child_options.tool_execution.cancel_requested = std::move(child_cancel_requested);
      AgentLoop child_loop(std::move(run_state->child_options));
      auto child_result = child_loop.run_turn(run_state->prompt, run_state->child_store, *run_state->provider_instance, *run_state->transport_instance);
      if (!child_result)
      {
        auto error = child_result.error();
        if (!context.stop_token.stop_requested())
          append_subagent_error_best_effort(run_state->child_append, error);
        return background_failure_completion(context, error);
      }
      auto completion = BackgroundJobCompletion{.state = BackgroundJobState::Completed,
                                                .final_text = child_result->final_text,
                                                .stop_reason = std::string(ava::core::to_string(child_result->outcome)),
                                                .provider_iterations = child_result->provider_iterations,
                                                .tool_calls = child_result->tool_calls,
                                                .tool_iterations = child_result->tool_iterations};
      {
        std::lock_guard lock(run_state->result_state->mutex);
        run_state->result_state->terminal_result = *child_result;
      }
      return completion;
    };

    auto coordinated = options_.subagent_coordinator->start(
        SubagentCoordinatorStartRequest{.parent_session_id = store_.session_id(),
                                        .mode = request.background ? SubagentJobMode::Background : SubagentJobMode::Foreground,
                                        .job = std::move(start_options),
                                        .launch_display = run_state->child_options.subagent_launch.display,
                                        .steering_queue = std::move(steering_queue),
                                        .history_append = options_.job_history_append},
        std::move(worker), interaction_gate, std::move(*inspection_source));
    if (!coordinated)
    {
      auto error = std::move(coordinated.error());
      // Roll back only with a positive proof that no coordinator state was
      // published. The uncertain compatibility value must remain fail-closed.
      if (!request.task_id && subagent_publication_commit_state(error) == SubagentPublicationCommitState::ProvenUnpublished)
        ava::session::rollback_created_session_with_context(run_state->child_store, run_state->child_lease, error);
      return std::unexpected(std::move(error));
    }
    auto const job_id = coordinated->job.identity.job_id;
    auto const job_state = std::string(to_string(coordinated->job.execution));

    if (request.background)
    {
      return TaskSubagentResult{.task_id = task_id,
                                .job_id = job_id,
                                .session_path = session_path,
                                .subagent_type = request.subagent_type,
                                .state = job_state,
                                .final_text = "",
                                .stop_reason = "background",
                                .provider_iterations = 0,
                                .tool_calls = 0,
                                .tool_iterations = 0};
    }

    for (;;)
    {
      auto waited = options_.subagent_coordinator->wait(store_.session_id(), job_id, std::chrono::milliseconds(50), SubagentWaitMode::TerminalOrPromotion);
      if (!waited)
        return std::unexpected(std::move(waited.error()));
      if (waited->job.was_promoted && !subagent_terminal(waited->job.execution))
      {
        return TaskSubagentResult{.task_id = task_id,
                                  .job_id = job_id,
                                  .session_path = session_path,
                                  .subagent_type = request.subagent_type,
                                  .state = std::string(to_string(waited->job.execution)),
                                  .final_text = "",
                                  .stop_reason = "promoted",
                                  .provider_iterations = 0,
                                  .tool_calls = 0,
                                  .tool_iterations = 0};
      }
      if (subagent_terminal(waited->job.execution))
      {
        if (waited->timed_out)
          continue;
        if (waited->job.execution != SubagentExecutionState::Completed)
        {
          auto error =
              ava::core::Error(ava::core::ErrorCategory::Tool,
                               waited->job.execution == SubagentExecutionState::Canceled ? "foreground subagent was canceled" : "foreground subagent failed",
                               waited->job.execution == SubagentExecutionState::Canceled ? ava::core::ErrorCode::Canceled : ava::core::ErrorCode::Unspecified);
          error.with_context("job_id", job_id);
          if (waited->job.error)
            error.with_context("cause", *waited->job.error);
          if (waited->job.stop_reason)
            error.with_context("stop_reason", *waited->job.stop_reason);
          return std::unexpected(std::move(error));
        }
        std::lock_guard result_lock(run_state->result_state->mutex);
        if (!run_state->result_state->terminal_result)
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "foreground subagent terminal result is unavailable"));
        return TaskSubagentResult{.task_id = task_id,
                                  .job_id = job_id,
                                  .session_path = session_path,
                                  .subagent_type = request.subagent_type,
                                  .state = std::string(to_string(waited->job.execution)),
                                  .final_text = run_state->result_state->terminal_result->final_text,
                                  .stop_reason = std::string(ava::core::to_string(run_state->result_state->terminal_result->outcome)),
                                  .provider_iterations = run_state->result_state->terminal_result->provider_iterations,
                                  .tool_calls = run_state->result_state->terminal_result->tool_calls,
                                  .tool_iterations = run_state->result_state->terminal_result->tool_iterations};
      }
      if (options_.cancel_requested && options_.cancel_requested())
        static_cast<void>(options_.subagent_coordinator->cancel(store_.session_id(), job_id));
    }
  }

  auto child_target = ava::session::SessionAppendTarget::create_persistent(child_store, child_lease, child_options.session_read_limits);
  if (!child_target)
    return std::unexpected(std::move(child_target.error()));
  if (auto reconciled = reconcile_unresolved_committed_function_calls(
          *child_options.session_read_authority, [target = *child_target](ava::session::SessionEntry entry) { return target->append(entry); },
          child_options.session_read_limits);
      !reconciled)
  {
    return std::unexpected(std::move(reconciled.error()));
  }
  if (child_options.child_compaction_blueprint)
  {
    child_options.child_compaction_binding =
        ChildContextCompactionBinding{.blueprint = *child_options.child_compaction_blueprint, .append_target = *child_target};
  }
  auto child_append_target = *child_target;
  child_options.append_entry = [target = child_append_target](ava::session::SessionEntry entry) { return target->append(entry); };
  child_options.append_batch = [target = std::move(child_append_target)](std::vector<ava::session::SessionEntry> entries) {
    return target->append_batch(std::move(entries));
  };
  AgentLoop child_loop(std::move(child_options));
  auto child_result = child_loop.run_turn(request.prompt, child_store, provider_, transport_);
  if (!child_result)
    return std::unexpected(std::move(child_result.error()));
  return TaskSubagentResult{.task_id = child_store.session_id(),
                            .job_id = "",
                            .session_path = child_store.session_path(),
                            .subagent_type = request.subagent_type,
                            .final_text = child_result->final_text,
                            .stop_reason = std::string(ava::core::to_string(child_result->outcome)),
                            .provider_iterations = child_result->provider_iterations,
                            .tool_calls = child_result->tool_calls,
                            .tool_iterations = child_result->tool_iterations};
}

}  // namespace ava::agent::detail
