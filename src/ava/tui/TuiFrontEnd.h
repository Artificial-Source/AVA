// Application boundary and startup owner for the TUI session architecture.
#pragma once

#include "BranchSummaryActions.h"
#include "SelectorInputActions.h"
#include "SessionSelectorActions.h"
#include "SettingsDialogActions.h"
#include "TuiSessionActions.h"
#include "composer.h"                   // StartupOverviewSnapshot
#include "keybindings.h"                // TuiKeyBindings
#include "runtime.h"                    // TuiRuntimeOptions, TuiCatalogDelivery, TuiLocalDisplaySettingsResult
#include "ava/frontend/FrontEnd.h"
#include "ava/app/ExitCode.h"
#include "utils/threading/EventLoop.h"
#include "utils/threading/aithreadid.h"

#include <array>
#include <filesystem>
#include <memory>

namespace ava::tui {

struct TuiRuntimeOptions;

// The application owns the concrete frontend; interactive calls run on the TUI thread.
// run() owns one TuiSession; that session and its joined submission worker must
// finish before the frontend is destroyed. Presentation owners request backend
// work through narrow feature interfaces instead of borrowing backend storage.
// Unless stated otherwise, calls are synchronous on the TUI thread. Arguments
// are borrowed for the call; implementations must copy any data retained for
// asynchronous work. Returned values own their display data. See
// docs/engineering/tui-architecture.md for the session and worker lifetimes.
class TuiFrontEnd : public frontend::FrontEnd,
                    public TuiSessionActions,
                    public SettingsDialogActions,
                    public SessionSelectorActions,
                    public BranchSummaryActions,
                    public SelectorInputActions
{
 protected:
  // TUI-thread effective bindings, initialized at construction and replaced by validated reloads.
  TuiKeyBindings key_bindings_;
  // Startup presentation/capabilities owned by this frontend; session hydration copies needed values.
  TuiRuntimeOptions options_;

 private:
  // The event loop of the TUI thread. Created with the frontend so that it exists before run() first waits on it
  // and after the TUI thread left it, which terminate() relies on. Shared only so that wakeup tokens held by
  // producers can lock it for the duration of one notification; nothing else owns it.
  std::shared_ptr<utils::threading::EventLoop> event_loop_;

 protected:
  // Construct startup presentation state for one derived application frontend.
  // Construct before a session borrows this owner; terminal initialization
  // happens later in run(), so construction performs no curses work.
  TuiFrontEnd(TuiRuntimeOptions&& options)
      : key_bindings_(default_key_bindings()), options_(std::move(options)), event_loop_(std::make_shared<utils::threading::EventLoop>())
  {
  }

  // Load display settings and append any recoverable startup failure to status_line.
  virtual void load_display_settings(std::string& status_line) = 0;
  // Return the effective cursor settings from application-owned display state.
  virtual ava::tui::terminal::CursorSettings cursor_settings() const = 0;
  // Return the application configuration path used for keybind loading.
  virtual std::filesystem::path keybinds_json_path() const = 0;

 protected:
  // Feature owners receive the corresponding narrow interface. Keep these concrete
  // declarations protected; the existing application overrides still validate and
  // perform every action. Public interface upcasts are explicit, not a security boundary.

  // TUI thread: return diagram callbacks that retain the application renderer's
  // resources. Removing a UI completion notifier does not stop or join its worker.
  virtual TuiMermaidRenderBridge mermaid_bridge() const override = 0;

  // Toggle the committed Build/Plan mode or return the command error.
  virtual core::VoidResult on_toggle_mode() const override = 0;

  // TUI thread: update application-owned sort preferences before rebuilding selector rows.
  virtual SelectListView on_session_selector_sort_cycle() override = 0;
  // TUI thread: change catalog filtering before publishing the replacement selector rows.
  virtual SelectListView on_session_selector_named_filter_toggle() override = 0;
  // TUI thread: rebuild row labels from the new path-display preference, retaining backend row identity.
  virtual SelectListView on_session_selector_path_display_toggle() override = 0;
  // TUI thread: rebuild the displayed catalog after changing which archived sessions are visible.
  virtual SelectListView on_session_selector_archived_filter_toggle() override = 0;
  // TUI thread: rebuild row labels after changing timestamp visibility without changing session identity.
  virtual SelectListView on_session_selector_label_timestamp_toggle() override = 0;
  // Archive session_id and return the refreshed selector, or a command/catalog error.
  virtual core::Result<SelectListView> on_session_selector_archive(std::string_view session_id) override = 0;
  // Unarchive session_id and return the refreshed selector, or a command/catalog error.
  virtual core::Result<SelectListView> on_session_selector_unarchive(std::string_view session_id) override = 0;
  // Open the parent of session_id or return validation, lookup, open, or replacement errors.
  virtual core::Result<std::string> on_session_selector_branch_parent(std::string_view session_id) override = 0;
  // Open the selected child of session_id or return validation, lookup, open, or replacement errors.
  virtual core::Result<std::string> on_session_selector_branch_child(std::string_view session_id) override = 0;
  // Prepare a summary for session_id and return its path-free operation snapshot.
  virtual core::Result<TuiBranchSummarySnapshot> on_branch_summary_prepare(std::string_view session_id) const override = 0;
  // TUI thread: read the application operation without joining its worker; the returned value is a presentation copy.
  virtual TuiBranchSummarySnapshot branch_summary_snapshot() const override = 0;
  // Confirm generation; false means it was not confirmable and errors preserve backend detail boundaries.
  virtual core::Result<bool> on_branch_summary_confirm(std::uint64_t generation) const override = 0;
  // Cancel generation; false means it was not cancelable and errors preserve backend detail boundaries.
  virtual core::Result<bool> on_branch_summary_cancel(std::uint64_t generation) const override = 0;
  // Refresh the session catalog after summary completion or return its refresh error.
  virtual core::Result<SelectListView> on_branch_summary_refresh_catalog() override = 0;

  // Apply settings token value and return user-facing status or command error.
  virtual core::Result<std::string> on_settings_selected(std::string_view value) override = 0;
  // Resolve and commit provider/model value and return user-facing status or resolution error.
  virtual core::Result<std::string> on_model_selected(std::string_view value) override = 0;
  // Commit level, where nullopt restores provider default, and return user-facing status.
  virtual core::Result<std::string> on_reasoning_selected(std::optional<std::string> level) override = 0;
  // Fork from public user entry_id and return committed-session status or a truthful failure.
  virtual core::Result<std::string> on_fork_user_turn_selected(std::string_view entry_id) override = 0;
  // Re-read public user entry_id text at action time or return session storage errors.
  virtual core::Result<std::string> on_read_user_turn_text(std::string_view entry_id) const override = 0;
  // Toggle scoped model value against previous and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_toggled(SelectListView const& previous, std::string_view value) const override = 0;
  // Enable visible scoped-model values and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_enable_all(SelectListView const& previous, std::vector<std::string> values) const override = 0;
  // Clear visible scoped-model values and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_clear_all(SelectListView const& previous, std::vector<std::string> values) const override = 0;
  // Toggle the provider represented by value and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_toggle_provider(SelectListView const& previous, std::string_view value) const override = 0;
  // Reorder value up or down and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_reorder(SelectListView const& previous, std::string_view value, bool up) const override = 0;
  // Persist the staged scoped-model cycle and return user-facing status or persistence error.
  virtual core::Result<std::string> on_scoped_model_save() const override = 0;
  // Open session value and return user-facing status or validation/open/replacement errors.
  virtual core::Result<std::string> on_session_selected(std::string_view value) override = 0;

  // TUI thread: copy only the three labels needed by the Settings Model section.
  // Called through SettingsDialogActions; refresh order matches normal model display.
  std::array<std::string, 3> read_settings_model_labels() override;

 private:
  // TUI thread before session construction: load effective bindings so all owners
  // see one validated map; retain default bindings and return feedback on failure.
  std::string load_key_bindings();

 public:
  // TUI thread: copy effective settings for preview staging; changing the copy cannot persist configuration.
  virtual TuiLocalDisplaySettings display_settings_snapshot() const = 0;

  // TUI thread: read committed identity after a backend action, so UI reset follows the actual session transition.
  virtual std::string read_committed_session_id() const override = 0;

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
  virtual core::Result<TuiLocalDisplaySettingsResult> on_local_display_settings_action(std::string_view action) override = 0;

  // TUI thread after active-run cleanup, while the terminal is still usable:
  // cancel and join application-owned discovery/summary workers that can notify
  // this runtime before the frontend and its notification targets can die.
  virtual void on_before_tui_shutdown() = 0;

  // Reset session-selector preferences and return the current application-owned session view.
  virtual SelectListView session_selector_view() override = 0;
  // Snapshot project trust for settings presentation without exposing operations that change trust.
  virtual ProjectTrustSnapshot read_project_trust() const override = 0;
  // TUI thread before launch: create queue callbacks bound to event_sink and
  // this backend submission. Its worker may use them until finish/retirement.
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
  virtual core::Result<TuiKeyBindings> on_reload_key_bindings() const override = 0;
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
  // Launch the terminal UI from the startup options owned by this frontend and run the TUI thread's event loop.
  // Called on the TUI thread by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // The caller must have checked that stdin and stdout are terminals. This function returns only after
  // another thread called terminate(); when the TUI wants to stop (or fails, or throws) it calls
  // core::Application::request_shutdown() and keeps waiting for that. Context initialization precedes
  // session construction; the session and worker borrows retire before return. Returns the exit code for main.
  app::ExitCode run();
  // Main thread: make run() leave its event loop.
  //
  // Blocks until the TUI thread acknowledged the termination; after that it dispatches no more events.
  // Must not be called from the TUI thread. Calling it before run() started is fine: run() then leaves
  // its loop at the first wait. Calling it again returns immediately.
  void terminate() override;

  // TUI thread: borrow startup presentation while this frontend lives; workers
  // receive request-local copies instead of sharing this reference.
  TuiRuntimeOptions const& options() const { return options_; }

  // TUI thread between dispatch steps: replace a validated map in the existing
  // object so borrowed const references remain valid. RuntimePresentationState
  // coordinates this operation with refreshing the displayed hints.
  void apply_key_bindings(TuiKeyBindings bindings);
  // TUI thread: borrow effective bindings until frontend destruction; updates
  // replace their value without changing the object address. Workers receive copies.
  TuiKeyBindings const& key_bindings() const override { return key_bindings_; }

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(frontend::FrontEnd)
};

} // namespace ava::tui
