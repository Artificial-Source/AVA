#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/runtime.h"
#include "ava/tui/terminal/Cursor.h"
#include "ava/tui/theme.h"
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

struct SettingsFrameState
{
  SettingsSection section = SettingsSection::Root;
  std::string query;
  std::size_t selected_item_index = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// TUI-thread-owned reversible display preview. Highlight never writes config.
struct DisplayPresentationBaseline
{
  bool show_images = true;
  std::size_t image_width_cells = 60;
  terminal::CursorSettings cursor{terminal::CursorStyle::Default};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct DisplayPreviewOverlay
{
  // Exact settings action token staged for confirm (theme:/settings:images.*/settings:image-width.*).
  std::string action_token;
  std::optional<TuiThemeInfo> theme;
  std::optional<bool> show_images;
  std::optional<std::size_t> image_width_cells;
  std::optional<terminal::CursorStyle> cursor_style;
  std::optional<bool> cursor_blink;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct DisplayPreviewTransaction
{
  DisplayPresentationBaseline authoritative;
  std::optional<DisplayPreviewOverlay> overlay;

  void begin(DisplayPresentationBaseline baseline);
  void update(DisplayPreviewOverlay next);
  void cancel();
  void confirm_clear();
  void rebase(DisplayPresentationBaseline baseline);
  [[nodiscard]] bool active() const;
  void apply_image_overlay(ComposerSnapshot& snapshot) const;
  void apply_theme_overlay() const;
  void clear_theme_overlay() const;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// After an applied external display reload: always rebase the authoritative baseline from
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

struct SettingsNavigationState
{
  SettingsSection section = SettingsSection::Root;
  std::optional<SettingsFrameState> root_frame;
  DisplayPreviewTransaction preview;

  void reset();
  [[nodiscard]] bool is_root() const;
  [[nodiscard]] bool in_display() const;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] bool settings_action_is_section(std::string_view value);
[[nodiscard]] bool settings_action_is_previewable(std::string_view value);
[[nodiscard]] bool settings_action_is_theme_reset(std::string_view value);
[[nodiscard]] std::optional<SettingsSection> settings_section_for_action(std::string_view value);
[[nodiscard]] std::optional<TuiThemeInfo> settings_preview_theme_for_action(std::string_view value, ComposerSnapshot const& snapshot);
[[nodiscard]] std::optional<DisplayPreviewOverlay> settings_preview_overlay_for_action(std::string_view value, ComposerSnapshot const& snapshot);

enum class BranchSummaryInputIntent
{
  Block,
  Confirm,
  Cancel,
  Exit,
};

enum class BranchSummaryCallbackFailure
{
  Prepare,
  Snapshot,
  Confirm,
  Cancel,
  Refresh,
};

[[nodiscard]] bool branch_summary_terminal(TuiBranchSummarySnapshot const& snapshot) noexcept;
[[nodiscard]] SelectListView branch_summary_operation_view(TuiBranchSummarySnapshot const& snapshot);
[[nodiscard]] std::string branch_summary_terminal_status(TuiBranchSummarySnapshot const& snapshot);
[[nodiscard]] BranchSummaryInputIntent branch_summary_input_intent(TuiBranchSummaryPhase phase, InputEvent const& event, TuiKeyBindings const& bindings);
[[nodiscard]] SelectListView restore_branch_summary_session_view(SelectListView refreshed, SelectListView const& prior, std::string_view selected_value,
                                                                 std::size_t prior_selected_index);
[[nodiscard]] std::string_view branch_summary_callback_failure_status(BranchSummaryCallbackFailure failure) noexcept;
[[nodiscard]] std::string permission_prompt_status(bool allow_session_available, bool allow_remember_available, bool deny_remember_available);
[[nodiscard]] ActiveRunHint active_run_hint_for(TuiKeyBindings const& bindings);
[[nodiscard]] std::string compact_path_leaf(std::string path);
[[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> question_answer_from_view(QuestionPromptView const& prompt);
[[nodiscard]] PermissionPromptView permission_prompt_view(ava::permissions::PermissionPrompt const& prompt);
[[nodiscard]] QuestionPromptView question_prompt_view(ava::agent::QuestionPrompt const& prompt);

}  // namespace runtime_views

}  // namespace ava::tui
