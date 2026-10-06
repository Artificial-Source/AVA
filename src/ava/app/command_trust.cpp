#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_trust.h"
#include "ava/app/commands.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_prompt.h"
#include "ava/app/subagent_delivery_manager.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ava::app {
namespace {

std::string project_trust_summary(ProjectTrustState const& state)
{
  std::string output = "Project trust:\n";
  output += "  workspace=" + state.workspace_dir.string() + "\n";
  output += "  decision=" + std::string(to_string(state.decision)) + "\n";
  if (!state.matched_path.empty())
    output += "  matched=" + state.matched_path.string() + "\n";
  output += "  trust_file=" + state.trust_file.string() + "\n";
  output += "  project_resources=" + std::string(project_resources_trusted(state) ? "enabled" : "skipped") + "\n";
  if (!state.diagnostic.empty())
    output += "  diagnostic=" + sanitize_inline_text(state.diagnostic) + "\n";
  if (state.protected_resources.empty())
  {
    output += "  protected_resources=none";
    return output;
  }
  output += "  protected_resources=" + std::to_string(state.protected_resources.size()) + "\n";
  for (auto const& resource : state.protected_resources)
  {
    output += "    " + sanitize_inline_text(resource.kind) + "  " + resource.path.string() + "\n";
  }
  if (!output.empty() && output.back() == '\n')
    output.pop_back();
  return output;
}

ava::core::Error transaction_error(std::string_view stage)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "project authority transition requires a session reopen");
  error.with_context("transition_stage", std::string(stage));
  error.with_context("recovery", "reopen the session before running or writing");
  return error;
}

struct TrustSessionSnapshot
{
  ava::config::XdgPaths paths;
  ava::config::ModelInfo model;
  ava::agent::Mode mode = ava::agent::Mode::Build;
  std::filesystem::path workspace_dir;
  std::filesystem::path current_dir;
  runtime::PromptOverrides prompt_overrides;
  std::optional<std::string> requested_primary_agent;
  std::shared_ptr<SessionRunController> run_controller;
  std::shared_ptr<ava::session::SessionAppendTarget> append_target;
  std::shared_ptr<SubagentDeliveryManager> delivery_manager;
  std::string session_id;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

TrustSessionSnapshot trust_session_snapshot(runtime::session_ts& unlocked_session)
{
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  return {.paths = session_r->paths(),
          .model = session_r->model(),
          .mode = session_r->mode(),
          .workspace_dir = session_r->workspace_dir(),
          .current_dir = session_r->current_dir(),
          .prompt_overrides = session_r->prompt_overrides(),
          .requested_primary_agent = session_r->requested_primary_agent(),
          .run_controller = session_r->run_controller(),
          .append_target = session_r->append_target(),
          .delivery_manager = session_r->subagent_delivery_manager(),
          .session_id = session_r->store.session_id()};
}

ava::core::Result<std::optional<StagedProjectTrustMutation>> stage_trust_operation(ProjectTrustOperation operation, TrustSessionSnapshot const& snapshot)
{
  if (operation == ProjectTrustOperation::Reload)
    return std::optional<StagedProjectTrustMutation>{};
  auto staged = operation == ProjectTrustOperation::Clear
                    ? stage_clear_project_trust_decision(snapshot.paths, snapshot.workspace_dir)
                    : stage_set_project_trust_decision(snapshot.paths, snapshot.workspace_dir, operation == ProjectTrustOperation::Enable);
  if (!staged)
    return std::unexpected(std::move(staged.error()));
  return std::optional<StagedProjectTrustMutation>(std::in_place, std::move(*staged));
}

}  // namespace

ava::core::Result<ProjectTrustApplyResult> apply_project_trust_operation(runtime::session_ts& unlocked_session, ProjectTrustOperation operation)
{
  auto snapshot = trust_session_snapshot(unlocked_session);
  if (!snapshot.run_controller || !snapshot.append_target || !snapshot.delivery_manager)
    return std::unexpected(transaction_error("runtime_authority_unavailable"));

  auto trust_mutation = snapshot.delivery_manager->reserve_trust_mutation(snapshot.workspace_dir);
  if (!trust_mutation)
    return std::unexpected(std::move(trust_mutation.error()));

  auto staged = stage_trust_operation(operation, snapshot);
  if (!staged)
    return std::unexpected(std::move(staged.error()));
  auto next_trust = *staged ? (*staged)->effective_state() : load_project_trust_state(snapshot.paths, snapshot.workspace_dir);

  if (project_resources_trusted(next_trust))
  {
    auto maintenance = snapshot.run_controller->reserve_maintenance();
    if (!maintenance)
      return std::unexpected(std::move(maintenance.error()));
    if (*staged)
    {
      if (auto persisted = (*staged)->commit(); !persisted)
        return std::unexpected(std::move(persisted.error()));
      next_trust = load_project_trust_state(snapshot.paths, snapshot.workspace_dir);
    }
    auto selected_primary_agent = runtime::resolve_runtime_primary_agent(snapshot.paths, snapshot.workspace_dir, true, snapshot.requested_primary_agent,
                                                                         runtime::PrimaryAgentResolutionPolicy::AllowUnavailable);
    if (!selected_primary_agent)
      return std::unexpected(std::move(selected_primary_agent.error()));
    auto prompt_state = runtime::load_runtime_prompt_state(snapshot.paths, snapshot.model, snapshot.mode, snapshot.workspace_dir, snapshot.current_dir, true,
                                                           snapshot.prompt_overrides, *selected_primary_agent);
    if (!prompt_state)
      return std::unexpected(std::move(prompt_state.error()));
    if (auto applied =
            runtime::Session::apply_trust_prompt_state_and_refresh(unlocked_session, next_trust, std::move(*selected_primary_agent), std::move(*prompt_state));
        !applied)
      return std::unexpected(std::move(applied.error()));
    return ProjectTrustApplyResult{.state = std::move(next_trust)};
  }

  auto workspace =
      snapshot.delivery_manager->reserve_workspace_maintenance(*trust_mutation, snapshot.workspace_dir, snapshot.session_id, snapshot.run_controller);
  if (!workspace)
    return std::unexpected(std::move(workspace.error()));

  // A navigation that won before workspace reservation leaves this wrapper
  // changed. Re-snapshot under the acquired publication barrier and reject
  // before persistence rather than applying the old workspace transition.
  auto current = trust_session_snapshot(unlocked_session);
  if (current.delivery_manager != snapshot.delivery_manager || current.workspace_dir != snapshot.workspace_dir ||
      current.run_controller != snapshot.run_controller || current.append_target != snapshot.append_target || current.session_id != snapshot.session_id)
    return std::unexpected(transaction_error("session_changed_before_persistence"));

  std::optional<ava::agent::AgentDefinition> selected_primary_agent;
  runtime::PromptState prompt_state;
  std::optional<ava::core::Error> prompt_warning;
  auto resolved_primary = runtime::resolve_runtime_primary_agent(current.paths, current.workspace_dir, false, current.requested_primary_agent,
                                                                 runtime::PrimaryAgentResolutionPolicy::AllowUnavailable);
  if (!resolved_primary)
  {
    prompt_warning.emplace(std::move(resolved_primary.error()));
    prompt_state.mode = current.mode;
  }
  else
  {
    selected_primary_agent = std::move(*resolved_primary);
    auto loaded_prompt = runtime::load_runtime_prompt_state(current.paths, current.model, current.mode, current.workspace_dir, current.current_dir, false,
                                                            current.prompt_overrides, selected_primary_agent);
    if (!loaded_prompt)
    {
      prompt_warning.emplace(std::move(loaded_prompt.error()));
      selected_primary_agent.reset();
      prompt_state.mode = current.mode;
    }
    else
    {
      prompt_state = std::move(*loaded_prompt);
    }
  }

  auto fresh_controller = workspace->prepare_fresh_controller(current.append_target);
  if (!fresh_controller)
    return std::unexpected(std::move(fresh_controller.error()));

  // Everything needed for a fail-closed in-memory publication now exists. The
  // trust file is the final operation before irreversible retirement.
  if (*staged)
  {
    if (auto persisted = (*staged)->commit(); !persisted)
      return std::unexpected(std::move(persisted.error()));
  }
  workspace->mark_persistence_committed();

  if (auto retired = workspace->retire_registered_controllers(); !retired)
  {
    workspace->fail_closed();
    return std::unexpected(std::move(retired.error()));
  }
  if (auto hook = workspace->run_before_publication_test_hook(); !hook)
  {
    workspace->fail_closed();
    return std::unexpected(std::move(hook.error()));
  }

  std::shared_ptr<SessionRunController> released_controller;
  bool publication_mismatch = false;
  try
  {
    SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
    publication_mismatch = session_w->subagent_delivery_manager() != current.delivery_manager || session_w->workspace_dir() != current.workspace_dir ||
                           session_w->run_controller() != current.run_controller || session_w->append_target() != current.append_target ||
                           session_w->store.session_id() != current.session_id;
    if (!publication_mismatch)
    {
      bool const selected_primary_is_permitted =
          selected_primary_agent && (selected_primary_agent->provenance == ava::agent::AgentDefinitionProvenance::Builtin ||
                                     selected_primary_agent->provenance == ava::agent::AgentDefinitionProvenance::Global);
      if (selected_primary_agent && !selected_primary_is_permitted)
        selected_primary_agent.reset();
      auto effective_tool_visibility = session_w->tool_visibility();
      if (selected_primary_agent && selected_primary_agent->tool_preset == ava::agent::SubagentToolPreset::ReadOnly)
        effective_tool_visibility = ava::agent::narrow_tool_visibility_to_read_only(std::move(effective_tool_visibility));

      session_w->trust_state().project_trust = next_trust;
      session_w->invocation_inputs().tool_visibility = std::move(effective_tool_visibility);
      session_w->invocation_inputs().selected_primary_agent = std::move(selected_primary_agent);
      static_cast<void>(session_w->apply_prompt_state(std::move(prompt_state)));
      released_controller = std::move(session_w->resources().run_controller);
      session_w->resources().run_controller = *fresh_controller;
    }
  }
  catch (...)
  {
    workspace->fail_closed();
    return std::unexpected(transaction_error("session_publication_failed"));
  }
  if (publication_mismatch)
  {
    workspace->fail_closed();
    return std::unexpected(transaction_error("session_changed_during_publication"));
  }
  released_controller.reset();

  if (auto committed = workspace->commit_after_publication(current.session_id); !committed)
  {
    workspace->fail_closed();
    return std::unexpected(std::move(committed.error()));
  }
  return ProjectTrustApplyResult{
      .state = std::move(next_trust), .authority_retired = true, .prompt_fail_closed = prompt_warning.has_value(), .prompt_warning = std::move(prompt_warning)};
}

ava::core::Result<CommandResult> run_trust_command(runtime::session_ts& unlocked_session, std::string_view argument)
{
  auto const args = split_command_arguments(argument);
  auto const action = args.empty() ? std::string("status") : args.front();
  if (action == "status")
    return handled_text(project_trust_summary(runtime::session_ts::rat(unlocked_session)->project_trust()));

  ProjectTrustOperation operation;
  std::string prefix;
  if (action == "project" || action == "trust" || action == "approve")
  {
    operation = ProjectTrustOperation::Enable;
    prefix = "trusted project resources";
  }
  else if (action == "deny" || action == "untrust")
  {
    operation = ProjectTrustOperation::Deny;
    prefix = "denied project resources";
  }
  else if (action == "clear")
  {
    operation = ProjectTrustOperation::Clear;
    prefix = "cleared project trust decision";
  }
  else
  {
    return handled_text("unsupported trust action: " + action + "\nsupported: status, project, deny, clear");
  }

  auto applied = apply_project_trust_operation(unlocked_session, operation);
  if (!applied)
    return std::unexpected(std::move(applied.error()));
  if (applied->prompt_warning)
    return std::unexpected(std::move(*applied->prompt_warning));
  return handled_text(std::move(prefix) + "\n" + project_trust_summary(applied->state));
}

}  // namespace ava::app
