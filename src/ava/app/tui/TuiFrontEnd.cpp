#include "sys.h"
#include "TuiFrontEnd.h"
#include "command_palette_views.h"      // project_slash_command_items, project_file_reference_items
#include "interactive_tui_internal.h"
#include "reasoning_selector.h"
#include "startup_overview.h"           // build_startup_overview_snapshot
#include "subagent_workspace.h"
#include "ava/app/branch_summary_coordinator.h"
#include "ava/app/clipboard_image.h"
#include "ava/app/command_format.h"
#include "ava/app/command_jobs.h"
#include "ava/app/command_palette.h"    // SessionSelectorSort
#include "ava/app/command_sessions.h"
#include "ava/app/commands.h"
#include "ava/app/interactive_run_queue.h"
#include "ava/app/plugin_ui_capability.h"
#include "ava/app/reasoning_controls.h"
#include "ava/app/rpc/runtime_navigation.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/session_title_coordinator.h"
#include "ava/app/session_user_turns.h"
#include "ava/agent/todo.h"             // agent::latest_committed_todowrite_snapshot
#include "ava/config/model_profiles.h"
#include "ava/session/attachments.h"
#include "ava/provider/catalog.h"
#include "ava/core/ids.h"

#include <functional>
#include <type_traits>

namespace ava::app {
namespace {

// Append one nonempty startup status line while preserving existing diagnostics.
void append_status_line(std::string& target, std::string line)
{
  if (line.empty())
    return;
  if (!target.empty())
    target += '\n';
  target += std::move(line);
}

// Compose the stable selector controls footer; preference parameters are reserved for richer hints.
std::string session_selector_footer_hint(SessionSelectorSort sort, bool named_only, bool show_paths, bool show_archived, bool show_label_time)
{
  static_cast<void>(sort);
  static_cast<void>(named_only);
  static_cast<void>(show_paths);
  static_cast<void>(show_archived);
  static_cast<void>(show_label_time);
  return "↑↓ navigate · Enter open · type filter · Esc close · Ctrl+D archive";
}

// Convert application invocation authority to its path-free TUI binding DTO.
tui::TuiPluginUiBinding tui_plugin_ui_binding(PluginUiInvocationBinding const& binding)
{
  return {.plugin_id = binding.plugin_id, .command = binding.command_name, .invocation_id = binding.invocation_id};
}

// Convert one typed application plugin request to its bounded TUI presentation DTO.
tui::TuiPluginUiRequest tui_plugin_ui_request(PluginUiPresentationRequest const& request)
{
  tui::TuiPluginUiRequest converted{.binding = tui_plugin_ui_binding(request.binding),
                                    .request_id = std::string(plugin::plugin_ui_request_id(request.request)),
                                    .kind = tui::TuiPluginUiKind::Status,
                                    .text = {},
                                    .title = {},
                                    .description = {},
                                    .lines = {},
                                    .options = {}};
  std::visit(
      [&converted](auto const& value) {
        using Request = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Request, plugin::PluginUiStatusRequest>)
        {
          converted.kind = tui::TuiPluginUiKind::Status;
          converted.text = value.text;
        }
        else if constexpr (std::is_same_v<Request, plugin::PluginUiWidgetRequest>)
        {
          converted.kind = tui::TuiPluginUiKind::Widget;
          converted.title = value.title;
          converted.lines = value.lines;
        }
        else if constexpr (std::is_same_v<Request, plugin::PluginUiSelectRequest>)
        {
          converted.kind = tui::TuiPluginUiKind::Select;
          converted.title = value.title;
          converted.description = value.description;
          converted.options.reserve(value.choices.size());
          for (auto const& choice : value.choices)
            converted.options.push_back({.id = choice.id, .label = choice.label, .description = choice.description});
        }
        else
        {
          converted.kind = tui::TuiPluginUiKind::Confirm;
          converted.title = value.title;
          converted.description = value.description;
        }
      },
      request.request);
  return converted;
}

// Convert one TUI reply back to the narrow application plugin action.
plugin::PluginUiAction plugin_ui_action(tui::TuiPluginUiReply reply)
{
  switch (reply.action)
  {
    case tui::TuiPluginUiReplyKind::Ack:
      return {.action = plugin::PluginUiActionKind::Ack, .option_id = {}};
    case tui::TuiPluginUiReplyKind::Select:
      return {.action = plugin::PluginUiActionKind::Select, .option_id = std::move(reply.option_id)};
    case tui::TuiPluginUiReplyKind::Confirm:
      return {.action = plugin::PluginUiActionKind::Confirm, .option_id = {}};
    case tui::TuiPluginUiReplyKind::Cancel:
      return {.action = plugin::PluginUiActionKind::Cancel, .option_id = {}};
  }
  return {.action = plugin::PluginUiActionKind::Cancel, .option_id = {}};
}

} // namespace

// Retains exact per-run queue and jobs authority for callbacks that may outlive create_active_run_queues.
struct TuiFrontEnd::ActiveRunQueueState
{
  std::shared_ptr<InteractiveRunQueue> queue;
  JobsCommandBinding jobs_binding;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Retains one immutable plugin endpoint for the lifetime of its minted invocation capability.
struct TuiFrontEnd::PluginUiEndpointState
{
  tui::TuiPluginUiEndpoint endpoint;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Borrows on_submit locals only during the synchronous queued-follow-up drain.
struct TuiFrontEnd::QueuedFollowUpState
{
  runtime::session_ts& unlocked_session;
  permissions::PermissionResolver permission_resolver;
  tui::TuiSubmitContext& context;
  std::vector<CommandHotkey> const& request_hotkeys;
  interactive_internal::TuiRequestPresentation& presentation;
  bool initial_is_local_command;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Bind Session/catalog authority and create the frontend-owned branch-summary coordinator.
TuiFrontEnd::TuiFrontEnd(runtime::session_ts& unlocked_session, ApplicationCatalogCache&& application_catalog_cache, std::size_t title_catalog_cursor,
                         tui::TuiRuntimeOptions&& options)
    : tui::TuiFrontEnd(std::move(options)),
      unlocked_session_(unlocked_session),
      display_settings_(LocalTuiDisplaySettings::create(unlocked_session)),
      application_catalog_(std::move(application_catalog_cache), title_catalog_cursor)
{
  auto created = BranchSummaryCoordinator::create();
  if (!created)
    throw std::move(created.error());
  branch_summary_coordinator_ = std::move(*created);
}

// Project the current catalog and application-owned selector preferences without backend I/O.
tui::SelectListView TuiFrontEnd::session_selector_view_from_catalog() const
{
  return application_catalog_session_view(application_catalog_, session_selector_sort_,
                                          session_selector_footer_hint(session_selector_sort_, session_selector_named_only_, session_selector_show_paths_,
                                                                       session_selector_show_archived_, session_selector_show_label_time_),
                                          session_selector_named_only_, session_selector_show_paths_, session_selector_show_archived_,
                                          session_selector_show_label_time_,
                                          ava::tui::keys_display(key_bindings_, ava::tui::TuiAction::SessionSummarizeParent));
}

// Resolve this Session's keybinds.json path from immutable runtime paths.
std::filesystem::path TuiFrontEnd::keybinds_json_path() const
{
  auto const paths = runtime::session_ts::crat(unlocked_session_)->paths();
  return paths.ava_config_dir / "keybinds.json";
}

// Load display settings while preserving startup through a formatted status on failure.
void TuiFrontEnd::load_display_settings(std::string& status_line)
{
  if (auto loaded = display_settings_.reload({}); !loaded)
    append_status_line(status_line, loaded.error().format());
}

// Return the effective application-owned cursor presentation.
tui::terminal::CursorSettings TuiFrontEnd::cursor_settings() const
{
  return display_settings_.presentation().cursor;
}

// Return one value snapshot of effective application-owned display presentation.
tui::TuiLocalDisplaySettings TuiFrontEnd::display_settings_snapshot() const
{
  return display_settings_.presentation();
}

// Return the callable bridge bound to the application-owned Mermaid worker.
tui::TuiMermaidRenderBridge TuiFrontEnd::mermaid_bridge() const
{
  return display_settings_.mermaid_bridge();
}

// Copy the committed Session identity under one brief read guard.
std::string TuiFrontEnd::read_committed_session_id() const
{
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
  return session_r->store.options_r()->session_id;
}

// Deliver pending catalog generations as frontend-safe presentation items.
tui::TuiCatalogDelivery TuiFrontEnd::read_catalog_delivery()
{
  auto delivery = application_catalog_.delivery_snapshot();
  return tui::TuiCatalogDelivery{.slash_commands = project_slash_command_items(std::move(delivery.slash_commands)),
                                 .slash_catalog_generation = delivery.slash_catalog_generation,
                                 .file_references = project_file_reference_items(std::move(delivery.file_references)),
                                 .workspace_catalog_generation = delivery.workspace_catalog_generation};
}

// Reconstruct latest committed todos; unavailable history intentionally yields an empty list.
std::vector<agent::TodoItem> TuiFrontEnd::read_committed_todos() const
{
  auto read_authority = runtime::session_ts::crat(unlocked_session_)->read_authority_1();
  if (!read_authority)
    return {};
  auto entries = read_authority->load();
  if (!entries)
    return {};
  auto snapshot = agent::latest_committed_todowrite_snapshot(*entries);
  return std::move(snapshot.todos);
}

// Aggregate one owning startup overview from bounded in-memory Session facts.
tui::StartupOverviewSnapshot TuiFrontEnd::read_startup_overview() const
{
  // The builder holds one Session read guard while performing only bounded,
  // in-memory safe aggregation and returns an owning path-filtered DTO.

  // Static empty bindings keep backend-only overview construction free of
  // mutable frontend preferences while avoiding a per-snapshot allocation.
  static tui::TuiKeyBindings const kNoBindings;
  static tui::TuiThemeInfo const kNoTheme{.kind = tui::TuiThemeKind::Dark, .name = {}, .detail = {}, .badge = {}, .palette = {}, .revision = {}};
  return build_startup_overview_snapshot(unlocked_session_, kNoBindings, kNoTheme);
}

// Reload and apply display configuration, preserving the detailed configuration error.
core::Result<tui::TuiLocalDisplaySettingsResult> TuiFrontEnd::on_reload_display_settings()
{
  return display_settings_.reload();
}

// Poll watched display configuration; nullopt reports an unchanged file set.
core::Result<std::optional<tui::TuiLocalDisplaySettingsResult>> TuiFrontEnd::on_maybe_reload_display_settings()
{
  return display_settings_.maybe_reload();
}

// Persist and apply one validated local display action.
core::Result<tui::TuiLocalDisplaySettingsResult> TuiFrontEnd::on_local_display_settings_action(std::string_view action)
{
  return display_settings_.apply_action(action);
}

// Join application workers while the runtime and its notification targets still exist.
void TuiFrontEnd::on_before_tui_shutdown()
{
  branch_summary_coordinator_->shutdown();
  display_settings_.shutdown();
}

// Capture changes after the catalog cursor without mutating the catalog.
SessionTitleCatalogChanges TuiFrontEnd::capture_title_catalog_changes() const
{
  auto const cursor = application_catalog_.title_catalog_cursor();
  auto coordinator = runtime::session_ts::rat(unlocked_session_)->session_title_coordinator();
  return coordinator ? coordinator->catalog_changes_since(cursor) : SessionTitleCatalogChanges{.cursor = cursor};
}

// Refresh the presentation catalog from title changes while preserving its prior snapshot on error.
core::Result<bool> TuiFrontEnd::refresh_title_catalog()
{
  if (!runtime::session_ts::rat(unlocked_session_)->session_title_coordinator())
    return false;
  return application_catalog_.refresh_title_changes(unlocked_session_, capture_title_catalog_changes());
}

// Refresh session-tree and title data as one catalog operation.
core::Result<bool> TuiFrontEnd::refresh_session_tree_catalog()
{
  return application_catalog_.refresh_session_tree_and_consume_title_changes(unlocked_session_, capture_title_catalog_changes());
}

// Refresh lightweight branch/title presentation after a completed backend action.
void TuiFrontEnd::settle_backend_action()
{
  static_cast<void>(runtime::refresh_git_branch(unlocked_session_));
  static_cast<void>(refresh_title_catalog());
}

// Open session_id into replacement authority without committing it.
core::Result<runtime::session_ts> TuiFrontEnd::open_requested_session(std::string_view session_id) const
{
  auto context = runtime::session_ts::rat(unlocked_session_)->replacement_open_context({});
  runtime::SessionLifecycleRequest request;
  request.requested_session_id = std::string(session_id);
  return runtime::Session::open(context, request);
}

// Validate, commit, and reconcile one selector target or return its open/replacement error.
core::Result<std::string> TuiFrontEnd::open_session_selector_target(std::string target_session_id, std::string status_prefix)
{
  if (target_session_id.empty())
    return std::unexpected(core::Error(core::ErrorCategory::InvalidArgument, "session branch target is missing session id"));
  auto const current_session_id = runtime::session_ts::rat(unlocked_session_)->store.options_r()->session_id;
  if (target_session_id == current_session_id)
  {
    application_catalog_.retarget_session(current_session_id);
    settle_backend_action();
    return status_prefix + target_session_id + " (already open)";
  }
  auto opened = open_requested_session(target_session_id);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  if (auto replaced = runtime::Session::replace_with(unlocked_session_, *opened); !replaced)
    return std::unexpected(std::move(replaced.error()));
  application_catalog_.retarget_session(runtime::session_ts::rat(unlocked_session_)->store.options_r()->session_id);
  application_catalog_.refresh_values(unlocked_session_);
  settle_backend_action();
  return status_prefix + target_session_id;
}

// Resolve the selected parent or child relationship and open its exact target.
core::Result<std::string> TuiFrontEnd::open_selector_branch(std::string_view selected_session_id, bool parent)
{
  if (auto refreshed = refresh_title_catalog(); !refreshed)
    return std::unexpected(std::move(refreshed.error()));
  if (selected_session_id.empty())
    return std::unexpected(core::Error(core::ErrorCategory::InvalidArgument, "session branch navigation is missing session id"));
  auto target = parent ? application_catalog_.parent_target(selected_session_id)
                       : application_catalog_.child_target(selected_session_id, session_selector_sort_, session_selector_show_archived_);
  if (!target)
    return std::unexpected(std::move(target.error()));
  if (!*target)
  {
    auto error = core::Error(core::ErrorCategory::NotFound, parent ? "selected session has no parent branch" : "selected session has no child branch");
    error.with_context("session_id", std::string(selected_session_id));
    return std::unexpected(std::move(error));
  }
  return open_session_selector_target(std::move(**target), parent ? "opened parent branch " : "opened child branch ");
}

// Refresh available title changes and return the current selector projection.
tui::SelectListView TuiFrontEnd::session_selector_snapshot()
{
  static_cast<void>(refresh_title_catalog());
  return session_selector_view_from_catalog();
}

// Reset application-owned selector preferences before opening a fresh selector.
tui::SelectListView TuiFrontEnd::session_selector_view()
{
  session_selector_sort_ = ava::app::SessionSelectorSort::Recent;
  session_selector_named_only_ = false;
  session_selector_show_paths_ = false;
  session_selector_show_archived_ = false;
  session_selector_show_label_time_ = false;
  return session_selector_snapshot();
}

// Copy current trust state into the path-filtered frontend DTO.
tui::ProjectTrustSnapshot TuiFrontEnd::read_project_trust() const
{
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
  return interactive_internal::project_trust_snapshot(session_r->project_trust());
}

// Queue message on state-captured run authority; no current Session state is consulted.
//static
core::VoidResult TuiFrontEnd::queue_steering(std::shared_ptr<ActiveRunQueueState> const& state, std::string message)
{
  return state->queue->queue_steering(std::move(message));
}

// Queue follow-up on state-captured run authority; no current Session state is consulted.
//static
core::VoidResult TuiFrontEnd::queue_follow_up(std::shared_ptr<ActiveRunQueueState> const& state, std::string message)
{
  return state->queue->queue_follow_up(std::move(message));
}

// Drain steering from state-captured run authority and propagate queue errors.
//static
core::Result<std::vector<std::string>> TuiFrontEnd::take_steering_messages(std::shared_ptr<ActiveRunQueueState> const& state)
{
  return state->queue->take_steering_messages();
}

// Skip steering on state-captured run authority with the supplied bounded reason.
//static
core::VoidResult TuiFrontEnd::skip_active_steering(std::shared_ptr<ActiveRunQueueState> const& state, std::string_view reason)
{
  return state->queue->skip_active_steering(reason);
}

// Convert the next state-captured application follow-up to the TUI transport DTO.
//static
std::optional<tui::TuiQueuedFollowUp> TuiFrontEnd::take_next_follow_up(std::shared_ptr<ActiveRunQueueState> const& state)
{
  auto next = state->queue->take_next_follow_up();
  if (!next)
    return std::nullopt;
  return tui::TuiQueuedFollowUp{.request_id = next->request_id, .message = next->message};
}

// Mark the exact TUI follow-up started using its request id for both queue identities.
//static
core::VoidResult TuiFrontEnd::mark_follow_up_started(std::shared_ptr<ActiveRunQueueState> const& state, tui::TuiQueuedFollowUp const& follow_up)
{
  return state->queue->mark_follow_up_started(
      InteractiveQueuedMessage{.request_id = follow_up.request_id, .correlation_id = follow_up.request_id, .message = follow_up.message});
}

// Restore the latest message from state-captured run authority and preserve recovery errors.
//static
core::Result<tui::TuiRestoredQueuedMessage> TuiFrontEnd::restore_latest(std::shared_ptr<ActiveRunQueueState> const& state)
{
  auto restored = state->queue->restore_latest();
  if (!restored)
    return std::unexpected(std::move(restored.error()));
  return tui::TuiRestoredQueuedMessage{.message = restored->message, .steering = restored->steering};
}

// Execute recognized /jobs input against the jobs authority captured with this run.
//static
std::optional<std::vector<std::string>> TuiFrontEnd::run_nonblocking_command(std::shared_ptr<ActiveRunQueueState> const& state, std::string const& submitted)
{
  auto arguments = active_jobs_command_arguments(submitted);
  if (!arguments)
    return std::nullopt;
  auto command = run_jobs_command(state->jobs_binding, *arguments, true);
  if (!command)
    return std::vector<std::string>{command.error().format()};
  return std::move(command->output);
}

// Finish the state-captured queue with the active run's cancellation outcome.
//static
core::VoidResult TuiFrontEnd::finish_active_run_queue(std::shared_ptr<ActiveRunQueueState> const& state, bool canceled)
{
  return state->queue->finish(canceled);
}

// Build callbacks around one shared state object so every callback retains exact run authority.
tui::TuiActiveRunQueues TuiFrontEnd::create_active_run_queues(event::EventEnvelopeSink event_sink) const
{
  auto jobs_binding = capture_jobs_command_binding(unlocked_session_);
  auto queue = std::make_shared<InteractiveRunQueue>(jobs_binding.parent_session_id, core::make_id("request"), std::move(event_sink));
  auto state = std::make_shared<ActiveRunQueueState>(ActiveRunQueueState{.queue = queue, .jobs_binding = std::move(jobs_binding)});
  return {.active_request_id = queue->active_request_id(),
          .queue_steering = std::bind_front(&TuiFrontEnd::queue_steering, state),
          .queue_follow_up = std::bind_front(&TuiFrontEnd::queue_follow_up, state),
          .take_steering_messages = std::bind_front(&TuiFrontEnd::take_steering_messages, state),
          .skip_active_steering = std::bind_front(&TuiFrontEnd::skip_active_steering, state),
          .take_next_follow_up = std::bind_front(&TuiFrontEnd::take_next_follow_up, state),
          .mark_follow_up_started = std::bind_front(&TuiFrontEnd::mark_follow_up_started, state),
          .restore_latest = std::bind_front(&TuiFrontEnd::restore_latest, state),
          .run_nonblocking_command = std::bind_front(&TuiFrontEnd::run_nonblocking_command, state),
          .finish = std::bind_front(&TuiFrontEnd::finish_active_run_queue, state)};
}

// Present through the endpoint retained for this invocation and convert its typed reply.
//static
core::Result<plugin::PluginUiAction> TuiFrontEnd::present_plugin_ui(std::shared_ptr<PluginUiEndpointState> const& state,
                                                                    PluginUiPresentationRequest const& request, std::chrono::steady_clock::time_point deadline,
                                                                    plugin::CancelCallback cancel_requested)
{
  return plugin_ui_action(state->endpoint.present(tui_plugin_ui_request(request), deadline, std::move(cancel_requested)));
}

// Close only the endpoint retained for this invocation and exact plugin binding.
//static
void TuiFrontEnd::close_plugin_ui(std::shared_ptr<PluginUiEndpointState> const& state, PluginUiInvocationBinding const& binding)
{
  state->endpoint.close(tui_plugin_ui_binding(binding));
}

// Read committed identity for the synchronous transition check without retaining the callback.
//static
std::string TuiFrontEnd::queued_follow_up_session_id(QueuedFollowUpState const& state)
{
  return runtime::session_ts::rat(state.unlocked_session)->store.options_r()->session_id;
}

// Execute one queued follow-up using borrowed on_submit context and append its presentation projection.
//static
interactive_internal::InteractiveResult TuiFrontEnd::run_queued_follow_up(QueuedFollowUpState& state, tui::TuiQueuedFollowUp const& follow_up)
{
  interactive_internal::InteractiveState interactive_state{state.unlocked_session};
  auto result = interactive_internal::handle_interactive_submission(
      interactive_state, follow_up.message, state.permission_resolver, state.context.question_resolver, state.request_hotkeys, state.context.event_sink,
      state.context.cancel_requested, state.context.take_steering_messages, {}, follow_up.request_id, state.context.on_subagent_launch, nullptr);
  interactive_internal::capture_tui_request_presentation(state.presentation, state.initial_is_local_command, follow_up.message, follow_up.request_id, result);
  return result;
}

// Execute one submitted line, reconcile backend catalogs, and return its TUI-only presentation projection.
tui::TuiSubmitResult TuiFrontEnd::on_submit(std::string const& submitted, tui::TuiSubmitContext context)
{
  auto const request_hotkeys = interactive_internal::command_hotkeys_from_key_bindings(context.key_bindings);
  auto const permission_rule_store = runtime::session_ts::rat(unlocked_session_)->permission_rule_store();
  auto permission_resolver = permissions::build_persistent_permission_rule_resolver(permission_rule_store, context.permission_resolver);
  auto const session_id_before = runtime::session_ts::rat(unlocked_session_)->store.options_r()->session_id;
  bool workspace_catalog_reload = interactive_internal::workspace_catalog_reload_requested(submitted);

  std::shared_ptr<PluginUiInvocationCapability> plugin_ui_capability;
  bool plugin_ui_capability_unavailable = submitted.starts_with("/plugin run ");
  if (context.plugin_ui && *context.plugin_ui)
  {
    auto endpoint = std::make_shared<PluginUiEndpointState>(PluginUiEndpointState{.endpoint = *context.plugin_ui});
    auto minted = make_tui_plugin_ui_invocation_capability(submitted, context.request_id, context.plugin_ui->deadline, context.plugin_ui->runtime_token,
                                                           std::bind_front(&TuiFrontEnd::present_plugin_ui, endpoint),
                                                           std::bind_front(&TuiFrontEnd::close_plugin_ui, endpoint));
    if (minted)
    {
      plugin_ui_capability = std::move(*minted);
      plugin_ui_capability_unavailable = false;
    }
  }

  interactive_internal::InteractiveResult line_result;
  if (plugin_ui_capability_unavailable)
  {
    interactive_internal::add_output(line_result, "plugin UI capability is unavailable");
  }
  else
  {
    interactive_internal::InteractiveState state{unlocked_session_};
    line_result = interactive_internal::handle_interactive_submission(
        state, submitted, permission_resolver, context.question_resolver, request_hotkeys, context.event_sink, context.cancel_requested,
        context.take_steering_messages, std::move(context.image_attachments), context.request_id, context.on_subagent_launch, std::move(plugin_ui_capability));
  }

  interactive_internal::TuiRequestPresentation request_presentation;
  bool const initial_is_local_command = submitted.starts_with('/') || submitted.starts_with('!');
  interactive_internal::capture_tui_request_presentation(request_presentation, initial_is_local_command, submitted, context.request_id, line_result);
  QueuedFollowUpState follow_ups{.unlocked_session = unlocked_session_,
                                 .permission_resolver = permission_resolver,
                                 .context = context,
                                 .request_hotkeys = request_hotkeys,
                                 .presentation = request_presentation,
                                 .initial_is_local_command = initial_is_local_command};
  auto const session_changed = interactive_internal::run_queued_follow_ups_until_session_transition(
      line_result, workspace_catalog_reload, session_id_before, context, std::bind_front(&TuiFrontEnd::queued_follow_up_session_id, std::cref(follow_ups)),
      std::bind_front(&TuiFrontEnd::run_queued_follow_up, std::ref(follow_ups)));

  bool const workspace_changed = workspace_catalog_reload || interactive_internal::workspace_catalog_changed(line_result);
  if (workspace_changed)
    application_catalog_.refresh_workspace(unlocked_session_);
  if (line_result.session_tree_changed)
  {
    if (auto refreshed = refresh_session_tree_catalog(); !refreshed)
      interactive_internal::add_output(line_result, refreshed.error().format());
  }
  else if (session_changed)
  {
    application_catalog_.retarget_session(runtime::session_ts::rat(unlocked_session_)->store.options_r()->session_id);
    application_catalog_.refresh_values(unlocked_session_);
  }
  else if (line_result.ordinary_turn_committed)
  {
    bool current_refreshed = false;
    if (runtime::session_ts::rat(unlocked_session_)->session_title_coordinator())
    {
      auto refreshed = refresh_title_catalog();
      if (!refreshed)
        interactive_internal::add_output(line_result, refreshed.error().format());
      else
        current_refreshed = *refreshed;
    }
    if (!current_refreshed)
    {
      auto refreshed = application_catalog_.refresh_current_session(unlocked_session_);
      if (!refreshed)
        interactive_internal::add_output(line_result, refreshed.error().format());
    }
  }
  else if (!workspace_changed)
  {
    application_catalog_.refresh_values(unlocked_session_);
  }

  std::optional<tui::TuiLocalCommandResult> local_command_result;
  if (request_presentation.has_local_command)
  {
    local_command_result =
        tui::TuiLocalCommandResult{.output = std::move(request_presentation.local_command.output),
                                   .tool_timeline = interactive_internal::tui_tool_timeline(request_presentation.local_command.tool_timeline)};
  }
  settle_backend_action();
  return {.quit = line_result.quit,
          .ordinary_turn_committed = line_result.ordinary_turn_committed,
          .output = line_result.output,
          .tool_timeline = interactive_internal::tui_tool_timeline(line_result.tool_timeline),
          .ordinary_turn_request_ids = std::move(request_presentation.ordinary_turn_request_ids),
          .local_command_result = std::move(local_command_result),
          .conversation_output = std::move(request_presentation.conversation.output),
          .conversation_tool_timeline = interactive_internal::tui_tool_timeline(request_presentation.conversation.tool_timeline)};
}

// Resolve path against Session directories and import it into the attachment store.
core::Result<session::ImageAttachmentRef> TuiFrontEnd::on_attach_image(std::string const& path) const
{
  auto source = std::filesystem::path(path);
  if (source.empty())
    return std::unexpected(core::Error(core::ErrorCategory::InvalidArgument, "usage: /attach <image-path>"));
  if (!source.is_absolute())
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
    auto const base = session_r->current_dir().empty() ? session_r->workspace_dir() : session_r->current_dir();
    source = base / source;
  }
  SCOPED_CRITICAL_AREA_W(session_w, unlocked_session_);
  return session::import_image_attachment(session_w->store, source);
}

// Capture store/process authority together and import without later Session reads.
core::Result<std::optional<session::ImageAttachmentRef>> TuiFrontEnd::on_paste_clipboard_image() const
{
  std::optional<std::pair<session::SessionStore, std::optional<process::ProcessScopeV1>>> authority;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
    authority.emplace(session_r->store, session_r->session_process_scope());
  }
  return import_clipboard_image_attachment(authority->first, authority->second);
}

// Delegate initial_text to the application external-editor integration.
core::Result<std::optional<std::string>> TuiFrontEnd::on_external_editor(std::string_view initial_text) const
{
  return interactive_internal::edit_text_with_external_editor(initial_text);
}

// Load attachment bytes from the current Session store under one read guard.
core::Result<session::LoadedImageAttachment> TuiFrontEnd::on_load_image_attachment(session::ImageAttachmentRef const& attachment) const
{
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
  return session::load_image_attachment(session_r->store, attachment);
}

// Toggle committed mode through the canonical command path.
core::VoidResult TuiFrontEnd::on_toggle_mode() const
{
  auto result = run_command(unlocked_session_, CommandRequest{.command = "/mode"});
  if (!result)
    return std::unexpected(std::move(result.error()));
  return {};
}

// Cycle reasoning through canonical runtime controls.
core::Result<std::string> TuiFrontEnd::on_cycle_reasoning() const
{
  return cycle_runtime_reasoning(unlocked_session_);
}

// Resolve and commit the adjacent model in the requested direction.
core::Result<std::string> TuiFrontEnd::on_cycle_model(bool forward)
{
  auto model =
      forward ? rpc::next_runtime_model(runtime::session_ts::rat(unlocked_session_)) : rpc::previous_runtime_model(runtime::session_ts::rat(unlocked_session_));
  if (!model)
    return std::unexpected(std::move(model.error()));
  auto switched = runtime::Session::switch_model_and_refresh(unlocked_session_, std::move(*model));
  if (!switched)
    return std::unexpected(std::move(switched.error()));
  settle_backend_action();
  return *switched ? "model cycled" : "model already selected";
}

// Load validated keybindings from this Session's configuration path.
core::Result<tui::TuiKeyBindings> TuiFrontEnd::on_reload_key_bindings() const
{
  auto loaded = tui::load_key_bindings(keybinds_json_path());
  if (!loaded)
    return std::unexpected(std::move(loaded.error()));
  return std::move(*loaded);
}

// Build the current application model selector.
tui::SelectListView TuiFrontEnd::model_selector_view() const
{
  return model_selector_view_1(unlocked_session_, "Enter switch model · type to filter · Esc cancel");
}

// Build reasoning choices with footer semantics selected by chained.
std::optional<tui::SelectListView> TuiFrontEnd::reasoning_selector_view(bool chained) const
{
  return ava::app::reasoning_selector_view(unlocked_session_,
                                           chained ? "Enter select · type to filter · Esc keep default" : "Enter select · type to filter · Esc cancel");
}

// Build the staged scoped-model selector from current runtime model state.
tui::SelectListView TuiFrontEnd::scoped_model_selector_view() const
{
  return scoped_model_selector_view_1(unlocked_session_, interactive_internal::scoped_model_selector_footer_hint());
}

// List path-free subagent rows for the committed Session identity.
core::Result<tui::SelectListView> TuiFrontEnd::list_subagents() const
{
  std::shared_ptr<agent::SubagentCoordinator> coordinator;
  std::string session_id;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
    coordinator = session_r->subagent_coordinator();
    session_id = session_r->store.options_r()->session_id;
  }
  return subagent_selector_view(coordinator, session_id);
}

// Inspect job_id for the committed Session after known_generation.
core::Result<std::shared_ptr<agent::SubagentInspectorFrame const>> TuiFrontEnd::inspect_subagent(std::string_view job_id,
                                                                                                 std::optional<std::uint64_t> known_generation) const
{
  std::shared_ptr<agent::SubagentCoordinator> coordinator;
  std::string session_id;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
    coordinator = session_r->subagent_coordinator();
    session_id = session_r->store.options_r()->session_id;
  }
  if (!coordinator)
    return std::unexpected(core::Error(core::ErrorCategory::NotFound, "subagent workspace is unavailable"));
  return coordinator->inspect(session_id, job_id, known_generation);
}

// Request cancellation for job_id using committed subagent authority.
tui::SubagentWorkspaceCancelOutcome TuiFrontEnd::cancel_subagent(std::string_view job_id) const
{
  std::shared_ptr<agent::SubagentCoordinator> coordinator;
  std::string session_id;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
    coordinator = session_r->subagent_coordinator();
    session_id = session_r->store.options_r()->session_id;
  }
  return cancel_subagent_for_workspace(coordinator, session_id, job_id);
}

// Request promotion for job_id using committed subagent authority.
tui::SubagentWorkspacePromoteOutcome TuiFrontEnd::promote_subagent(std::string_view job_id) const
{
  std::shared_ptr<agent::SubagentCoordinator> coordinator;
  std::string session_id;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
    coordinator = session_r->subagent_coordinator();
    session_id = session_r->store.options_r()->session_id;
  }
  return promote_subagent_for_workspace(coordinator, session_id, job_id);
}

// Validate persistent-session authority and build the fork picker seeded by initial_query.
core::Result<tui::SelectListView> TuiFrontEnd::on_open_fork_user_turn_selector(std::string_view initial_query) const
{
  if (auto allowed = require_persistent_session_for_fork_from(unlocked_session_); !allowed)
    return std::unexpected(std::move(allowed.error()));
  return user_turn_selector_view(unlocked_session_, "Fork from user turn", "Enter fork · type to filter · Esc cancel", std::string(initial_query));
}

// Build the copy picker seeded by initial_query.
core::Result<tui::SelectListView> TuiFrontEnd::on_open_copy_user_turn_selector(std::string_view initial_query) const
{
  return user_turn_selector_view(unlocked_session_, "Copy user turn", "Enter copy · type to filter · Esc cancel", std::string(initial_query));
}

// Advance sort preference and rebuild the selector.
tui::SelectListView TuiFrontEnd::on_session_selector_sort_cycle()
{
  session_selector_sort_ = next_session_selector_sort(session_selector_sort_);
  return session_selector_snapshot();
}

// Toggle named-only filtering and rebuild the selector.
tui::SelectListView TuiFrontEnd::on_session_selector_named_filter_toggle()
{
  session_selector_named_only_ = !session_selector_named_only_;
  return session_selector_snapshot();
}

// Toggle path display and rebuild the selector.
tui::SelectListView TuiFrontEnd::on_session_selector_path_display_toggle()
{
  session_selector_show_paths_ = !session_selector_show_paths_;
  return session_selector_snapshot();
}

// Toggle archived filtering and rebuild the selector.
tui::SelectListView TuiFrontEnd::on_session_selector_archived_filter_toggle()
{
  session_selector_show_archived_ = !session_selector_show_archived_;
  return session_selector_snapshot();
}

// Toggle label timestamps and rebuild the selector.
tui::SelectListView TuiFrontEnd::on_session_selector_label_timestamp_toggle()
{
  session_selector_show_label_time_ = !session_selector_show_label_time_;
  return session_selector_snapshot();
}

// Archive session_id through canonical commands and refresh changed catalog data.
core::Result<tui::SelectListView> TuiFrontEnd::on_session_selector_archive(std::string_view session_id)
{
  auto archived = run_command(unlocked_session_, CommandRequest{.command = "/sessions archive " + std::string(session_id) + " --confirm"});
  if (!archived)
    return std::unexpected(std::move(archived.error()));
  if (archived->session_tree_changed)
  {
    if (auto refreshed = refresh_session_tree_catalog(); !refreshed)
      return std::unexpected(std::move(refreshed.error()));
  }
  return session_selector_snapshot();
}

// Unarchive session_id through canonical commands and refresh changed catalog data.
core::Result<tui::SelectListView> TuiFrontEnd::on_session_selector_unarchive(std::string_view session_id)
{
  auto unarchived = run_command(unlocked_session_, CommandRequest{.command = "/sessions unarchive " + std::string(session_id)});
  if (!unarchived)
    return std::unexpected(std::move(unarchived.error()));
  if (unarchived->session_tree_changed)
  {
    if (auto refreshed = refresh_session_tree_catalog(); !refreshed)
      return std::unexpected(std::move(refreshed.error()));
  }
  return session_selector_snapshot();
}

// Resolve and open the parent branch of session_id.
core::Result<std::string> TuiFrontEnd::on_session_selector_branch_parent(std::string_view session_id)
{
  return open_selector_branch(session_id, true);
}

// Resolve and open the selected child branch of session_id.
core::Result<std::string> TuiFrontEnd::on_session_selector_branch_child(std::string_view session_id)
{
  return open_selector_branch(session_id, false);
}

// Prepare a branch-summary operation from catalog source session_id.
core::Result<tui::TuiBranchSummarySnapshot> TuiFrontEnd::on_branch_summary_prepare(std::string_view session_id) const
{
  auto source = application_catalog_.session_summary(session_id);
  if (!source)
    return std::unexpected(std::move(source.error()));
  if (!*source)
    return std::unexpected(core::Error(core::ErrorCategory::NotFound, "selected parent session is unavailable"));
  auto request = make_branch_summary_operation_request(unlocked_session_, std::move(**source));
  if (!request)
    return std::unexpected(std::move(request.error()));
  auto prepared = branch_summary_coordinator_->prepare(std::move(*request));
  if (!prepared)
    return std::unexpected(std::move(prepared.error()));
  auto operation = branch_summary_coordinator_->snapshot();
  if (operation.generation != *prepared)
    return std::unexpected(core::Error(core::ErrorCategory::Unknown, "parent summary generation is unavailable"));
  return interactive_internal::tui_branch_summary_snapshot(operation);
}

// Project the coordinator's current operation into the path-free TUI snapshot.
tui::TuiBranchSummarySnapshot TuiFrontEnd::branch_summary_snapshot() const
{
  return interactive_internal::tui_branch_summary_snapshot(branch_summary_coordinator_->snapshot());
}

// Confirm the exact branch-summary generation.
core::Result<bool> TuiFrontEnd::on_branch_summary_confirm(std::uint64_t generation) const
{
  return branch_summary_coordinator_->confirm(generation);
}

// Cancel the exact branch-summary generation.
core::Result<bool> TuiFrontEnd::on_branch_summary_cancel(std::uint64_t generation) const
{
  return branch_summary_coordinator_->cancel(generation);
}

// Refresh the session tree after branch-summary completion.
core::Result<tui::SelectListView> TuiFrontEnd::on_branch_summary_refresh_catalog()
{
  if (auto refreshed = refresh_session_tree_catalog(); !refreshed)
    return std::unexpected(std::move(refreshed.error()));
  return session_selector_view_from_catalog();
}

// Persist action for prompt through the application permission-rule store.
core::Result<tui::TuiRememberedPermissionRule> TuiFrontEnd::remember_permission_rule(permissions::PermissionPrompt const& prompt,
                                                                                     permissions::PermissionAction action) const
{
  return interactive_internal::remember_permission_rule_for_prompt(unlocked_session_, prompt, action);
}

// Apply one settings token and reconcile backend presentation.
core::Result<std::string> TuiFrontEnd::on_settings_selected(std::string_view value)
{
  if (value == "settings:keybindings.validate")
  {
    auto validated = run_command(unlocked_session_, CommandRequest{.command = "/keybindings validate"});
    if (!validated)
      return std::unexpected(std::move(validated.error()));
    auto status = validated->output.empty() ? std::string("keybindings validation complete") : validated->output.front();
    settle_backend_action();
    return status;
  }
  constexpr std::string_view trust_prefix = "settings:trust.";
  if (value.starts_with(trust_prefix))
  {
    auto trusted = run_command(unlocked_session_, CommandRequest{.command = "/trust " + std::string(value.substr(trust_prefix.size()))});
    if (!trusted)
      return std::unexpected(std::move(trusted.error()));
    application_catalog_.refresh_values(unlocked_session_);
    auto status = trusted->output.empty() ? std::string("trust action complete") : trusted->output.front();
    settle_backend_action();
    return status;
  }
  settle_backend_action();
  return "view closed";
}

// Resolve provider/model value and commit the selected runtime model.
core::Result<std::string> TuiFrontEnd::on_model_selected(std::string_view value)
{
  auto const separator = value.find('/');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= value.size())
    return std::unexpected(core::Error(core::ErrorCategory::InvalidArgument, "model selection is missing provider/model"));
  auto provider_catalog = runtime::session_ts::crat(unlocked_session_)->provider_catalog();
  auto const paths = runtime::session_ts::crat(unlocked_session_)->paths();
  auto model = resolve_runtime_model(paths, std::move(provider_catalog), value.substr(0, separator), value.substr(separator + 1));
  if (!model)
    return std::unexpected(std::move(model.error()));
  auto switched = runtime::Session::switch_model_and_refresh(unlocked_session_, std::move(*model));
  if (!switched)
    return std::unexpected(std::move(switched.error()));
  settle_backend_action();
  return *switched ? "model switched" : "model already selected";
}

// Commit level or restore provider default when level is nullopt.
core::Result<std::string> TuiFrontEnd::on_reasoning_selected(std::optional<std::string> level)
{
  if (!level)
  {
    auto changed = runtime::Session::set_reasoning_and_refresh(unlocked_session_, std::nullopt);
    if (!changed)
      return std::unexpected(std::move(changed.error()));
    settle_backend_action();
    return *changed ? "thinking mode set to Default" : "thinking mode already Default";
  }
  config::ModelInfo const model = runtime::session_ts::rat(unlocked_session_)->model_selection_r()->model;
  auto selection = reasoning_selection_for_level(model, *level);
  if (!selection)
    return std::unexpected(std::move(selection.error()));
  auto changed = runtime::Session::set_reasoning_and_refresh(unlocked_session_, std::move(*selection));
  if (!changed)
    return std::unexpected(std::move(changed.error()));
  auto const label = reasoning_level_label(*level);
  settle_backend_action();
  return *changed ? "thinking mode set to " + label : "thinking mode already " + label;
}

// Fork from public user entry_id and reconcile committed Session/catalog authority.
core::Result<std::string> TuiFrontEnd::on_fork_user_turn_selected(std::string_view entry_id)
{
  if (entry_id.empty())
    return std::unexpected(core::Error(core::ErrorCategory::InvalidArgument, "fork-from selection is missing entry id"));
  if (auto allowed = require_persistent_session_for_fork_from(unlocked_session_); !allowed)
    return std::unexpected(std::move(allowed.error()));
  std::string before_session_id;
  std::string before_session_path;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
    before_session_id = session_r->store.options_r()->session_id;
    before_session_path = session_r->store.session_path().string();
  }
  auto forked = run_fork_command(unlocked_session_, {}, entry_id);
  if (!forked)
    return std::unexpected(std::move(forked.error()));
  std::string after_session_id;
  std::string after_session_path;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
    after_session_id = session_r->store.options_r()->session_id;
    after_session_path = session_r->store.session_path().string();
  }
  if (after_session_id == before_session_id && after_session_path == before_session_path)
  {
    auto message = forked->output.empty() ? std::string("fork-from did not switch sessions") : forked->output.front();
    if (auto const newline = message.find('\n'); newline != std::string::npos)
      message.erase(newline);
    auto error = core::Error(core::ErrorCategory::InvalidArgument, std::move(message));
    error.with_context("operation", "on_fork_user_turn_selected");
    error.with_context("session_id", before_session_id);
    return std::unexpected(std::move(error));
  }
  auto status = forked->output.empty() ? std::string("forked session") : forked->output.front();
  if (auto const newline = status.find('\n'); newline != std::string::npos)
    status.erase(newline);
  if (forked->session_tree_changed)
  {
    if (auto refreshed = refresh_session_tree_catalog(); !refreshed)
    {
      application_catalog_.retarget_session(after_session_id);
      application_catalog_.refresh_values(unlocked_session_);
      auto warning = sanitize_inline_text(refreshed.error().format());
      if (auto const newline = warning.find('\n'); newline != std::string::npos)
        warning.erase(newline);
      if (!status.empty())
        status += " · ";
      status += "session tree refresh deferred: " + warning;
    }
  }
  else
  {
    application_catalog_.retarget_session(after_session_id);
    application_catalog_.refresh_values(unlocked_session_);
  }
  settle_backend_action();
  return status;
}

// Read public user entry_id text from the current Session at action time.
core::Result<std::string> TuiFrontEnd::on_read_user_turn_text(std::string_view entry_id) const
{
  return read_session_user_turn_text(unlocked_session_, entry_id);
}

// Toggle scoped model value against previous and rebuild the selector.
core::Result<tui::SelectListView> TuiFrontEnd::on_scoped_model_toggled(tui::SelectListView const& previous, std::string_view value) const
{
  return interactive_internal::toggle_scoped_model(unlocked_session_, previous, value);
}

// Enable the supplied visible scoped-model values and rebuild the selector.
core::Result<tui::SelectListView> TuiFrontEnd::on_scoped_model_enable_all(tui::SelectListView const& previous, std::vector<std::string> values) const
{
  return interactive_internal::enable_scoped_models(unlocked_session_, previous, std::move(values));
}

// Clear the supplied visible scoped-model values and rebuild the selector.
core::Result<tui::SelectListView> TuiFrontEnd::on_scoped_model_clear_all(tui::SelectListView const& previous, std::vector<std::string> values) const
{
  return interactive_internal::clear_scoped_models(unlocked_session_, previous, std::move(values));
}

// Toggle the provider represented by value and rebuild the selector.
core::Result<tui::SelectListView> TuiFrontEnd::on_scoped_model_toggle_provider(tui::SelectListView const& previous, std::string_view value) const
{
  return interactive_internal::toggle_scoped_model_provider(unlocked_session_, previous, value);
}

// Reorder value in the requested direction and rebuild the selector.
core::Result<tui::SelectListView> TuiFrontEnd::on_scoped_model_reorder(tui::SelectListView const& previous, std::string_view value, bool up) const
{
  return interactive_internal::reorder_scoped_model(unlocked_session_, previous, value, up);
}

// Persist the staged scoped-model cycle.
core::Result<std::string> TuiFrontEnd::on_scoped_model_save() const
{
  return interactive_internal::save_scoped_model_cycle(unlocked_session_);
}

// Open Session value and reconcile committed catalog identity.
core::Result<std::string> TuiFrontEnd::on_session_selected(std::string_view value)
{
  if (value.empty())
    return std::unexpected(core::Error(core::ErrorCategory::InvalidArgument, "session selection is missing session id"));
  auto const current_session_id = runtime::session_ts::rat(unlocked_session_)->store.options_r()->session_id;
  if (value == current_session_id)
  {
    application_catalog_.retarget_session(current_session_id);
    settle_backend_action();
    return "session already open";
  }
  auto opened = open_requested_session(value);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  if (auto replaced = runtime::Session::replace_with(unlocked_session_, *opened); !replaced)
    return std::unexpected(std::move(replaced.error()));
  application_catalog_.retarget_session(runtime::session_ts::rat(unlocked_session_)->store.options_r()->session_id);
  application_catalog_.refresh_values(unlocked_session_);
  settle_backend_action();
  return "session opened";
}

} // namespace ava::app
