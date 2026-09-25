#pragma once

#include "ava/event/events.h"
#include "ava/agent/question.h"
#include "ava/agent/subagent_launch.h"
#include "ava/tui/composer.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime_plugin_ui.h"
#include "ava/tui/terminal.h"
#include "ava/tui/terminal/Cursor.h"
#include "ava/session/attachments.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tui {

struct TuiQueuedFollowUp
{
  std::string request_id;
  std::string message;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiRestoredQueuedMessage
{
  std::string message;
  bool steering = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiRememberedPermissionRule
{
  std::string rule_id;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiMermaidRenderRequest
{
  std::uint64_t identity = 0;
  std::uint64_t config_epoch = 0;
  std::string source;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class TuiMermaidEnqueueResult : std::uint8_t
{
  Accepted,
  QueueFull,
  Rejected,
};

struct TuiMermaidRenderCompletion
{
  std::uint64_t identity = 0;
  std::uint64_t config_epoch = 0;
  bool accepted = false;
  std::string text;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Process-neutral nonblocking application bridge. The TUI owns only opaque
// identities and bounded payload DTOs; helper execution remains entirely
// application-owned.
struct TuiMermaidRenderBridge
{
  std::uint64_t config_epoch = 0;
  bool enabled = false;
  std::function<TuiMermaidEnqueueResult(TuiMermaidRenderRequest)> enqueue;
  std::function<bool(std::uint64_t identity, std::uint64_t config_epoch)> cancel;
  std::function<std::vector<TuiMermaidRenderCompletion>()> drain;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TuiActiveRunQueues
{
  std::string active_request_id;
  std::function<ava::core::VoidResult(std::string)> queue_steering;
  std::function<ava::core::VoidResult(std::string)> queue_follow_up;
  std::function<ava::core::Result<std::vector<std::string>>()> take_steering_messages;
  std::function<ava::core::VoidResult(std::string_view)> skip_active_steering;
  std::function<std::optional<TuiQueuedFollowUp>()> take_next_follow_up;
  std::function<ava::core::VoidResult(TuiQueuedFollowUp const&)> mark_follow_up_started;
  std::function<ava::core::Result<TuiRestoredQueuedMessage>()> restore_latest;
  // Bound to the exact active session before its submit worker starts. A
  // disengaged result leaves ordinary active-run queue behavior unchanged.
  std::function<std::optional<std::vector<std::string>>(std::string const&)> run_nonblocking_command;
  std::function<ava::core::VoidResult(bool)> finish;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiRuntimeStateSnapshot
{
  std::string mode;
  std::string provider;
  std::string model;
  std::string session_id;
  std::string session_path;
  std::string workspace;
  std::string git_branch;
  std::optional<std::size_t> context_source_count = std::nullopt;
  std::string status;
  std::vector<SlashCommandItem> slash_commands = {};
  std::optional<std::size_t> slash_catalog_generation = std::nullopt;
  std::vector<FileReferenceItem> file_references = {};
  // When present, identical generations let runtime snapshots reuse the
  // existing workspace references without invalidating completion rankings.
  std::optional<std::size_t> workspace_catalog_generation = std::nullopt;
  std::vector<ThemeOptionItem> custom_themes = {};
  std::optional<ProjectTrustSnapshot> project_trust = std::nullopt;
  // Presentation hydration for the latest committed todowrite snapshot.
  std::vector<TodoItem> todos = {};
  // Effective display presentation owned by the application display document.
  bool show_images = true;
  std::size_t image_width_cells = 60;
  terminal::CursorSettings cursor{terminal::CursorStyle::Default};
  std::uint64_t mermaid_config_epoch = 0;
  bool mermaid_enabled = false;
  // Application-owned path-free startup/resources snapshot. Omitted rather than
  // rebuilt by the TUI. Never contains paths, credentials, or prompt contents.
  std::optional<StartupOverviewSnapshot> startup_overview = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiLocalCommandResult
{
  std::vector<std::string> output;
  std::vector<ToolTimelineItem> tool_timeline;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TuiSubmitResult
{
  bool quit = false;
  // True only when backend handling committed a genuine provider conversation
  // turn. Slash/shell invocation text remains local even when this is true.
  bool ordinary_turn_committed = false;
  std::vector<std::string> output;
  std::vector<ToolTimelineItem> tool_timeline;
  // TUI-only request authority captured before LineResult queue aggregation.
  // Initial/queued local commands can be presented without granting their
  // RuntimeEvents transcript authority. Missing ids always fail closed.
  std::vector<std::string> ordinary_turn_request_ids;
  std::optional<TuiLocalCommandResult> local_command_result = std::nullopt;
  std::vector<std::string> conversation_output;
  std::vector<ToolTimelineItem> conversation_tool_timeline;
  std::optional<std::size_t> context_source_count = std::nullopt;
  // Returned by submit workers so the TUI thread applies state changes before
  // accepting another prompt. This is intentionally distinct from command
  // output/status, which the TUI settles after rendering the completed turn.
  std::optional<TuiRuntimeStateSnapshot> state_snapshot = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TuiKeyBindingReloadResult
{
  TuiKeyBindings key_bindings = default_key_bindings();
  TuiRuntimeStateSnapshot state;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiSubmitContext
{
  // One exact identity for this submitted turn. The event envelope and backend
  // RunOptions must both use it; queued follow-ups replace it before dispatch.
  std::string request_id;
  ava::permissions::PermissionResolver permission_resolver;
  ava::agent::QuestionResolver question_resolver;
  ava::event::RuntimeEventSink event_sink;
  std::function<bool()> cancel_requested;
  std::function<ava::core::Result<std::vector<std::string>>()> take_steering_messages;
  std::function<ava::core::VoidResult(std::string_view)> skip_active_steering;
  std::function<std::optional<TuiQueuedFollowUp>()> take_next_follow_up;
  std::function<ava::core::VoidResult(TuiQueuedFollowUp const&)> mark_follow_up_started;
  ava::agent::SubagentLaunchSink on_subagent_launch;
  std::optional<TuiPluginUiEndpoint> plugin_ui;
  std::vector<ava::session::ImageAttachmentRef> image_attachments;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Narrow TUI outcomes for live subagent workspace controls. Mappers and
// callbacks must never surface Error::format, ids, paths, or backend text.
enum class SubagentWorkspaceCancelOutcome
{
  CancellationRequested,
  AlreadyFinished,
  CancelUnavailable,
};

enum class SubagentWorkspacePromoteOutcome
{
  CurrentlyBackground,
  AlreadyFinished,
  PromotionUnavailable,
};

// Narrow TUI outcome for application-owned display settings polls.
// Applied means the app delivered a fresh authoritative snapshot; callers must not
// infer applied/unchanged by comparing presentation values under an active overlay.
enum class DisplaySettingsReloadPollOutcome
{
  TerminalFailure,
  Unchanged,
  Applied,
};

enum class TuiBranchSummaryPhase
{
  Idle,
  Preparing,
  AwaitingConfirmation,
  Generating,
  Revalidating,
  Appending,
  Succeeded,
  Existing,
  Ineligible,
  Canceled,
  Failed,
};

enum class TuiBranchSummaryEligibilityCode
{
  CurrentSessionEphemeral,
  CurrentSessionUnavailable,
  ActiveRun,
  InvalidSourceSelection,
  NotDirectSource,
  InvalidFork,
  SourceUnavailable,
  SourceLeaseBusy,
  SourceCorrupt,
  ForkEntryNotFound,
  EmptySuffix,
};

enum class TuiBranchSummaryFailureCode
{
  Deadline,
  RecoveryFailed,
  ProjectionRecordLimit,
  ProjectionTextLimit,
  ProjectionByteLimit,
  ProjectionInvalidText,
  ProjectionEmpty,
  ModelUnavailable,
  AuthenticationUnavailable,
  ProviderFailed,
  InvalidGeneratedSummary,
  StaleSource,
  AppendFailed,
  Internal,
};

enum class TuiBranchSummaryAppendCommitState
{
  NotStarted,
  PartialOrUnknown,
  CommittedToLeasedInode,
};

// Path-free, payload-free operation state delivered from the application. The
// application bounds both labels before crossing this callback boundary.
struct TuiBranchSummarySnapshot
{
  std::uint64_t generation = 0;
  TuiBranchSummaryPhase phase = TuiBranchSummaryPhase::Idle;
  std::string source_label;
  std::string model_label;
  std::optional<TuiBranchSummaryEligibilityCode> eligibility_code = std::nullopt;
  std::optional<TuiBranchSummaryFailureCode> failure_code = std::nullopt;
  std::optional<TuiBranchSummaryAppendCommitState> append_commit_state = std::nullopt;
  bool refresh_required = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiRuntimeOptions
{
  std::string mode;
  std::string provider;
  std::string model;
  std::string session_id;
  std::string session_path;
  std::string workspace;
  std::string git_branch;
  std::string app_version;
  std::optional<std::size_t> context_source_count = std::nullopt;
  std::string initial_status = "";
  std::vector<TranscriptItem> initial_transcript = {};
  std::vector<SlashCommandItem> slash_commands = {};
  std::optional<std::size_t> slash_catalog_generation = std::nullopt;
  std::vector<FileReferenceItem> file_references = {};
  std::optional<std::size_t> workspace_catalog_generation = std::nullopt;
  std::vector<ThemeOptionItem> custom_themes = {};
  std::optional<ProjectTrustSnapshot> project_trust = std::nullopt;
  std::vector<TodoItem> initial_todos = {};
  bool show_images = true;
  std::size_t image_width_cells = 60;
  terminal::CursorSettings cursor{terminal::CursorStyle::Default};
  std::optional<StartupOverviewSnapshot> startup_overview = std::nullopt;
  TuiMermaidRenderBridge mermaid_render;
  TuiKeyBindings key_bindings = default_key_bindings();
  // Called on the TUI main thread at startup and after a submit worker completes; never from render/spinner loops.
  std::function<std::optional<std::string>()> token_status_provider;
  std::function<std::optional<std::string>()> active_context_status_provider;
  std::function<std::optional<std::string>()> reasoning_status_provider;
  std::function<TuiActiveRunQueues(ava::event::EventEnvelopeSink)> create_active_run_queues;
  std::function<TuiSubmitResult(std::string const&, TuiSubmitContext)> on_submit;
  std::function<ava::core::Result<ava::session::ImageAttachmentRef>(std::string const&)> on_attach_image;
  std::function<ava::core::Result<std::optional<ava::session::ImageAttachmentRef>>()> on_paste_clipboard_image;
  std::function<ava::core::Result<std::optional<std::string>>(std::string_view)> on_external_editor;
  std::function<ava::core::Result<ava::session::LoadedImageAttachment>(ava::session::ImageAttachmentRef const&)> on_load_image_attachment;
  std::function<ava::core::Result<std::string>()> on_toggle_mode;
  std::function<ava::core::Result<std::string>()> on_cycle_reasoning;
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>(bool)> on_cycle_model;
  std::function<ava::core::Result<TuiKeyBindingReloadResult>()> on_reload_key_bindings;
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>()> on_reload_display_settings;
  // Optional snapshot is the explicit applied/unchanged signal from the application:
  // nullopt = unchanged, some(snapshot) = applied. unexpected = terminal apply/watch failure.
  // The TUI action layer maps this to DisplaySettingsReloadPollOutcome and must not compare values.
  std::function<ava::core::Result<std::optional<TuiRuntimeStateSnapshot>>()> on_maybe_reload_display_settings;
  std::function<SelectListView()> model_selector_view;
  // The bool is true for the staged model-selection handoff, allowing an
  // accurate "Esc keep default" footer. No value means no configurable mode.
  std::function<std::optional<SelectListView>(bool)> reasoning_selector_view;
  std::function<SelectListView()> scoped_model_selector_view;
  std::function<SelectListView()> session_selector_view;
  // Path-free, owner-bound live subagent workspace callbacks. Exact ids are
  // accepted only as hidden selector values and callback authority. Cancel and
  // promote return narrow typed TUI outcomes only — never free-form backend text.
  std::function<ava::core::Result<SelectListView>()> list_subagents;
  std::function<ava::core::Result<std::shared_ptr<ava::agent::SubagentInspectorFrame const>>(std::string_view, std::optional<std::uint64_t>)> inspect_subagent;
  std::function<SubagentWorkspaceCancelOutcome(std::string_view)> cancel_subagent;
  std::function<SubagentWorkspacePromoteOutcome(std::string_view)> promote_subagent;
  // Open searchable public user-turn pickers. Fail closed with Result when the
  // bound session authority has no public user turns, is sessionless for fork-from,
  // or cannot list them. Optional initial_query seeds the picker filter.
  std::function<ava::core::Result<SelectListView>(std::string_view initial_query)> on_open_fork_user_turn_selector;
  std::function<ava::core::Result<SelectListView>(std::string_view initial_query)> on_open_copy_user_turn_selector;
  std::function<SelectListView()> on_session_selector_sort_cycle;
  std::function<SelectListView()> on_session_selector_named_filter_toggle;
  std::function<SelectListView()> on_session_selector_path_display_toggle;
  std::function<SelectListView()> on_session_selector_archived_filter_toggle;
  std::function<SelectListView()> on_session_selector_label_timestamp_toggle;
  std::function<ava::core::Result<SelectListView>(std::string_view)> on_session_selector_archive;
  std::function<ava::core::Result<SelectListView>(std::string_view)> on_session_selector_unarchive;
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>(std::string_view)> on_session_selector_branch_parent;
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>(std::string_view)> on_session_selector_branch_child;
  // Hidden selector values are the only session identities crossing this boundary.
  // All returned operation state is path-free and fixed-code only.
  std::function<ava::core::Result<TuiBranchSummarySnapshot>(std::string_view)> on_branch_summary_prepare;
  std::function<TuiBranchSummarySnapshot()> branch_summary_snapshot;
  std::function<ava::core::Result<bool>(std::uint64_t)> on_branch_summary_confirm;
  std::function<ava::core::Result<bool>(std::uint64_t)> on_branch_summary_cancel;
  std::function<ava::core::Result<SelectListView>()> on_branch_summary_refresh_catalog;
  std::function<ava::core::Result<TuiRememberedPermissionRule>(ava::permissions::PermissionPrompt const&, ava::permissions::PermissionAction)>
      remember_permission_rule;
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>(std::string_view)> on_settings_selected;
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>(std::string_view)> on_model_selected;
  // No value clears the explicit AVA level and restores model/provider default.
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>(std::optional<std::string>)> on_reasoning_selected;
  // Fork at the selected public user entry id through the normal branch path.
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>(std::string_view entry_id)> on_fork_user_turn_selected;
  // Re-read exact public user-turn text by stable entry id at action time.
  std::function<ava::core::Result<std::string>(std::string_view entry_id)> on_read_user_turn_text;
  std::function<ava::core::Result<SelectListView>(SelectListView const&, std::string_view)> on_scoped_model_toggled;
  std::function<ava::core::Result<SelectListView>(SelectListView const&, std::vector<std::string>)> on_scoped_model_enable_all;
  std::function<ava::core::Result<SelectListView>(SelectListView const&, std::vector<std::string>)> on_scoped_model_clear_all;
  std::function<ava::core::Result<SelectListView>(SelectListView const&, std::string_view)> on_scoped_model_toggle_provider;
  std::function<ava::core::Result<SelectListView>(SelectListView const&, std::string_view, bool)> on_scoped_model_reorder;
  std::function<ava::core::Result<std::string>()> on_scoped_model_save;
  std::function<ava::core::Result<TuiRuntimeStateSnapshot>(std::string_view)> on_session_selected;
  // Runs after the composer loop exits while curses is still active. Application-scoped
  // workers that can notify this runtime must be canceled and joined here.
  std::function<void()> on_before_tui_shutdown;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Shallow nested settings hierarchy used by /settings.
enum class SettingsSection
{
  Root,
  Theme,
  Display,
  ModelsAndReasoning,
  InputAndKeybindings,
  SessionsAndWorkspace,
  ToolsAndExtensions,
};

[[nodiscard]] int run_interactive_composer(TuiRuntimeOptions options);
[[nodiscard]] SelectListView hotkeys_select_list_view(TuiKeyBindings const& bindings, std::string footer_hint = {});
// Root settings selector with shallow section rows.
[[nodiscard]] SelectListView settings_select_list_view(ComposerSnapshot const& snapshot, TuiKeyBindings const& bindings, std::string footer_hint = {});
[[nodiscard]] SelectListView settings_select_list_view(ComposerSnapshot const& snapshot, std::string footer_hint = {});
[[nodiscard]] SelectListView settings_select_list_view_for_section(SettingsSection section, ComposerSnapshot const& snapshot, TuiKeyBindings const& bindings,
                                                                   std::string footer_hint = {});
// Read-only host-owned startup overview list. Enter closes without backend mutation.
[[nodiscard]] SelectListView overview_select_list_view(StartupOverviewSnapshot const& overview, std::string footer_hint = {});
[[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> question_answer_from_prompt_view(QuestionPromptView const& prompt);

}  // namespace ava::tui
