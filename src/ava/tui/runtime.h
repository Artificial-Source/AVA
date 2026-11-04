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
#include "ava/core/NoLogString.h"
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
  ava::core::NoLogString message;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiRestoredQueuedMessage
{
  ava::core::NoLogString message;
  bool steering = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiRememberedPermissionRule
{
  std::string rule_id;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned process-neutral payload passed from TUI reconciliation to the application bridge. It crosses worker
// boundaries only through bridge-owned synchronization; opaque identity/epoch reject stale results.
struct TuiMermaidRenderRequest
{
  // Caller-assigned opaque request identity retained through completion/cancellation; paired with configuration
  // epoch across threads.
  std::uint64_t identity = 0;
  // Owned application configuration identity; TUI submits and accepts only the current epoch, rejecting stale
  // worker results.
  std::uint64_t config_epoch = 0;
  // Owned diagram source moved into application admission; no TUI transcript reference reaches the worker.
  ava::core::NoLogString source;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// TUI-thread admission outcome; only Busy schedules the controller's bounded retry, while accepted/full queues
// rely on completion wakes.
enum class TuiMermaidEnqueueResult : std::uint8_t
{
  // The bridge lock was contended; retry with a bounded backoff.
  Busy,
  // Application owns the request; a completion may already exist for cached/fallback work.
  Accepted,
  // No capacity admitted this request; drain a later completion before retrying admission.
  QueueFull,
  // Current source remains fallback presentation; unchanged record is not repeatedly submitted.
  Rejected,
};

// Owned application result transferred by drain to the TUI thread. Publication precedes notification; TUI
// checks epoch/identity before attaching text to a retained fence.
struct TuiMermaidRenderCompletion
{
  // Caller-assigned opaque request identity retained through completion/cancellation; paired with configuration
  // epoch across threads.
  std::uint64_t identity = 0;
  // Owned application configuration identity; TUI submits and accepts only the current epoch, rejecting stale
  // worker results.
  std::uint64_t config_epoch = 0;
  // Application validation succeeded; false tells presentation to retain the original diagram source.
  bool accepted = false;
  // Owned accepted helper text; rejected completions carry no display text and no terminal authority.
  ava::core::NoLogString text;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Process-neutral application bridge owned by the TUI controller as a callback bundle.
// Callbacks retain application bridge lifetime; enqueue/cancel/drain avoid helper I/O
// and expose admission contention. Setup/teardown registration can wait for serialization.
// Producers publish owned completions before notifying; TUI owners are never captured.
struct TuiMermaidRenderBridge
{
  // Owned application configuration identity; TUI submits and accepts only the current epoch, rejecting stale
  // worker results.
  std::uint64_t config_epoch = 0;
  // Initial TUI-thread presentation admission flag copied from application settings; later screen settings
  // drive reconciliation.
  bool enabled = false;
  // TUI thread: transfer owned source to admission without helper I/O; Busy retains the record for a 100 ms
  // then 250 ms retry.
  std::function<TuiMermaidEnqueueResult(TuiMermaidRenderRequest)> enqueue;
  // TUI thread: best-effort cancel matching identity/epoch; false can mean contention or already retired work,
  // so completion may still arrive.
  std::function<bool(std::uint64_t identity, std::uint64_t config_epoch)> cancel;
  // TUI thread: nullopt means retry after contention or temporary allocation
  // failure; no results were consumed. An empty vector means no results.
  std::function<std::optional<std::vector<TuiMermaidRenderCompletion>>()> drain;
  // Register before submitting work; an empty callback unregisters. Notifications
  // run outside producer/bridge locks and may overlap on caller/worker threads.
  // TUI callbacks only signal shared thread-safe state and never borrow a UI owner.
  // TUI setup/teardown: an already copied notifier may run after empty registration returns. It must capture
  // only independent shared readiness and a weak waiter, publish readiness before wake, and never access a
  // retired UI owner.
  std::function<void(std::function<void()>)> set_completion_notifier{};

  AVA_DEBUG_PRINT_MEMBERS_ON
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

// Pending-review catalog transport pulled and applied at serialized boundaries.
struct TuiCatalogDelivery
{
  std::vector<SlashCommandItem> slash_commands = {};
  std::optional<std::size_t> slash_catalog_generation = std::nullopt;
  std::vector<FileReferenceItem> file_references = {};
  std::optional<std::size_t> workspace_catalog_generation = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// TUI-thread-only effective display values. This local presentation is never
// published by a backend worker or stored in backend runtime state.
struct TuiLocalDisplaySettings
{
  std::vector<ThemeOptionItem> custom_themes = {};
  bool show_images = true;
  std::size_t image_width_cells = 60;
  terminal::CursorSettings cursor{terminal::CursorStyle::Default};
  // TUI-thread display projection of application helper configuration; changing it invalidates accepted Mermaid
  // geometry.
  std::uint64_t mermaid_config_epoch = 0;
  // TUI-thread local display admission flag; process ownership and validation stay in the application bridge.
  bool mermaid_enabled = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Result of a local display load or persistence action. Applied is represented
// by the presence of this value even when every effective value is unchanged.
struct TuiLocalDisplaySettingsResult
{
  TuiLocalDisplaySettings settings;
  std::string status;
  std::vector<ava::core::NoLogString> output;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiLocalCommandResult
{
  std::vector<ava::core::NoLogString> output;
  std::vector<ToolTimelineItem> tool_timeline;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TuiSubmitResult
{
  bool quit = false;
  // True only when backend handling committed a genuine provider conversation
  // turn. Slash/shell invocation text remains local even when this is true.
  bool ordinary_turn_committed = false;
  std::vector<ava::core::NoLogString> output;
  std::vector<ToolTimelineItem> tool_timeline;
  // TUI-only request authority captured before LineResult queue aggregation.
  // Initial/queued local commands can be presented without granting their
  // RuntimeEvents transcript authority. Missing ids always fail closed.
  std::vector<std::string> ordinary_turn_request_ids;
  std::optional<TuiLocalCommandResult> local_command_result = std::nullopt;
  std::vector<ava::core::NoLogString> conversation_output;
  std::vector<ToolTimelineItem> conversation_tool_timeline;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
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
  // Immutable presentation bindings captured on the TUI thread before the
  // submit worker starts. Backend help rendering may derive request-local
  // labels from this copy but must never retain the mutable UI owner.
  TuiKeyBindings key_bindings = default_key_bindings();

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

// Narrow TUI outcome for frontend-owned display settings polls.
// Applied means the local owner delivered a fresh effective result; callers must not
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
  ava::core::NoLogString source_label;
  std::string model_label;
  std::optional<TuiBranchSummaryEligibilityCode> eligibility_code = std::nullopt;
  std::optional<TuiBranchSummaryFailureCode> failure_code = std::nullopt;
  std::optional<TuiBranchSummaryAppendCommitState> append_commit_state = std::nullopt;
  bool refresh_required = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Forward declare ava::tui::TuiFrontEnd.
class TuiFrontEnd;

struct TuiRuntimeOptions
{
  std::string app_version;
  std::vector<TranscriptItem> initial_transcript = {};
  std::vector<SlashCommandItem> slash_commands = {};
  std::optional<std::size_t> slash_catalog_generation = std::nullopt;
  std::vector<FileReferenceItem> file_references = {};
  std::optional<std::size_t> workspace_catalog_generation = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
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

[[nodiscard]] SelectListView hotkeys_select_list_view(TuiKeyBindings const& bindings, std::string footer_hint = {});
// Root settings selector with shallow section rows.
[[nodiscard]] SelectListView settings_select_list_view(ComposerSnapshot const& snapshot, TuiKeyBindings const& bindings, std::string footer_hint = {});
[[nodiscard]] SelectListView settings_select_list_view(ComposerSnapshot const& snapshot, std::string footer_hint = {});
[[nodiscard]] SelectListView settings_select_list_view_for_section(TuiFrontEnd* frontend, SettingsSection section, ComposerSnapshot const& snapshot,
                                                                   TuiKeyBindings const& bindings, std::string footer_hint = {});
// Read-only host-owned startup overview list. Enter closes without backend mutation.
[[nodiscard]] SelectListView overview_select_list_view(StartupOverviewSnapshot const& overview, std::string footer_hint = {});
[[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> question_answer_from_prompt_view(QuestionPromptView const& prompt);

}  // namespace ava::tui
