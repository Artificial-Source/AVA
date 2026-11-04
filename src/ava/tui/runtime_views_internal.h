// Defines settings preview state and helpers that build owned TUI views from borrowed display data.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/runtime.h"
#include "ava/tui/terminal/Cursor.h"
#include "ava/tui/theme.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::agent {
struct QuestionAnswer;
struct QuestionPrompt;
}  // namespace ava::agent

namespace ava::permissions {
struct PermissionPrompt;
}  // namespace ava::permissions

namespace ava::tui {

struct ActiveRunHint;
struct ComposerSnapshot;
struct InputEvent;
struct PermissionPromptView;
struct QuestionPromptView;
struct SelectListView;
struct TuiKeyBindings;

namespace runtime_views {

// Stable hidden row values used by settings input routing; visible labels may change independently.
inline constexpr std::string_view kSettingsOpenKeybindings = "settings:keybindings.open";
inline constexpr std::string_view kSettingsEditKeybindings = "settings:keybindings.edit";
inline constexpr std::string_view kSettingsReloadKeybindings = "settings:keybindings.reload";
inline constexpr std::string_view kSettingsOpenModels = "settings:models.open";
inline constexpr std::string_view kSettingsOpenScopedModels = "settings:models.scoped";
inline constexpr std::string_view kSettingsOpenReasoning = "settings:reasoning.open";
inline constexpr std::string_view kSettingsImagesOn = "settings:images.on";
inline constexpr std::string_view kSettingsImagesOff = "settings:images.off";
inline constexpr std::string_view kSettingsImagesReset = "settings:images.reset";
inline constexpr std::string_view kSettingsImageWidthReset = "settings:image-width.reset";
inline constexpr std::string_view kSettingsImageWidthPrefix = "settings:image-width.";
inline constexpr std::string_view kSettingsCursorStylePrefix = "settings:cursor.style.";
inline constexpr std::string_view kSettingsCursorBlink = "settings:cursor.blink";
inline constexpr std::string_view kSettingsCursorSteady = "settings:cursor.steady";
inline constexpr std::string_view kSettingsTrustProject = "settings:trust.project";
inline constexpr std::string_view kSettingsTrustDeny = "settings:trust.deny";
inline constexpr std::string_view kSettingsTrustClear = "settings:trust.clear";
inline constexpr std::string_view kSettingsDraftPermissions = "settings:draft.permissions";
inline constexpr std::string_view kSettingsDraftTools = "settings:draft.tools";
inline constexpr std::string_view kSettingsDraftPlugins = "settings:draft.plugins";
inline constexpr std::string_view kSettingsDraftMcp = "settings:draft.mcp";
inline constexpr std::string_view kSettingsDraftJobs = "settings:draft.jobs";
inline constexpr std::string_view kSettingsDraftSessions = "settings:draft.sessions";
inline constexpr std::string_view kSettingsDraftThinking = "settings:draft.thinking";
inline constexpr std::string_view kSettingsDraftDetails = "settings:draft.details";
inline constexpr std::string_view kSettingsSectionTheme = "settings:section.theme";
inline constexpr std::string_view kSettingsSectionDisplay = "settings:section.display";
inline constexpr std::string_view kSettingsSectionModels = "settings:section.models";
inline constexpr std::string_view kSettingsSectionInput = "settings:section.input";
inline constexpr std::string_view kSettingsSectionSessions = "settings:section.sessions";
inline constexpr std::string_view kSettingsSectionTools = "settings:section.tools";

// SettingsNavigationState owns this TUI-thread checkpoint while a child section is open. It copies query and
// selection values for restoration and borrows no view storage.
struct SettingsFrameState
{
  // Section to restore when returning from the child settings list.
  SettingsSection section = SettingsSection::Root;
  // Owned filter query retained across section navigation.
  ava::core::NoLogString query;
  // Prior row selection; clamp against the restored list before using it.
  std::size_t selected_item_index = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// DisplayPreviewTransaction owns this TUI-thread copy of applied display settings. Preview cancellation and
// application reloads use it to restore image/cursor values; it borrows nothing and never writes config.
struct DisplayPresentationBaseline
{
  // Applied image visibility before any temporary preview.
  bool show_images = true;
  // Applied image width in terminal cells.
  std::size_t image_width_cells = 60;
  // Applied cursor style and blink setting.
  terminal::CursorSettings cursor{terminal::CursorStyle::Default};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// DisplayPreviewTransaction owns this TUI-thread candidate selected by highlighting a settings row. Optional
// fields replace only their matching baseline settings; all values are owned and confirmation remains separate.
struct DisplayPreviewOverlay
{
  // Exact hidden row value retained for confirmation and for refreshing the candidate after reload.
  std::string action_token;
  // Theme candidate, including any owned palette data used by the temporary preview.
  std::optional<TuiThemeInfo> theme;
  // Temporary image visibility; absence retains the baseline.
  std::optional<bool> show_images;
  // Temporary image width; absence retains the baseline.
  std::optional<std::size_t> image_width_cells;
  // Temporary cursor shape; absence retains the baseline.
  std::optional<terminal::CursorStyle> cursor_style;
  // Temporary blink setting; absence retains the baseline.
  std::optional<bool> cursor_blink;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// SettingsNavigationState owns this TUI-thread preview transaction. Settings handlers apply its values to
// screen data and the theme preview, then explicitly clear it on cancel/confirm. It owns its baseline/candidate
// and borrows no screen; these operations do not persist configuration.
struct DisplayPreviewTransaction
{
  // Last applied settings, independent of the highlighted candidate.
  DisplayPresentationBaseline authoritative;
  // Current highlighted candidate; absent when no preview is active.
  std::optional<DisplayPreviewOverlay> overlay;

  // Capture applied settings and remove any previous candidate/theme preview.
  void begin(DisplayPresentationBaseline baseline);
  // Replace the candidate and apply its temporary theme immediately.
  void update(DisplayPreviewOverlay next);
  // Discard the candidate and clear the temporary theme; callers reapply baseline screen values.
  void cancel();
  // Clear preview state after the caller handles confirmation; this does not itself save settings.
  void confirm_clear();
  // Replace applied settings after reload while retaining and reapplying any current candidate theme.
  void rebase(DisplayPresentationBaseline baseline);
  // Report whether a highlighted candidate is retained.
  [[nodiscard]] bool active() const;
  // Restore baseline image/cursor settings into the borrowed screen, then apply candidate overrides.
  void apply_image_overlay(ComposerSnapshot& snapshot) const;
  // Set the temporary theme from the candidate, or clear it when no theme candidate exists.
  void apply_theme_overlay() const;
  // Remove the temporary theme without changing the retained baseline or candidate.
  void clear_theme_overlay() const;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// After an applied external display reload: always replace the applied-settings baseline from
// the hydrated snapshot, refresh a staged candidate when the action token still resolves
// against app-delivered options, otherwise keep last-known-good overlay bytes, then apply
// the image overlay onto snapshot. Never infers applied state from value equality.
void reapply_settings_preview_after_display_reload(DisplayPreviewTransaction& preview, ComposerSnapshot& snapshot);
// After Display rows are rebuilt (for example on applied reload), restore selection by exact
// hidden action value rather than a stale numeric index. Prefer the pre-rebuild selected
// actionable value; if absent, prefer the staged overlay action when that row still exists;
// otherwise clamp the prior index through ordinary non-action/filter rules.
[[nodiscard]] std::size_t reselect_settings_display_row_after_rebuild(SelectListView const& view, std::string_view selected_action_value,
                                                                      std::string_view staged_overlay_action, std::optional<std::size_t> prior_selected_index);

// SettingsDialog owns this TUI-thread navigation and preview state for an open settings selector. It owns
// restoration values and borrows no view data; the dialog clears previews before closing or changing owner.
struct SettingsNavigationState
{
  // Currently displayed settings section.
  SettingsSection section = SettingsSection::Root;
  // Saved root query and row selection while visiting a child section.
  std::optional<SettingsFrameState> root_frame;
  // Reversible candidate settings for Theme and Display sections.
  DisplayPreviewTransaction preview;

  // Return to root, discard its checkpoint, and cancel any display preview.
  void reset();
  // Report whether input is currently routed to the root settings list.
  [[nodiscard]] bool is_root() const;
  // Report whether the current Theme or Display section supports display previews.
  [[nodiscard]] bool in_display() const;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Pure lookup: report whether a hidden settings row value opens a child section.
[[nodiscard]] bool settings_action_is_section(std::string_view value);
// Pure classification: recognize candidate theme/image/cursor actions; theme reset requires confirmation.
[[nodiscard]] bool settings_action_is_previewable(std::string_view value);
// Pure lookup for the theme-reset row, which must not preview while merely highlighted.
[[nodiscard]] bool settings_action_is_theme_reset(std::string_view value);
// Pure lookup: map a section row value to its destination, or return no match.
[[nodiscard]] std::optional<SettingsSection> settings_section_for_action(std::string_view value);
// TUI thread: copy a valid built-in or application-supplied custom theme for this action; borrow inputs only
// for the call and return no value for unavailable/reset actions.
[[nodiscard]] std::optional<TuiThemeInfo> settings_preview_theme_for_action(std::string_view value, ComposerSnapshot const& snapshot);
// TUI thread: validate the action against current display options and return an owned candidate, or no value.
[[nodiscard]] std::optional<DisplayPreviewOverlay> settings_preview_overlay_for_action(std::string_view value, ComposerSnapshot const& snapshot);

// TUI-thread routing result consumed by BranchSummaryOperation; the application still owns summary work.
enum class BranchSummaryInputIntent
{
  Block, // Consume keys that have no action in this phase.
  Confirm, // Generate only while the operation is awaiting confirmation.
  Cancel, // Ask the application to cancel the operation.
  Exit, // Request session exit through its normal cancellation path.
};

// Failed application callback stage, used on the TUI thread to select concise status feedback.
enum class BranchSummaryCallbackFailure
{
  Prepare, // Preparing the operation failed.
  Snapshot, // Reading operation state failed.
  Confirm, // Confirmation was not accepted.
  Cancel, // Requesting cancellation failed.
  Refresh, // Refreshing the session list after completion failed.
};

// Pure phase check: true means summary work has reached a finished phase.
[[nodiscard]] bool branch_summary_terminal(TuiBranchSummarySnapshot const& snapshot) noexcept;
// TUI thread: build an owned modal from the borrowed application snapshot; only confirmation has an action row.
[[nodiscard]] SelectListView branch_summary_operation_view(TuiBranchSummarySnapshot const& snapshot);
// Pure formatting of final summary status; return owned feedback without retaining the snapshot.
[[nodiscard]] std::string branch_summary_terminal_status(TuiBranchSummarySnapshot const& snapshot);
// TUI thread: apply configured exit/cancel/confirm precedence for the current summary phase.
[[nodiscard]] BranchSummaryInputIntent branch_summary_input_intent(TuiBranchSummaryPhase phase, InputEvent const& event, TuiKeyBindings const& bindings);
// TUI thread: restore the previous query and selected session by hidden value, falling back to a clamped row.
[[nodiscard]] SelectListView restore_branch_summary_session_view(SelectListView refreshed, SelectListView const& prior, std::string_view selected_value,
                                                                 std::size_t prior_selected_index);
// Pure lookup returning a static status string for the failed callback stage.
[[nodiscard]] std::string_view branch_summary_callback_failure_status(BranchSummaryCallbackFailure failure) noexcept;
// Pure formatting of permission choices supported by the current prompt.
[[nodiscard]] std::string permission_prompt_status(bool allow_session_available, bool allow_remember_available, bool deny_remember_available);
// TUI thread: copy active-run key hints from the current bindings without retaining them.
[[nodiscard]] ActiveRunHint active_run_hint_for(TuiKeyBindings const& bindings);
// Pure formatting: keep only the final slash- or backslash-delimited path component for display.
[[nodiscard]] std::string compact_path_leaf(std::string path);
// TUI thread: copy selected/custom answers; return an error when a single-choice prompt has no answer.
[[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> question_answer_from_view(QuestionPromptView const& prompt);
// TUI thread: copy backend permission fields into an owned modal view; the prompt need only live for this call.
[[nodiscard]] PermissionPromptView permission_prompt_view(ava::permissions::PermissionPrompt const& prompt);
// TUI thread: copy backend question fields/options into an owned modal view; retain no prompt references.
[[nodiscard]] QuestionPromptView question_prompt_view(ava::agent::QuestionPrompt const& prompt);

}  // namespace runtime_views

}  // namespace ava::tui
