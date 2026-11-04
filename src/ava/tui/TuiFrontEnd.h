// Application boundary and startup owner for the TUI session architecture.
#pragma once

#include "composer.h"                   // StartupOverviewSnapshot
#include "keybindings.h"                // TuiKeyBindings
#include "runtime.h"                    // TuiRuntimeOptions, TuiCatalogDelivery, TuiLocalDisplaySettingsResult
#include "ava/frontend/FrontEnd.h"
#include "utils/threading/aithreadid.h"

#include <filesystem>

namespace ava::tui {

struct TuiRuntimeOptions;

// The application owns the concrete frontend; interactive calls run on the TUI thread.
// run() owns one TuiSession; that session and its joined submission worker must
// finish before the frontend is destroyed. Presentation owners request backend
// work through these virtual operations instead of borrowing backend storage.
// Unless stated otherwise, calls are synchronous on the TUI thread. Arguments
// are borrowed for the call; implementations must copy any data retained for
// asynchronous work. Returned values own their display data. See
// docs/engineering/tui-architecture.md for the session and worker lifetimes.
class TuiFrontEnd : public frontend::FrontEnd
{
 protected:
  // TUI-thread effective bindings, initialized at construction and replaced by validated reloads.
  TuiKeyBindings key_bindings_;
  // Startup presentation/capabilities owned by this frontend; session hydration copies needed values.
  TuiRuntimeOptions options_;

 protected:
  // Construct startup presentation state for one derived application frontend.
  // Construct before a session borrows this owner; terminal initialization
  // happens later in run(), so construction performs no curses work.
  TuiFrontEnd(TuiRuntimeOptions&& options) : key_bindings_(default_key_bindings()), options_(std::move(options)) { }

  // Load display settings and append any recoverable startup failure to status_line.
  virtual void load_display_settings(std::string& status_line) = 0;
  // Return the effective cursor settings from application-owned display state.
  virtual ava::tui::terminal::CursorSettings cursor_settings() const = 0;
  // Return the application configuration path used for keybind loading.
  virtual std::filesystem::path keybinds_json_path() const = 0;

 public:
  // TUI feature owners request backend actions through these existing virtual
  // operations without gaining protected-member access. The application
  // implementations retain backend validation and state changes.

  // TUI thread: return diagram callbacks that retain the application renderer's
  // resources. Removing a UI completion notifier does not stop or join its worker.
  virtual TuiMermaidRenderBridge mermaid_bridge() const = 0;

  // Toggle the committed Build/Plan mode or return the command error.
  virtual core::VoidResult on_toggle_mode() const = 0;

  // TUI thread: update application-owned sort preferences before rebuilding selector rows.
  virtual SelectListView on_session_selector_sort_cycle() = 0;
  // TUI thread: change catalog filtering before publishing the replacement selector rows.
  virtual SelectListView on_session_selector_named_filter_toggle() = 0;
  // TUI thread: rebuild row labels from the new path-display preference, retaining backend row identity.
  virtual SelectListView on_session_selector_path_display_toggle() = 0;
  // TUI thread: rebuild the displayed catalog after changing which archived sessions are visible.
  virtual SelectListView on_session_selector_archived_filter_toggle() = 0;
  // TUI thread: rebuild row labels after changing timestamp visibility without changing session identity.
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
  // TUI thread: read the application operation without joining its worker; the returned value is a presentation copy.
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
  // TUI thread before session construction: load effective bindings so all owners
  // see one validated map; retain default bindings and return feedback on failure.
  std::string load_key_bindings();

 public:
  // TUI thread: copy effective settings for preview staging; changing the copy cannot persist configuration.
  virtual TuiLocalDisplaySettings display_settings_snapshot() const = 0;

  // TUI thread: read committed identity after a backend action, so UI reset follows the actual session transition.
  virtual std::string read_committed_session_id() const = 0;

  // TUI thread: pull pending catalog generations before updating their display values.
  virtual TuiCatalogDelivery read_catalog_delivery() = 0;

  // TUI thread: hydrate todo presentation from committed state after session changes; the returned copy owns its values.
  virtual std::vector<agent::TodoItem> read_committed_todos() const = 0;

  // TUI thread: build an owning, filtered overview once per popup open; never perform this backend read during rendering.
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
  // TUI thread after active-run cleanup, while the terminal is still usable.
  // Join application-owned discovery/summary work before the frontend can die.
  virtual void on_before_tui_shutdown() = 0;

  // Reset session-selector preferences and return the current application-owned session view.
  virtual SelectListView session_selector_view() = 0;
  // Snapshot project trust for settings presentation without exposing operations that change trust.
  virtual ProjectTrustSnapshot read_project_trust() const = 0;
  // Create queue callables bound to event_sink and the specific backend run.
  // TUI thread before launch: bind queue capabilities to this submission.
  // Returned callbacks can be used by its worker until finish/retirement.
  virtual TuiActiveRunQueues create_active_run_queues(ava::event::EventEnvelopeSink event_sink) const = 0;
  // Execute submitted with request-local context and return presentation-only output.
  // Submission worker: context owns this request's endpoints. The frontend
  // outlives the joined call; return only after backend callback scopes retire.
  virtual TuiSubmitResult on_submit(std::string const& submitted, TuiSubmitContext context) = 0;
  // TUI thread: ask the application to validate and import the path before staging its returned attachment reference.
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
  // Build the current model selector from the current application model settings.
  virtual SelectListView model_selector_view() const = 0;
  // Build reasoning choices; chained selects the staged-model footer and nullopt means unsupported.
  virtual std::optional<SelectListView> reasoning_selector_view(bool chained) const = 0;
  // Build the current scoped-model selector from the current application model settings.
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
  // TUI thread: persist a selected rule before acknowledging it; return the persistence result, leaving validation in the backend.
  virtual core::Result<TuiRememberedPermissionRule> remember_permission_rule(permissions::PermissionPrompt const& prompt,
                                                                             permissions::PermissionAction action) const = 0;
  // Launch the terminal UI from the startup options owned by this frontend.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // TUI thread: Context initialization precedes session construction; the session
  // and worker borrows retire before return. The outer guard invokes application
  // shutdown even when startup or run exits early. Returns the final exit status.
  int run();

  // TUI thread: borrow startup presentation while this frontend lives; workers
  // receive request-local copies instead of sharing this reference.
  TuiRuntimeOptions const& options() const { return options_; }

  // TUI thread: update effective bindings only between dispatch steps so key hints
  // and dispatch agree; the reference expires with this owner.
  TuiKeyBindings& key_bindings() { return key_bindings_; }
  // TUI thread: inspect current dispatch bindings; copy them into a worker request.
  TuiKeyBindings const& key_bindings() const { return key_bindings_; }

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(frontend::FrontEnd)
};

} // namespace ava::tui
