#pragma once

#include "ava/app/command_palette.h"    // ApplicationCatalogCoordinator, SessionSelectorSort
#include "ava/app/plugin_ui_capability.h"
#include "ava/app/tui/display_reload.h" // LocalTuiDisplaySettings
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/composer.h"           // tui::SelectListView

namespace ava::app {

class BranchSummaryCoordinator;

namespace interactive_internal {
struct InteractiveResult;
}

class TuiFrontEnd : public tui::TuiFrontEnd
{
 private:
  runtime::session_ts& unlocked_session_;
  LocalTuiDisplaySettings display_settings_;
  ApplicationCatalogCoordinator application_catalog_;
  std::shared_ptr<BranchSummaryCoordinator> branch_summary_coordinator_;
  SessionSelectorSort session_selector_sort_ = SessionSelectorSort::Recent;
  bool session_selector_named_only_ = false;
  bool session_selector_show_paths_ = false;
  bool session_selector_show_archived_ = false;
  bool session_selector_show_label_time_ = false;

  // Per-active-run authority captured before the submit worker starts; callbacks retain this state independently of later session replacement.
  struct ActiveRunQueueState;
  // Per-submission plugin endpoint retained by the minted capability without consulting mutable frontend session authority.
  struct PluginUiEndpointState;
  // Borrowed synchronous follow-up context; run_queued_follow_ups_until_session_transition never retains this state.
  struct QueuedFollowUpState;

  // Project the current catalog and selector preferences without backend I/O.
  tui::SelectListView session_selector_view_from_catalog() const;
  // Refresh title data when available and return the current session selector projection.
  tui::SelectListView session_selector_snapshot();
  // Capture title changes after the catalog's current cursor; an absent coordinator yields an empty change set at that cursor.
  SessionTitleCatalogChanges capture_title_catalog_changes() const;
  // Apply title-coordinator changes to the presentation catalog; errors preserve the prior catalog snapshot.
  core::Result<bool> refresh_title_catalog();
  // Rebuild the session tree and consume the same captured title change set; errors leave the prior selector usable.
  core::Result<bool> refresh_session_tree_catalog();
  // Refresh lightweight backend presentation after a completed application action.
  void settle_backend_action();
  // Open session_id through replacement authority without mutating the currently committed session.
  core::Result<runtime::session_ts> open_requested_session(std::string_view session_id) const;
  // Commit target_session_id and return status_prefix plus the selected identity, or an open/replace error.
  core::Result<std::string> open_session_selector_target(std::string target_session_id, std::string status_prefix);
  // Resolve and open the selected parent or child branch; missing relationships return NotFound.
  core::Result<std::string> open_selector_branch(std::string_view selected_session_id, bool parent);

  // Queue one steering message on the exact active-run queue retained by state.
  static core::VoidResult queue_steering(std::shared_ptr<ActiveRunQueueState> const& state, std::string message);
  // Queue one follow-up message on the exact active-run queue retained by state.
  static core::VoidResult queue_follow_up(std::shared_ptr<ActiveRunQueueState> const& state, std::string message);
  // Drain steering messages from the exact active-run queue retained by state.
  static core::Result<std::vector<std::string>> take_steering_messages(std::shared_ptr<ActiveRunQueueState> const& state);
  // Mark active steering skipped for reason on the exact retained queue.
  static core::VoidResult skip_active_steering(std::shared_ptr<ActiveRunQueueState> const& state, std::string_view reason);
  // Convert the next retained application follow-up into the path-free TUI DTO.
  static std::optional<tui::TuiQueuedFollowUp> take_next_follow_up(std::shared_ptr<ActiveRunQueueState> const& state);
  // Mark follow_up started on the retained queue using its request id as correlation authority.
  static core::VoidResult mark_follow_up_started(std::shared_ptr<ActiveRunQueueState> const& state, tui::TuiQueuedFollowUp const& follow_up);
  // Restore the latest retained queued message, propagating queue recovery errors.
  static core::Result<tui::TuiRestoredQueuedMessage> restore_latest(std::shared_ptr<ActiveRunQueueState> const& state);
  // Run only a recognized nonblocking /jobs command against state-captured jobs authority; nullopt means not recognized.
  static std::optional<std::vector<std::string>> run_nonblocking_command(std::shared_ptr<ActiveRunQueueState> const& state, std::string const& submitted);
  // Settle and deactivate the exact retained queue with the caller's cancellation outcome.
  static core::VoidResult finish_active_run_queue(std::shared_ptr<ActiveRunQueueState> const& state, bool canceled);
  // Present one plugin request through state-captured endpoint authority and convert its reply.
  static core::Result<plugin::PluginUiAction> present_plugin_ui(std::shared_ptr<PluginUiEndpointState> const& state, PluginUiPresentationRequest const& request,
                                                                std::chrono::steady_clock::time_point deadline, plugin::CancelCallback cancel_requested);
  // Close the state-captured plugin endpoint for the exact invocation binding.
  static void close_plugin_ui(std::shared_ptr<PluginUiEndpointState> const& state, PluginUiInvocationBinding const& binding);
  // Read the committed session identity for synchronous queued-follow-up transition detection.
  static std::string queued_follow_up_session_id(QueuedFollowUpState const& state);
  // Execute and project one synchronous queued follow-up using only state-borrowed request context.
  static interactive_internal::InteractiveResult run_queued_follow_up(QueuedFollowUpState& state, tui::TuiQueuedFollowUp const& follow_up);

 protected:
  // Load application display settings and append any formatted failure to status_line.
  void load_display_settings(std::string& status_line) override;
  // Return the effective application-owned cursor presentation.
  tui::terminal::CursorSettings cursor_settings() const override;
  // Return callbacks bound to the application-owned Mermaid worker.
  tui::TuiMermaidRenderBridge mermaid_bridge() const override;
  // Return the configured keybinds.json path for this runtime Session.
  std::filesystem::path keybinds_json_path() const override;

  // Run the canonical /mode command against this Session.
  core::VoidResult on_toggle_mode() const override;

  // Advance the application-owned session sort and rebuild the selector.
  tui::SelectListView on_session_selector_sort_cycle() override;
  // Toggle the application-owned named filter and rebuild the selector.
  tui::SelectListView on_session_selector_named_filter_toggle() override;
  // Toggle the application-owned path display and rebuild the selector.
  tui::SelectListView on_session_selector_path_display_toggle() override;
  // Toggle the application-owned archived filter and rebuild the selector.
  tui::SelectListView on_session_selector_archived_filter_toggle() override;
  // Toggle the application-owned label timestamp display and rebuild the selector.
  tui::SelectListView on_session_selector_label_timestamp_toggle() override;
  // Archive session_id through the canonical command path and refresh the catalog.
  core::Result<tui::SelectListView> on_session_selector_archive(std::string_view session_id) override;
  // Unarchive session_id through the canonical command path and refresh the catalog.
  core::Result<tui::SelectListView> on_session_selector_unarchive(std::string_view session_id) override;
  // Resolve and open the parent branch of session_id.
  core::Result<std::string> on_session_selector_branch_parent(std::string_view session_id) override;
  // Resolve and open the selected child branch of session_id.
  core::Result<std::string> on_session_selector_branch_child(std::string_view session_id) override;
  // Prepare a branch-summary operation from catalog source session_id.
  core::Result<tui::TuiBranchSummarySnapshot> on_branch_summary_prepare(std::string_view session_id) const override;
  // Project the coordinator's current operation into the path-free TUI snapshot.
  tui::TuiBranchSummarySnapshot branch_summary_snapshot() const override;
  // Confirm the exact branch-summary generation.
  core::Result<bool> on_branch_summary_confirm(std::uint64_t generation) const override;
  // Cancel the exact branch-summary generation.
  core::Result<bool> on_branch_summary_cancel(std::uint64_t generation) const override;
  // Refresh the session tree after branch-summary completion.
  core::Result<tui::SelectListView> on_branch_summary_refresh_catalog() override;

  // Apply one settings token and reconcile backend presentation.
  core::Result<std::string> on_settings_selected(std::string_view value) override;
  // Resolve provider/model value and commit the selected runtime model.
  core::Result<std::string> on_model_selected(std::string_view value) override;
  // Commit level or restore provider default when level is nullopt.
  core::Result<std::string> on_reasoning_selected(std::optional<std::string> level) override;
  // Fork from public user entry_id and reconcile committed Session/catalog authority.
  core::Result<std::string> on_fork_user_turn_selected(std::string_view entry_id) override;
  // Read public user entry_id text from the current Session at action time.
  core::Result<std::string> on_read_user_turn_text(std::string_view entry_id) const override;
  // Toggle scoped model value against previous and rebuild the selector.
  core::Result<tui::SelectListView> on_scoped_model_toggled(tui::SelectListView const& previous, std::string_view value) const override;
  // Enable the supplied visible scoped-model values and rebuild the selector.
  core::Result<tui::SelectListView> on_scoped_model_enable_all(tui::SelectListView const& previous, std::vector<std::string> values) const override;
  // Clear the supplied visible scoped-model values and rebuild the selector.
  core::Result<tui::SelectListView> on_scoped_model_clear_all(tui::SelectListView const& previous, std::vector<std::string> values) const override;
  // Toggle the provider represented by value and rebuild the selector.
  core::Result<tui::SelectListView> on_scoped_model_toggle_provider(tui::SelectListView const& previous, std::string_view value) const override;
  // Reorder value in the requested direction and rebuild the selector.
  core::Result<tui::SelectListView> on_scoped_model_reorder(tui::SelectListView const& previous, std::string_view value, bool up) const override;
  // Persist the staged scoped-model cycle.
  core::Result<std::string> on_scoped_model_save() const override;
  // Open Session value and reconcile committed catalog identity.
  core::Result<std::string> on_session_selected(std::string_view value) override;

 public:
  // Bind one runtime Session, initial catalog, title cursor, and startup presentation to the application frontend; coordinator creation errors throw
  // core::Error.
  TuiFrontEnd(runtime::session_ts& unlocked_session, ApplicationCatalogCache&& application_catalog_cache, std::size_t title_catalog_cursor,
              tui::TuiRuntimeOptions&& options);

  // Implement application integration; interaction with the backend and other AVA-specific state.

  // Return the effective application-owned display presentation.
  tui::TuiLocalDisplaySettings display_settings_snapshot() const override;
  // Read the currently committed Session identity under a brief read guard.
  std::string read_committed_session_id() const override;
  // Return pending catalog generations and path-safe presentation items.
  tui::TuiCatalogDelivery read_catalog_delivery() override;
  // Reconstruct the latest committed todo snapshot; unreadable history yields no todos.
  std::vector<agent::TodoItem> read_committed_todos() const override;
  // Build one owning startup overview from current in-memory Session facts.
  tui::StartupOverviewSnapshot read_startup_overview() const override;
  // Reload and apply display settings, returning the applied presentation or error.
  core::Result<tui::TuiLocalDisplaySettingsResult> on_reload_display_settings() override;
  // Poll watched display settings; nullopt means no change and errors retain last-good presentation.
  core::Result<std::optional<tui::TuiLocalDisplaySettingsResult>> on_maybe_reload_display_settings() override;
  // Persist and apply one validated display action or return its configuration error.
  core::Result<tui::TuiLocalDisplaySettingsResult> on_local_display_settings_action(std::string_view) override;
  // Join the branch-summary coordinator and display workers before TUI-owned state is torn down.
  void on_before_tui_shutdown() override;
  // Reset selector preferences and return the current session catalog projection.
  tui::SelectListView session_selector_view() override;
  // Copy current trust state into the path-filtered TUI DTO.
  tui::ProjectTrustSnapshot read_project_trust() const override;
  // Capture exact queue and jobs authority for event_sink and bind its TUI callable transport.
  tui::TuiActiveRunQueues create_active_run_queues(event::EventEnvelopeSink event_sink) const override;
  // Execute submitted with request-local authority and reconcile application catalogs.
  tui::TuiSubmitResult on_submit(std::string const& submitted, tui::TuiSubmitContext context) override;
  // Resolve and import path into this Session's attachment store.
  core::Result<session::ImageAttachmentRef> on_attach_image(std::string const& path) const override;
  // Import a clipboard image using Session-captured store and process authority.
  core::Result<std::optional<session::ImageAttachmentRef>> on_paste_clipboard_image() const override;
  // Delegate initial_text to the application external-editor integration.
  core::Result<std::optional<std::string>> on_external_editor(std::string_view initial_text) const override;
  // Load attachment bytes from this Session's store.
  core::Result<session::LoadedImageAttachment> on_load_image_attachment(session::ImageAttachmentRef const& attachment) const override;
  // Cycle runtime reasoning and return canonical status.
  core::Result<std::string> on_cycle_reasoning() const override;
  // Resolve and commit the adjacent model in the requested direction.
  core::Result<std::string> on_cycle_model(bool forward) override;
  // Load validated bindings from this Session's configuration path.
  core::Result<tui::TuiKeyBindings> on_reload_key_bindings() const override;
  // Build the application model selector for current Session authority.
  tui::SelectListView model_selector_view() const override;
  // Build current reasoning choices, using chained footer semantics when requested.
  std::optional<tui::SelectListView> reasoning_selector_view(bool chained) const override;
  // Build the staged scoped-model selector for current Session authority.
  tui::SelectListView scoped_model_selector_view() const override;
  // List path-free subagent rows for the committed Session identity.
  core::Result<tui::SelectListView> list_subagents() const override;
  // Inspect job_id for the committed Session after known_generation.
  core::Result<std::shared_ptr<agent::SubagentInspectorFrame const>> inspect_subagent(std::string_view job_id,
                                                                                      std::optional<std::uint64_t> known_generation) const override;
  // Request cancellation for job_id against committed subagent authority.
  tui::SubagentWorkspaceCancelOutcome cancel_subagent(std::string_view job_id) const override;
  // Request promotion for job_id against committed subagent authority.
  tui::SubagentWorkspacePromoteOutcome promote_subagent(std::string_view job_id) const override;
  // Validate persistent-session authority and build the fork source picker.
  core::Result<tui::SelectListView> on_open_fork_user_turn_selector(std::string_view initial_query) const override;
  // Build the copy source picker seeded by initial_query.
  core::Result<tui::SelectListView> on_open_copy_user_turn_selector(std::string_view initial_query) const override;
  // Persist action for prompt through the application permission-rule store.
  core::Result<tui::TuiRememberedPermissionRule> remember_permission_rule(permissions::PermissionPrompt const& prompt,
                                                                          permissions::PermissionAction action) const override;
  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(tui::TuiFrontEnd)
};

} // namespace ava::app
