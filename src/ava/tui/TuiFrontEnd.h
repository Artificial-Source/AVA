#pragma once

#include "composer.h"                   // StartupOverviewSnapshot
#include "keybindings.h"                // TuiKeyBindings
#include "runtime.h"                    // TuiRuntimeOptions, TuiCatalogDelivery, TuiLocalDisplaySettingsResult
#include "ava/frontend/FrontEnd.h"

#include <filesystem>

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public frontend::FrontEnd
{
 protected:
  TuiKeyBindings key_bindings_;
  TuiRuntimeOptions options_;
  // Construct startup presentation state for one derived application frontend.
  TuiFrontEnd(TuiRuntimeOptions&& options) : key_bindings_(default_key_bindings()), options_(std::move(options)) { }

  // Load display settings and append any recoverable startup failure to status_line.
  virtual void load_display_settings(std::string& status_line) = 0;
  // Return the effective cursor settings from application-owned display state.
  virtual ava::tui::terminal::CursorSettings cursor_settings() const = 0;
  // Return a process-neutral callable bridge to application-owned Mermaid rendering.
  virtual TuiMermaidRenderBridge mermaid_bridge() const = 0;
  // Return the application configuration path used for keybind loading.
  virtual std::filesystem::path keybinds_json_path() const = 0;

  // Toggle the committed Build/Plan mode or return the command error.
  virtual core::VoidResult on_toggle_mode() const = 0;

  // Advance session sort and return the refreshed selector projection.
  virtual SelectListView on_session_selector_sort_cycle() = 0;
  // Toggle named-only filtering and return the refreshed selector projection.
  virtual SelectListView on_session_selector_named_filter_toggle() = 0;
  // Toggle path display and return the refreshed selector projection.
  virtual SelectListView on_session_selector_path_display_toggle() = 0;
  // Toggle archived filtering and return the refreshed selector projection.
  virtual SelectListView on_session_selector_archived_filter_toggle() = 0;
  // Toggle label timestamps and return the refreshed selector projection.
  virtual SelectListView on_session_selector_label_timestamp_toggle() = 0;
  // Archive session_id and return the refreshed selector, or a command/catalog error.
  virtual core::Result<SelectListView> on_session_selector_archive(std::string_view session_id) = 0;
  // Unarchive session_id and return the refreshed selector, or a command/catalog error.
  virtual core::Result<SelectListView> on_session_selector_unarchive(std::string_view session_id) = 0;
  // Open the parent of session_id or return validation, lookup, open, or replacement errors.
  virtual core::Result<std::string> on_session_selector_branch_parent(std::string_view session_id) = 0;
  // Open the selected child of session_id or return validation, lookup, open, or replacement errors.
  virtual core::Result<std::string> on_session_selector_branch_child(std::string_view session_id) = 0;
  // Prepare a summary for session_id and return its path-free operation snapshot.
  virtual core::Result<TuiBranchSummarySnapshot> on_branch_summary_prepare(std::string_view session_id) const = 0;
  // Read the current path-free branch-summary operation snapshot.
  virtual TuiBranchSummarySnapshot branch_summary_snapshot() const = 0;
  // Confirm generation; false means it was not confirmable and errors preserve backend detail boundaries.
  virtual core::Result<bool> on_branch_summary_confirm(std::uint64_t generation) const = 0;
  // Cancel generation; false means it was not cancelable and errors preserve backend detail boundaries.
  virtual core::Result<bool> on_branch_summary_cancel(std::uint64_t generation) const = 0;
  // Refresh the session catalog after summary completion or return its refresh error.
  virtual core::Result<SelectListView> on_branch_summary_refresh_catalog() = 0;

  // Apply settings token value and return user-facing status or command error.
  virtual core::Result<std::string> on_settings_selected(std::string_view value) = 0;
  // Resolve and commit provider/model value and return user-facing status or resolution error.
  virtual core::Result<std::string> on_model_selected(std::string_view value) = 0;
  // Commit level, where nullopt restores provider default, and return user-facing status.
  virtual core::Result<std::string> on_reasoning_selected(std::optional<std::string> level) = 0;
  // Fork from public user entry_id and return committed-session status or a truthful failure.
  virtual core::Result<std::string> on_fork_user_turn_selected(std::string_view entry_id) = 0;
  // Re-read public user entry_id text at action time or return session storage errors.
  virtual core::Result<std::string> on_read_user_turn_text(std::string_view entry_id) const = 0;
  // Toggle scoped model value against previous and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_toggled(SelectListView const& previous, std::string_view value) const = 0;
  // Enable visible scoped-model values and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_enable_all(SelectListView const& previous, std::vector<std::string> values) const = 0;
  // Clear visible scoped-model values and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_clear_all(SelectListView const& previous, std::vector<std::string> values) const = 0;
  // Toggle the provider represented by value and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_toggle_provider(SelectListView const& previous, std::string_view value) const = 0;
  // Reorder value up or down and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_reorder(SelectListView const& previous, std::string_view value, bool up) const = 0;
  // Persist the staged scoped-model cycle and return user-facing status or persistence error.
  virtual core::Result<std::string> on_scoped_model_save() const = 0;
  // Open session value and return user-facing status or validation/open/replacement errors.
  virtual core::Result<std::string> on_session_selected(std::string_view value) = 0;

 private:
  // Load key bindings for startup and return an empty string or the formatted load error.
  std::string load_key_bindings();
  // Run the internal composer loop; only run() exposes this operation to callers.
  [[nodiscard]] int run_interactive_composer();

 public:
  // Return a value snapshot of current application-owned display settings.
  virtual TuiLocalDisplaySettings display_settings_snapshot() const = 0;

  // Return the currently committed application session identity.
  virtual std::string read_committed_session_id() const = 0;

  // Pull pending catalog generations at serialized boundaries, never while holding the UI lock.
  virtual TuiCatalogDelivery read_catalog_delivery() = 0;

  // Return the latest committed todo projection without exposing backend storage authority.
  virtual std::vector<agent::TodoItem> read_committed_todos() const = 0;

  // Return an owning, filtered overview DTO once per popup open; never call during rendering.
  virtual StartupOverviewSnapshot read_startup_overview() const = 0;

  // Reload and apply display settings, returning the effective presentation or error.
  virtual core::Result<TuiLocalDisplaySettingsResult> on_reload_display_settings() = 0;

  // Optional local result is the explicit applied/unchanged signal from the frontend owner:
  // nullopt = unchanged, some(result) = applied. unexpected = terminal apply/watch failure.
  // The TUI action layer maps this to DisplaySettingsReloadPollOutcome and must not compare values.
  virtual core::Result<std::optional<TuiLocalDisplaySettingsResult>> on_maybe_reload_display_settings() = 0;

  // Persists and applies a recognized display command or settings action on the TUI thread.
  virtual core::Result<TuiLocalDisplaySettingsResult> on_local_display_settings_action(std::string_view action) = 0;

  // Runs after the composer loop exits while curses is still active. Application-scoped
  // workers that can notify this runtime must be canceled and joined here.
  virtual void on_before_tui_shutdown() = 0;

  // Reset session-selector preferences and return the current application-owned session view.
  virtual SelectListView session_selector_view() = 0;
  // Snapshot project trust for settings presentation without retaining backend authority.
  virtual ProjectTrustSnapshot read_project_trust() const = 0;
  // Create queue callables bound to event_sink and exact per-run application authority.
  virtual TuiActiveRunQueues create_active_run_queues(ava::event::EventEnvelopeSink event_sink) const = 0;
  // Execute submitted with request-local context and return presentation-only output.
  virtual TuiSubmitResult on_submit(std::string const& submitted, TuiSubmitContext context) = 0;
  // Import path as an image attachment or return a validated path/import error.
  virtual core::Result<session::ImageAttachmentRef> on_attach_image(std::string const& path) const = 0;
  // Import a clipboard image; nullopt means no supported image was available.
  virtual core::Result<std::optional<session::ImageAttachmentRef>> on_paste_clipboard_image() const = 0;
  // Edit initial_text through the application editor integration; nullopt means canceled.
  virtual core::Result<std::optional<std::string>> on_external_editor(std::string_view initial_text) const = 0;
  // Load attachment bytes from application-owned storage or return an attachment error.
  virtual core::Result<session::LoadedImageAttachment> on_load_image_attachment(session::ImageAttachmentRef const& attachment) const = 0;
  // Cycle committed reasoning configuration and return user-facing status.
  virtual core::Result<std::string> on_cycle_reasoning() const = 0;
  // Cycle the committed model forward or backward and return user-facing status.
  virtual core::Result<std::string> on_cycle_model(bool forward) = 0;
  // Load and validate application keybindings without mutating TUI state.
  virtual core::Result<TuiKeyBindings> on_reload_key_bindings() const = 0;
  // Build the current model selector from application model authority.
  virtual SelectListView model_selector_view() const = 0;
  // Build reasoning choices; chained selects the staged-model footer and nullopt means unsupported.
  virtual std::optional<SelectListView> reasoning_selector_view(bool chained) const = 0;
  // Build the current scoped-model selector from application model authority.
  virtual SelectListView scoped_model_selector_view() const = 0;
  // List path-free subagent rows or return the backend snapshot error.
  virtual core::Result<SelectListView> list_subagents() const = 0;
  // Inspect job_id after known_generation; a not-modified frame avoids retransmitting messages.
  virtual core::Result<std::shared_ptr<agent::SubagentInspectorFrame const>> inspect_subagent(std::string_view job_id,
                                                                                              std::optional<std::uint64_t> known_generation) const = 0;
  // Request cancellation for job_id and return only the narrow TUI outcome.
  virtual SubagentWorkspaceCancelOutcome cancel_subagent(std::string_view job_id) const = 0;
  // Request promotion for job_id and return only the narrow TUI outcome.
  virtual SubagentWorkspacePromoteOutcome promote_subagent(std::string_view job_id) const = 0;
  // Open the fork source picker seeded with initial_query or return session/listing errors.
  virtual core::Result<SelectListView> on_open_fork_user_turn_selector(std::string_view initial_query) const = 0;
  // Open the copy source picker seeded with initial_query or return listing errors.
  virtual core::Result<SelectListView> on_open_copy_user_turn_selector(std::string_view initial_query) const = 0;
  // Persist action for prompt and return the narrow remembered-rule receipt or storage error.
  virtual core::Result<TuiRememberedPermissionRule> remember_permission_rule(permissions::PermissionPrompt const& prompt,
                                                                             permissions::PermissionAction action) const = 0;
  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run();

  // Access immutable startup presentation data owned by this frontend.
  TuiRuntimeOptions const& options() const { return options_; }

  // Runtime keybindings are mutable only on the serialized TUI thread.
  TuiKeyBindings& key_bindings() { return key_bindings_; }
  TuiKeyBindings const& key_bindings() const { return key_bindings_; }

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(frontend::FrontEnd)
};

} // namespace ava::tui
