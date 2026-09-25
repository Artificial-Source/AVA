#include "sys.h"
#include "ava/agent/question.h"
#include "ava/tui/composer.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/tui/theme.h"
#include "ava/permissions/permission.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui::runtime_views {
namespace {

std::vector<std::string> split_key_display(std::string_view keys)
{
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= keys.size())
  {
    auto const comma = keys.find(',', start);
    auto end = comma == std::string_view::npos ? keys.size() : comma;
    while (start < end && std::isspace(static_cast<unsigned char>(keys[start])) != 0)
      ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(keys[end - 1])) != 0)
      --end;
    if (start < end)
      parts.emplace_back(keys.substr(start, end - start));
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  return parts;
}

std::string first_key_display(TuiKeyBindings const& bindings, TuiAction action)
{
  for (auto const& [configured_action, keys] : bindings.bindings)
  {
    if (configured_action == action && !keys.empty())
      return key_display(keys.front());
  }
  return {};
}

std::size_t shared_key_count(std::vector<TuiKeyBindingHelpItem> const& items, std::string_view key)
{
  return static_cast<std::size_t>(std::ranges::count_if(items, [&](TuiKeyBindingHelpItem const& item) {
    auto const keys = split_key_display(item.keys);
    return std::ranges::find(keys, key) != keys.end();
  }));
}

std::string value_or_unknown(std::string value)
{
  return value.empty() ? std::string("unknown") : std::move(value);
}

void add_settings_action_row(SelectListView& view, std::string value, std::string label, std::string description = {}, std::string detail = {})
{
  view.items.push_back(SelectListItemView{.value = std::move(value),
                                          .label = std::move(label),
                                          .description = std::move(description),
                                          .group = {},
                                          .detail = std::move(detail),
                                          .badge = {},
                                          .current = false,
                                          .enabled = true,
                                          .disabled_reason = {}});
}

void add_theme_settings_action(SelectListView& view, std::string value, std::string label, bool current)
{
  view.items.push_back(SelectListItemView{.value = "theme:" + value,
                                          .label = std::move(label),
                                          .description = current ? std::string("current") : std::string{},
                                          .group = {},
                                          .detail = {},
                                          .badge = {},
                                          .current = current,
                                          .enabled = true,
                                          .disabled_reason = {}});
}

void add_custom_theme_settings_action(SelectListView& view, ThemeOptionItem const& theme, bool current)
{
  view.items.push_back(SelectListItemView{.value = "theme:" + theme.name,
                                          .label = theme.name,
                                          .description = current ? std::string("current") : std::string{},
                                          .group = {},
                                          .detail = {},
                                          .badge = {},
                                          .current = current,
                                          .enabled = true,
                                          .disabled_reason = {}});
}

void add_section_row(SelectListView& view, std::string_view value, std::string label)
{
  add_settings_action_row(view, std::string(value), std::move(label));
}

void add_image_width_action(SelectListView& view, std::size_t width, std::size_t current_width)
{
  bool const current = width == current_width;
  view.items.push_back(SelectListItemView{.value = std::string(kSettingsImageWidthPrefix) + std::to_string(width),
                                          .label = "Width " + std::to_string(width),
                                          .description = current ? std::string("current") : std::string{},
                                          .group = {},
                                          .detail = {},
                                          .badge = {},
                                          .current = current,
                                          .enabled = true,
                                          .disabled_reason = {}});
}

TuiThemeInfo built_in_theme_info(std::string_view name)
{
  if (name == "light" || name == "ava-light")
  {
    return TuiThemeInfo{.kind = TuiThemeKind::Light,
                        .name = "ava-light",
                        .detail = "built-in light ncurses token palette",
                        .badge = "preview",
                        .palette = std::nullopt,
                        .revision = "built-in-light"};
  }
  if (name == "plain" || name == "none" || name == "no-color" || name == "no_color")
  {
    return TuiThemeInfo{.kind = TuiThemeKind::Plain,
                        .name = "plain",
                        .detail = "disable ANSI styling in rendered frames",
                        .badge = "preview",
                        .palette = std::nullopt,
                        .revision = "plain"};
  }
  return TuiThemeInfo{.kind = TuiThemeKind::Dark,
                      .name = "ava-dark",
                      .detail = "built-in dark ncurses token palette",
                      .badge = "preview",
                      .palette = std::nullopt,
                      .revision = "built-in-dark"};
}

SelectListView make_settings_shell(std::string title, std::string empty_text, std::string footer_hint)
{
  return SelectListView{.title = std::move(title),
                        .subtitle = {},
                        .items = {},
                        .selected_item_index = 0,
                        .query = {},
                        .placeholder = {},
                        .empty_text = std::move(empty_text),
                        .footer_hint = std::move(footer_hint),
                        .compact_settings_chrome = true};
}

void populate_theme_section(SelectListView& view, ComposerSnapshot const& snapshot)
{
  auto const theme = active_tui_theme();
  add_theme_settings_action(view, "dark", "Dark", theme.kind == TuiThemeKind::Dark && theme.name == "ava-dark");
  add_theme_settings_action(view, "light", "Light", theme.kind == TuiThemeKind::Light);
  add_theme_settings_action(view, "plain", "Plain", theme.kind == TuiThemeKind::Plain);
  for (auto const& custom_theme : snapshot.custom_themes)
  {
    if (custom_theme.palette)
      add_custom_theme_settings_action(view, custom_theme, theme.kind == TuiThemeKind::Custom && custom_theme.name == theme.name);
  }
  add_settings_action_row(view, "theme:reset", "Default");
}

void populate_display_section(SelectListView& view, ComposerSnapshot const& snapshot)
{
  add_settings_action_row(view, std::string(kSettingsImagesOn), "Images on", snapshot.show_images ? "current" : std::string{});
  add_settings_action_row(view, std::string(kSettingsImagesOff), "Images off", snapshot.show_images ? std::string{} : "current");
  add_settings_action_row(view, std::string(kSettingsImagesReset), "Images default");
  constexpr std::size_t kWidthChoices[] = {40, 60, 80, 120};
  bool current_listed = false;
  for (auto const width : kWidthChoices)
  {
    if (width == snapshot.image_width_cells)
      current_listed = true;
    add_image_width_action(view, width, snapshot.image_width_cells);
  }
  if (!current_listed && snapshot.image_width_cells >= 8 && snapshot.image_width_cells <= 160)
    add_image_width_action(view, snapshot.image_width_cells, snapshot.image_width_cells);
  add_settings_action_row(view, std::string(kSettingsImageWidthReset), "Width default");
  auto add_cursor_style = [&](terminal::CursorStyle style, std::string_view token, std::string label) {
    add_settings_action_row(view, std::string(kSettingsCursorStylePrefix) + std::string(token), std::move(label),
                            snapshot.cursor.style() == style ? "current" : std::string{});
  };
  add_cursor_style(terminal::CursorStyle::Default, "default", "Cursor default");
  add_cursor_style(terminal::CursorStyle::Block, "block", "Cursor block");
  add_cursor_style(terminal::CursorStyle::Underline, "underline", "Cursor underline");
  add_cursor_style(terminal::CursorStyle::Bar, "bar", "Cursor bar");
  add_settings_action_row(view, std::string(kSettingsCursorBlink), "Cursor blink", snapshot.cursor.blink() ? "current" : std::string{});
  add_settings_action_row(view, std::string(kSettingsCursorSteady), "Cursor steady", snapshot.cursor.blink() ? std::string{} : "current");
  add_settings_action_row(view, std::string(kSettingsDraftThinking), "Thinking blocks", snapshot.thinking_visible ? "visible" : "hidden");
}

void populate_models_section(SelectListView& view, ComposerSnapshot const& snapshot)
{
  add_settings_action_row(view, std::string(kSettingsOpenModels), "Model", value_or_unknown(snapshot.provider) + "/" + value_or_unknown(snapshot.model));
  add_settings_action_row(view, std::string(kSettingsOpenReasoning), "Reasoning", snapshot.reasoning_status.value_or("default"));
  add_settings_action_row(view, std::string(kSettingsOpenScopedModels), "Cycle scope", "Ctrl+P");
}

void populate_input_section(SelectListView& view, TuiKeyBindings const& bindings)
{
  auto const binding_count = key_binding_help_items(bindings).size();
  add_settings_action_row(view, std::string(kSettingsOpenKeybindings), "Keybindings", std::to_string(binding_count));
  add_settings_action_row(view, std::string(kSettingsEditKeybindings), "Edit config");
  add_settings_action_row(view, std::string(kSettingsReloadKeybindings), "Reload");
}

// Concise path-free trust summary for the Workspace settings subtitle. Built only from
// the already-loaded snapshot and kept short enough for the one-line modal title row;
// detailed diagnostics stay with explicit /trust status.
std::string settings_workspace_trust_summary(ProjectTrustSnapshot const& trust)
{
  std::string summary = "trust " + value_or_unknown(trust.decision);
  if (!trust.project_resources.empty())
    summary += " · " + trust.project_resources;
  summary += " · " + std::to_string(trust.protected_resource_count) + " protected";
  return summary;
}

void populate_sessions_section(SelectListView& view, ComposerSnapshot const& snapshot)
{
  add_settings_action_row(view, std::string(kSettingsDraftSessions), "Sessions");
  if (snapshot.project_trust)
  {
    add_settings_action_row(view, std::string(kSettingsTrustProject), "Trust project", {}, "Enables this project's resources");
    add_settings_action_row(view, std::string(kSettingsTrustDeny), "Deny project", {}, "Keeps this project's resources disabled");
    add_settings_action_row(view, std::string(kSettingsTrustClear), "Clear trust decision");
  }
}

void populate_tools_section(SelectListView& view, ComposerSnapshot const& snapshot)
{
  add_settings_action_row(view, std::string(kSettingsDraftTools), "Tool details", std::string(to_string(snapshot.tool_presentation)));
  add_settings_action_row(view, std::string(kSettingsDraftPermissions), "Permissions");
  add_settings_action_row(view, std::string(kSettingsDraftPlugins), "Plugins");
  add_settings_action_row(view, std::string(kSettingsDraftMcp), "MCP");
  add_settings_action_row(view, std::string(kSettingsDraftJobs), "Jobs");
}

}  // namespace

ava::core::Result<ava::agent::QuestionAnswer> question_answer_from_view(QuestionPromptView const& prompt)
{
  ava::agent::QuestionAnswer answer;
  for (auto const& option : prompt.options)
  {
    if (option.selected)
      answer.selected_options.push_back(option.value);
  }

  if (prompt.allow_custom && !prompt.custom_text.empty() && (!prompt.searchable || answer.selected_options.empty()))
  {
    answer.custom_text = prompt.custom_text;
  }

  if (!prompt.multiple && answer.selected_options.empty() && answer.custom_text.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt resolved without an answer"));
  }

  return answer;
}

bool branch_summary_terminal(TuiBranchSummarySnapshot const& snapshot) noexcept
{
  switch (snapshot.phase)
  {
    case TuiBranchSummaryPhase::Succeeded:
    case TuiBranchSummaryPhase::Existing:
    case TuiBranchSummaryPhase::Ineligible:
    case TuiBranchSummaryPhase::Canceled:
    case TuiBranchSummaryPhase::Failed:
      return true;
    case TuiBranchSummaryPhase::Idle:
    case TuiBranchSummaryPhase::Preparing:
    case TuiBranchSummaryPhase::AwaitingConfirmation:
    case TuiBranchSummaryPhase::Generating:
    case TuiBranchSummaryPhase::Revalidating:
    case TuiBranchSummaryPhase::Appending:
      return false;
  }
  return true;
}

BranchSummaryInputIntent branch_summary_input_intent(TuiBranchSummaryPhase phase, InputEvent const& event, TuiKeyBindings const& bindings)
{
  if (key_matches_action(bindings, TuiAction::Exit, event.key))
    return BranchSummaryInputIntent::Exit;
  if (key_matches_action(bindings, TuiAction::SelectCancel, event.key))
    return BranchSummaryInputIntent::Cancel;
  if (phase == TuiBranchSummaryPhase::AwaitingConfirmation && key_matches_action(bindings, TuiAction::SelectConfirm, event.key))
    return BranchSummaryInputIntent::Confirm;
  return BranchSummaryInputIntent::Block;
}

SelectListView restore_branch_summary_session_view(SelectListView refreshed, SelectListView const& prior, std::string_view selected_value,
                                                   std::size_t prior_selected_index)
{
  refreshed.query = prior.query;
  auto const selected = std::ranges::find_if(refreshed.items, [&](SelectListItemView const& item) { return item.value == selected_value; });
  if (selected != refreshed.items.end())
    refreshed.selected_item_index = static_cast<std::size_t>(selected - refreshed.items.begin());
  else
    refreshed.selected_item_index = clamp_select_list_selection(refreshed, prior_selected_index);
  return refreshed;
}

std::string_view branch_summary_callback_failure_status(BranchSummaryCallbackFailure failure) noexcept
{
  switch (failure)
  {
    case BranchSummaryCallbackFailure::Prepare:
      return "parent summary could not be prepared";
    case BranchSummaryCallbackFailure::Snapshot:
      return "parent summary status is unavailable";
    case BranchSummaryCallbackFailure::Confirm:
      return "parent summary confirmation was not accepted";
    case BranchSummaryCallbackFailure::Cancel:
      return "parent summary cancel could not be requested";
    case BranchSummaryCallbackFailure::Refresh:
      return "parent summary completed, but session list refresh failed";
  }
  return "parent summary callback failed";
}

SelectListView branch_summary_operation_view(TuiBranchSummarySnapshot const& snapshot)
{
  auto bounded_label = [](std::string_view value, std::string_view fallback) {
    std::string label;
    label.reserve(std::min<std::size_t>(value.size(), 256));
    bool previous_space = false;
    for (unsigned char const byte : value)
    {
      if (byte < 0x20 || byte == 0x7f)
      {
        if (!previous_space && !label.empty())
          label.push_back(' ');
        previous_space = true;
        continue;
      }
      label.push_back(static_cast<char>(byte));
      previous_space = byte == ' ';
    }
    if (label.size() > 256)
    {
      std::size_t prefix = 256;
      while (prefix > 0 && (static_cast<unsigned char>(label[prefix]) & 0xc0U) == 0x80U)
        --prefix;
      label.resize(prefix);
    }
    while (!label.empty() && label.back() == ' ')
      label.pop_back();
    return label.empty() ? std::string(fallback) : label;
  };
  auto const source = bounded_label(snapshot.source_label, "selected parent");
  auto const model = bounded_label(snapshot.model_label, "active model");

  std::string title;
  std::string action_label;
  bool actionable = false;
  switch (snapshot.phase)
  {
    case TuiBranchSummaryPhase::Preparing:
      title = "Preparing parent summary";
      action_label = "Checking abandoned parent…";
      break;
    case TuiBranchSummaryPhase::AwaitingConfirmation:
      title = "Summarize abandoned parent?";
      action_label = "Generate summary";
      actionable = true;
      break;
    case TuiBranchSummaryPhase::Generating:
      title = "Generating parent summary";
      action_label = "Generating summary…";
      break;
    case TuiBranchSummaryPhase::Revalidating:
      title = "Revalidating parent summary";
      action_label = "Checking unchanged source…";
      break;
    case TuiBranchSummaryPhase::Appending:
      title = "Saving parent summary";
      action_label = "Appending metadata…";
      break;
    case TuiBranchSummaryPhase::Idle:
      title = "Preparing parent summary";
      action_label = "Starting…";
      break;
    case TuiBranchSummaryPhase::Succeeded:
    case TuiBranchSummaryPhase::Existing:
    case TuiBranchSummaryPhase::Ineligible:
    case TuiBranchSummaryPhase::Canceled:
    case TuiBranchSummaryPhase::Failed:
      title = "Parent summary complete";
      action_label = "Closing…";
      break;
  }

  return SelectListView{.title = std::move(title),
                        .subtitle = "Source: " + source + " · Model: " + model,
                        .items = {SelectListItemView{.value = actionable ? std::string("branch-summary:generate") : std::string{},
                                                     .label = std::move(action_label),
                                                     .description = actionable ? std::string("Provider work starts only after confirmation") : std::string{},
                                                     .group = "Parent summary",
                                                     .detail = {},
                                                     .badge = actionable ? std::string("confirm") : std::string("working"),
                                                     .current = false,
                                                     .enabled = actionable,
                                                     .disabled_reason = actionable ? std::string{} : std::string("operation in progress")}},
                        .selected_item_index = 0,
                        .query = {},
                        .placeholder = {},
                        .empty_text = {},
                        .footer_hint = actionable ? std::string("Enter generate · Esc cancel") : std::string("Esc cancel")};
}

std::string branch_summary_terminal_status(TuiBranchSummarySnapshot const& snapshot)
{
  if (snapshot.append_commit_state)
  {
    switch (*snapshot.append_commit_state)
    {
      case TuiBranchSummaryAppendCommitState::NotStarted:
        return "parent summary append did not start; inspect the source; no automatic retry";
      case TuiBranchSummaryAppendCommitState::PartialOrUnknown:
        return "parent summary append state is uncertain; inspect the source; no automatic retry";
      case TuiBranchSummaryAppendCommitState::CommittedToLeasedInode:
        return "parent summary may be committed; inspect the source; no automatic retry";
    }
  }

  switch (snapshot.phase)
  {
    case TuiBranchSummaryPhase::Succeeded:
      return "abandoned parent summary saved";
    case TuiBranchSummaryPhase::Existing:
      return "abandoned parent summary already exists";
    case TuiBranchSummaryPhase::Canceled:
      return "abandoned parent summary canceled";
    case TuiBranchSummaryPhase::Ineligible:
      if (!snapshot.eligibility_code)
        return "selected parent is not eligible for summarization";
      switch (*snapshot.eligibility_code)
      {
        case TuiBranchSummaryEligibilityCode::CurrentSessionEphemeral:
          return "parent summary is unavailable for a temporary current session";
        case TuiBranchSummaryEligibilityCode::CurrentSessionUnavailable:
          return "current session is unavailable for parent summarization";
        case TuiBranchSummaryEligibilityCode::ActiveRun:
          return "finish the active run before summarizing the parent";
        case TuiBranchSummaryEligibilityCode::InvalidSourceSelection:
          return "select the current session's direct parent";
        case TuiBranchSummaryEligibilityCode::NotDirectSource:
          return "selected session is not the current direct parent";
        case TuiBranchSummaryEligibilityCode::InvalidFork:
          return "current branch origin is not eligible for parent summarization";
        case TuiBranchSummaryEligibilityCode::SourceUnavailable:
          return "selected parent session is unavailable";
        case TuiBranchSummaryEligibilityCode::SourceLeaseBusy:
          return "selected parent is active in another AVA host";
        case TuiBranchSummaryEligibilityCode::SourceCorrupt:
          return "selected parent needs recovery before summarization";
        case TuiBranchSummaryEligibilityCode::ForkEntryNotFound:
          return "branch point is missing from the selected parent";
        case TuiBranchSummaryEligibilityCode::EmptySuffix:
          return "selected parent has no abandoned work to summarize";
      }
      break;
    case TuiBranchSummaryPhase::Failed:
      if (!snapshot.failure_code)
        return "parent summary failed";
      switch (*snapshot.failure_code)
      {
        case TuiBranchSummaryFailureCode::Deadline:
          return "parent summary timed out";
        case TuiBranchSummaryFailureCode::RecoveryFailed:
          return "parent recovery failed; inspect the source before retrying";
        case TuiBranchSummaryFailureCode::ProjectionRecordLimit:
          return "abandoned parent range exceeds the summary record limit";
        case TuiBranchSummaryFailureCode::ProjectionTextLimit:
          return "abandoned parent range exceeds the summary text limit";
        case TuiBranchSummaryFailureCode::ProjectionByteLimit:
          return "abandoned parent projection exceeds the summary byte limit";
        case TuiBranchSummaryFailureCode::ProjectionInvalidText:
          return "abandoned parent range contains invalid summary text";
        case TuiBranchSummaryFailureCode::ProjectionEmpty:
          return "abandoned parent range has no summarizable text";
        case TuiBranchSummaryFailureCode::ModelUnavailable:
          return "active model is unavailable for parent summarization";
        case TuiBranchSummaryFailureCode::AuthenticationUnavailable:
          return "provider authentication is unavailable for parent summarization";
        case TuiBranchSummaryFailureCode::ProviderFailed:
          return "provider failed to generate the parent summary";
        case TuiBranchSummaryFailureCode::InvalidGeneratedSummary:
          return "provider returned an invalid parent summary";
        case TuiBranchSummaryFailureCode::StaleSource:
          return "parent changed after confirmation; no summary was saved";
        case TuiBranchSummaryFailureCode::AppendFailed:
          return "parent summary append failed; inspect the source; no automatic retry";
        case TuiBranchSummaryFailureCode::Internal:
          return "parent summary failed internally";
      }
      break;
    case TuiBranchSummaryPhase::Idle:
    case TuiBranchSummaryPhase::Preparing:
    case TuiBranchSummaryPhase::AwaitingConfirmation:
    case TuiBranchSummaryPhase::Generating:
    case TuiBranchSummaryPhase::Revalidating:
    case TuiBranchSummaryPhase::Appending:
      return "parent summary operation in progress";
  }
  return "parent summary failed";
}

std::string permission_prompt_status(bool allow_session_available, bool allow_remember_available, bool deny_remember_available)
{
  bool const remember_available = allow_remember_available || deny_remember_available;
  std::string status = "permission required: A=allow once";
  if (allow_session_available)
    status += " S=allow session";
  status += " D=reject G=guide rejection";
  if (remember_available)
    status += " R=remember";
  status += " Tab/Left/Right choose Enter confirm Esc reject";
  return status;
}

ActiveRunHint active_run_hint_for(TuiKeyBindings const& bindings)
{
  return ActiveRunHint{.submit_or_queue = first_key_display(bindings, TuiAction::Submit),
                       .follow_up = first_key_display(bindings, TuiAction::MessageFollowUp),
                       .dequeue = first_key_display(bindings, TuiAction::MessageDequeue),
                       .interrupt = first_key_display(bindings, TuiAction::Cancel),
                       .jump_to_bottom = first_key_display(bindings, TuiAction::JumpToBottom)};
}

std::string compact_path_leaf(std::string path)
{
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
    path.pop_back();
  auto const slash = path.find_last_of("/\\");
  if (slash == std::string::npos)
    return path;
  if (slash + 1 >= path.size())
    return path;
  return path.substr(slash + 1);
}

PermissionPromptView permission_prompt_view(ava::permissions::PermissionPrompt const& prompt)
{
  PermissionPromptView view;
  view.tool_name = prompt.tool_name;
  view.operation = ava::permissions::to_string(prompt.operation);
  view.target = prompt.target_path.empty() ? std::string{} : prompt.target_path.generic_string();
  view.command = prompt.command;
  view.reason = prompt.reason;
  view.risk = ava::permissions::to_string(prompt.risk);
  view.request_id = prompt.permission_request_id;
  view.diff_preview = prompt.diff_preview;
  view.diff_truncated = prompt.diff_truncated;
  if (prompt.command_metadata)
  {
    view.recipe_display = prompt.command_metadata->recipe_display;
    view.workspace_recipe_key = prompt.command_metadata->workspace_recipe_key;
    for (std::size_t index = 0; index < prompt.command_metadata->effective_allowed_scopes.size(); ++index)
    {
      if (index > 0)
        view.effective_allowed_scopes += ", ";
      view.effective_allowed_scopes += ava::command::to_string(prompt.command_metadata->effective_allowed_scopes[index]);
    }
  }
  return view;
}

QuestionPromptView question_prompt_view(ava::agent::QuestionPrompt const& prompt)
{
  QuestionPromptView view;
  view.header = prompt.header;
  view.question = prompt.question;
  view.multiple = prompt.multiple;
  view.allow_custom = prompt.allow_custom;
  view.secret = prompt.secret;
  view.modal = prompt.modal;
  view.searchable = prompt.searchable;
  view.options.reserve(prompt.options.size());
  for (auto const& option : prompt.options)
  {
    view.options.push_back(QuestionPromptOptionView{.value = option.value, .label = option.label, .selected = false});
  }
  return view;
}

SelectListView build_settings_select_list_view_for_section(SettingsSection section, ComposerSnapshot const& snapshot, TuiKeyBindings const& bindings,
                                                           std::string footer_hint)
{
  switch (section)
  {
    case SettingsSection::Root: {
      auto view = make_settings_shell("Settings", "No settings match", footer_hint.empty() ? std::string("Enter open · Esc close") : std::move(footer_hint));
      view.items.reserve(6);
      add_section_row(view, kSettingsSectionTheme, "Theme");
      add_section_row(view, kSettingsSectionDisplay, "Display");
      add_section_row(view, kSettingsSectionModels, "Model");
      add_section_row(view, kSettingsSectionInput, "Input");
      add_section_row(view, kSettingsSectionSessions, "Workspace");
      add_section_row(view, kSettingsSectionTools, "Tools");
      return view;
    }
    case SettingsSection::Theme: {
      auto view = make_settings_shell("Settings › Theme", "No theme settings match",
                                      footer_hint.empty() ? std::string("Enter select · Esc back") : std::move(footer_hint));
      view.items.reserve(8 + snapshot.custom_themes.size());
      populate_theme_section(view, snapshot);
      return view;
    }
    case SettingsSection::Display: {
      auto view = make_settings_shell("Settings › Display", "No display settings match",
                                      footer_hint.empty() ? std::string("Enter select · Esc back") : std::move(footer_hint));
      view.items.reserve(18);
      populate_display_section(view, snapshot);
      return view;
    }
    case SettingsSection::ModelsAndReasoning: {
      auto view = make_settings_shell("Settings › Model", "No model settings match",
                                      footer_hint.empty() ? std::string("Enter select · Esc back") : std::move(footer_hint));
      view.items.reserve(3);
      populate_models_section(view, snapshot);
      return view;
    }
    case SettingsSection::InputAndKeybindings: {
      auto view = make_settings_shell("Settings › Input", "No input settings match",
                                      footer_hint.empty() ? std::string("Enter select · Esc back") : std::move(footer_hint));
      view.items.reserve(3);
      populate_input_section(view, bindings);
      return view;
    }
    case SettingsSection::SessionsAndWorkspace: {
      auto view = make_settings_shell("Settings › Workspace", "No workspace settings match",
                                      footer_hint.empty() ? std::string("Enter select · Esc back") : std::move(footer_hint));
      if (snapshot.project_trust)
        view.subtitle = settings_workspace_trust_summary(*snapshot.project_trust);
      view.items.reserve(4);
      populate_sessions_section(view, snapshot);
      return view;
    }
    case SettingsSection::ToolsAndExtensions: {
      auto view = make_settings_shell("Settings › Tools", "No tool settings match",
                                      footer_hint.empty() ? std::string("Enter select · Esc back") : std::move(footer_hint));
      view.items.reserve(5);
      populate_tools_section(view, snapshot);
      return view;
    }
  }
  return build_settings_select_list_view_for_section(SettingsSection::Root, snapshot, bindings, std::move(footer_hint));
}

SelectListView build_hotkeys_select_list_view(TuiKeyBindings const& bindings, std::string footer_hint)
{
  auto const help_items = key_binding_help_items(bindings);
  SelectListView view{
      .title = "Keybindings",
      .subtitle =
          "Active TUI bindings · Enter drafts /keybindings set · config $XDG_CONFIG_HOME/ava/keybinds.json · init /keybindings init · reload /reload "
          "keybindings",
      .items = {},
      .selected_item_index = 0,
      .query = {},
      .placeholder = "Search keybindings",
      .empty_text = "No keybindings match",
      .footer_hint = footer_hint.empty() ? std::string("Type to filter · PgUp/PgDn page · Enter draft edit · Esc close") : std::move(footer_hint)};
  view.items.reserve(help_items.size());
  for (auto const& item : help_items)
  {
    auto badge = item.keys;
    for (auto const& key : split_key_display(item.keys))
    {
      if (shared_key_count(help_items, key) > 1)
      {
        if (!badge.empty())
          badge += " · ";
        badge += "shared";
        break;
      }
    }
    auto const human_label = item.label.empty() ? item.action : item.label;
    view.items.push_back(SelectListItemView{.value = item.action,
                                            .label = human_label,
                                            .description = item.action,
                                            .group = "Hotkeys",
                                            .detail = {},
                                            .badge = std::move(badge),
                                            .current = false,
                                            .enabled = true,
                                            .disabled_reason = {}});
  }
  if (view.items.empty())
  {
    view.items.push_back(SelectListItemView{.value = {},
                                            .label = "No active hotkeys",
                                            .description = "Keybinding loader returned no displayable bindings",
                                            .group = "Hotkeys",
                                            .detail = {},
                                            .badge = {},
                                            .current = false,
                                            .enabled = false,
                                            .disabled_reason = "no bindings"});
  }
  return view;
}

void DisplayPreviewTransaction::begin(DisplayPresentationBaseline baseline)
{
  authoritative = baseline;
  overlay.reset();
  clear_theme_overlay();
}

void DisplayPreviewTransaction::update(DisplayPreviewOverlay next)
{
  overlay = std::move(next);
  apply_theme_overlay();
}

void DisplayPreviewTransaction::cancel()
{
  overlay.reset();
  clear_theme_overlay();
}

void DisplayPreviewTransaction::confirm_clear()
{
  overlay.reset();
  clear_theme_overlay();
}

void DisplayPreviewTransaction::rebase(DisplayPresentationBaseline baseline)
{
  authoritative = baseline;
  if (overlay)
    apply_theme_overlay();
  else
    clear_theme_overlay();
}

bool DisplayPreviewTransaction::active() const
{
  return overlay.has_value();
}

void DisplayPreviewTransaction::apply_image_overlay(ComposerSnapshot& snapshot) const
{
  snapshot.show_images = authoritative.show_images;
  snapshot.image_width_cells = authoritative.image_width_cells;
  snapshot.cursor = authoritative.cursor;
  if (overlay)
  {
    if (overlay->show_images)
      snapshot.show_images = *overlay->show_images;
    if (overlay->image_width_cells)
      snapshot.image_width_cells = *overlay->image_width_cells;
    snapshot.cursor =
        terminal::CursorSettings{overlay->cursor_style.value_or(snapshot.cursor.style()), overlay->cursor_blink.value_or(snapshot.cursor.blink())};
  }
}

void DisplayPreviewTransaction::apply_theme_overlay() const
{
  if (overlay && overlay->theme)
    set_tui_theme_preview(*overlay->theme);
  else
    clear_tui_theme_preview();
}

void DisplayPreviewTransaction::clear_theme_overlay() const
{
  clear_tui_theme_preview();
}

void reapply_settings_preview_after_display_reload(DisplayPreviewTransaction& preview, ComposerSnapshot& snapshot)
{
  // Always rebase from the hydrated authoritative presentation. Do not compare overlay values.
  auto const staged_token = preview.overlay ? std::optional<std::string>{preview.overlay->action_token} : std::nullopt;
  preview.rebase(DisplayPresentationBaseline{.show_images = snapshot.show_images, .image_width_cells = snapshot.image_width_cells, .cursor = snapshot.cursor});
  if (staged_token)
  {
    // Refresh against app-delivered options when the token still resolves. If a previously valid
    // custom theme became invalid, keep the last-known-good overlay rather than adopting missing/invalid bytes.
    if (auto refreshed = settings_preview_overlay_for_action(*staged_token, snapshot))
      preview.update(std::move(*refreshed));
    else
      preview.apply_theme_overlay();
  }
  preview.apply_image_overlay(snapshot);
}

std::size_t reselect_settings_display_row_after_rebuild(SelectListView const& view, std::string_view selected_action_value,
                                                        std::string_view staged_overlay_action, std::optional<std::size_t> prior_selected_index)
{
  auto find_exact_value = [&view](std::string_view value) -> std::optional<std::size_t> {
    if (value.empty())
      return std::nullopt;
    for (std::size_t index = 0; index < view.items.size(); ++index)
    {
      if (view.items[index].value == value)
        return index;
    }
    return std::nullopt;
  };

  if (auto const matched = find_exact_value(selected_action_value))
    return clamp_select_list_selection(view, *matched);
  if (auto const matched = find_exact_value(staged_overlay_action))
    return clamp_select_list_selection(view, *matched);
  if (prior_selected_index)
    return clamp_select_list_selection(view, *prior_selected_index);
  return clamp_select_list_selection(view, 0);
}

void SettingsNavigationState::reset()
{
  section = SettingsSection::Root;
  root_frame.reset();
  preview.cancel();
}

bool SettingsNavigationState::is_root() const
{
  return section == SettingsSection::Root;
}

bool SettingsNavigationState::in_display() const
{
  return section == SettingsSection::Theme || section == SettingsSection::Display;
}

bool settings_action_is_section(std::string_view value)
{
  return settings_section_for_action(value).has_value();
}

bool settings_action_is_theme_reset(std::string_view value)
{
  return value == "theme:reset";
}

bool settings_action_is_previewable(std::string_view value)
{
  if (settings_action_is_theme_reset(value))
    return false;
  if (value.starts_with("theme:"))
    return true;
  if (value == kSettingsImagesOn || value == kSettingsImagesOff || value == kSettingsImagesReset)
    return true;
  if (value == kSettingsImageWidthReset || value.starts_with(kSettingsImageWidthPrefix))
    return true;
  if (value.starts_with(kSettingsCursorStylePrefix) || value == kSettingsCursorBlink || value == kSettingsCursorSteady)
    return true;
  return false;
}

std::optional<SettingsSection> settings_section_for_action(std::string_view value)
{
  if (value == kSettingsSectionTheme)
    return SettingsSection::Theme;
  if (value == kSettingsSectionDisplay)
    return SettingsSection::Display;
  if (value == kSettingsSectionModels)
    return SettingsSection::ModelsAndReasoning;
  if (value == kSettingsSectionInput)
    return SettingsSection::InputAndKeybindings;
  if (value == kSettingsSectionSessions)
    return SettingsSection::SessionsAndWorkspace;
  if (value == kSettingsSectionTools)
    return SettingsSection::ToolsAndExtensions;
  return std::nullopt;
}

std::optional<TuiThemeInfo> settings_preview_theme_for_action(std::string_view value, ComposerSnapshot const& snapshot)
{
  constexpr std::string_view theme_prefix = "theme:";
  if (!value.starts_with(theme_prefix) || settings_action_is_theme_reset(value))
    return std::nullopt;
  auto const name = value.substr(theme_prefix.size());
  if (name == "dark" || name == "light" || name == "plain" || name == "ava-dark" || name == "ava-light")
    return built_in_theme_info(name);
  for (auto const& custom : snapshot.custom_themes)
  {
    if (custom.name != name || !custom.palette)
      continue;
    return TuiThemeInfo{.kind = TuiThemeKind::Custom,
                        .name = custom.name,
                        .detail = custom.detail.empty() ? std::string("custom theme preview") : custom.detail,
                        .badge = "preview",
                        .palette = custom.palette,
                        .revision = custom.revision.empty() ? std::string("custom-preview") : custom.revision};
  }
  return std::nullopt;
}

std::optional<DisplayPreviewOverlay> settings_preview_overlay_for_action(std::string_view value, ComposerSnapshot const& snapshot)
{
  if (!settings_action_is_previewable(value))
    return std::nullopt;

  // Default-initialize optional overlay fields; only stage the confirmed action token here.
  DisplayPreviewOverlay overlay{};
  overlay.action_token = std::string(value);
  if (auto theme = settings_preview_theme_for_action(value, snapshot))
  {
    overlay.theme = std::move(*theme);
    return overlay;
  }
  if (value == kSettingsImagesOn)
  {
    overlay.show_images = true;
    return overlay;
  }
  if (value == kSettingsImagesOff)
  {
    overlay.show_images = false;
    return overlay;
  }
  if (value == kSettingsImagesReset)
  {
    overlay.show_images = true;
    return overlay;
  }
  if (value == kSettingsImageWidthReset)
  {
    overlay.image_width_cells = 60;
    return overlay;
  }
  if (value.starts_with(kSettingsCursorStylePrefix))
  {
    auto const style = value.substr(kSettingsCursorStylePrefix.size());
    if (style == "default")
      overlay.cursor_style = terminal::CursorStyle::Default;
    else if (style == "block")
      overlay.cursor_style = terminal::CursorStyle::Block;
    else if (style == "underline")
      overlay.cursor_style = terminal::CursorStyle::Underline;
    else if (style == "bar")
      overlay.cursor_style = terminal::CursorStyle::Bar;
    else
      return std::nullopt;
    return overlay;
  }
  if (value == kSettingsCursorBlink)
  {
    overlay.cursor_blink = true;
    return overlay;
  }
  if (value == kSettingsCursorSteady)
  {
    overlay.cursor_blink = false;
    return overlay;
  }
  if (value.starts_with(kSettingsImageWidthPrefix))
  {
    auto const width_text = value.substr(kSettingsImageWidthPrefix.size());
    std::size_t width = 0;
    for (char const ch : width_text)
    {
      if (ch < '0' || ch > '9')
        return std::nullopt;
      width = (width * 10) + static_cast<std::size_t>(ch - '0');
      if (width > 160)
        return std::nullopt;
    }
    if (width < 8)
      return std::nullopt;
    overlay.image_width_cells = width;
    return overlay;
  }
  return std::nullopt;
}

}  // namespace ava::tui::runtime_views

namespace ava::tui {
using runtime_views::kSettingsEditKeybindings;
using runtime_views::kSettingsImagesOff;
using runtime_views::kSettingsImagesOn;
using runtime_views::kSettingsImagesReset;
using runtime_views::kSettingsImageWidthPrefix;
using runtime_views::kSettingsImageWidthReset;
using runtime_views::kSettingsOpenKeybindings;
using runtime_views::kSettingsOpenModels;
using runtime_views::kSettingsOpenReasoning;
using runtime_views::kSettingsOpenScopedModels;
using runtime_views::kSettingsReloadKeybindings;
using runtime_views::kSettingsTrustClear;
using runtime_views::kSettingsTrustDeny;
using runtime_views::kSettingsTrustProject;
using runtime_views::question_answer_from_view;

ava::core::Result<ava::agent::QuestionAnswer> question_answer_from_prompt_view(QuestionPromptView const& prompt)
{
  return question_answer_from_view(prompt);
}

SelectListView hotkeys_select_list_view(TuiKeyBindings const& bindings, std::string footer_hint)
{
  return runtime_views::build_hotkeys_select_list_view(bindings, std::move(footer_hint));
}

namespace {

void push_overview_row(SelectListView& view, std::string group, std::string label, std::string detail = {}, std::string badge = {}, std::string value = {})
{
  if (label.empty() && detail.empty())
    return;
  if (value.empty())
    value = group + ":" + label + ":" + detail;
  view.items.push_back(SelectListItemView{.value = std::move(value),
                                          .label = std::move(label),
                                          .description = {},
                                          .group = std::move(group),
                                          .detail = std::move(detail),
                                          .badge = std::move(badge),
                                          .current = false,
                                          .enabled = true,
                                          .disabled_reason = {}});
}

}  // namespace

SelectListView overview_select_list_view(StartupOverviewSnapshot const& overview, std::string footer_hint)
{
  SelectListView view{.title = "Startup overview",
                      .subtitle = "Path-free runtime resources · read-only · Esc close",
                      .items = {},
                      .selected_item_index = 0,
                      .query = {},
                      .placeholder = "Filter overview",
                      .empty_text = "No overview rows match",
                      .footer_hint = footer_hint.empty() ? std::string("Type to filter · PgUp/PgDn page · Enter/Esc close") : std::move(footer_hint),
                      .freeze_underlying_transcript_layout = true};

  push_overview_row(view, "Runtime", "Mode", overview.mode);
  push_overview_row(view, "Runtime", "Provider", overview.provider);
  push_overview_row(view, "Runtime", "Model", overview.model);
  if (!overview.trust_decision.empty())
  {
    auto detail = overview.trust_decision;
    if (!overview.project_resources.empty())
      detail += " · project " + overview.project_resources;
    if (overview.protected_resource_count > 0)
      detail += " · protected " + std::to_string(overview.protected_resource_count);
    push_overview_row(view, "Trust", "Decision", std::move(detail));
  }
  if (!overview.theme_name.empty())
    push_overview_row(view, "Display", "Theme", overview.theme_name, overview.theme_badge);

  // Session titles/ids/origins are omitted: free-form and prompt-derived labels are not safe.

  // Exact O(1) instruction total. Group/plugin-failure aggregates may be lower bounds.
  auto format_bounded_count = [](std::size_t count, bool is_lower_bound) {
    auto text = std::to_string(count);
    if (is_lower_bound)
      text += '+';
    return text;
  };
  if (overview.instruction_source_count > 0)
    push_overview_row(view, "Instructions", "Sources", std::to_string(overview.instruction_source_count));
  for (auto const& group : overview.resource_groups)
  {
    auto label = group.kind;
    if (!group.scope.empty())
      label += " · " + group.scope;
    auto detail = format_bounded_count(group.count, group.count_is_lower_bound);
    if (!group.labels.empty())
    {
      detail += " · ";
      for (std::size_t index = 0; index < group.labels.size(); ++index)
      {
        if (index > 0)
          detail += ", ";
        detail += group.labels[index];
      }
    }
    push_overview_row(view, "Resources", std::move(label), std::move(detail));
  }

  for (auto const& name : overview.skill_names)
    push_overview_row(view, "Skills", name);
  for (auto const& name : overview.prompt_command_names)
    push_overview_row(view, "Prompt commands", name);
  for (auto const& id : overview.plugin_ids)
    push_overview_row(view, "Plugins", id);
  if (overview.plugin_resource_failure_count && (*overview.plugin_resource_failure_count > 0 || overview.plugin_resource_failure_count_is_lower_bound))
  {
    push_overview_row(view, "Plugins", "Resource failures",
                      format_bounded_count(*overview.plugin_resource_failure_count, overview.plugin_resource_failure_count_is_lower_bound));
  }

  // MCP/LSP statuses are not retained path-free at startup; omit rather than claim counts.

  for (auto const& hint : overview.key_hints)
  {
    if (hint.label.empty())
      continue;
    push_overview_row(view, "Keys", hint.label, {}, hint.keys);
  }

  if (view.items.empty())
    push_overview_row(view, "Runtime", "Status", "no retained startup resources");
  return view;
}

SelectListView settings_select_list_view(ComposerSnapshot const& snapshot, std::string footer_hint)
{
  return settings_select_list_view(snapshot, default_key_bindings(), std::move(footer_hint));
}

SelectListView settings_select_list_view(ComposerSnapshot const& snapshot, TuiKeyBindings const& bindings, std::string footer_hint)
{
  return settings_select_list_view_for_section(SettingsSection::Root, snapshot, bindings, std::move(footer_hint));
}

SelectListView settings_select_list_view_for_section(SettingsSection section, ComposerSnapshot const& snapshot, TuiKeyBindings const& bindings,
                                                     std::string footer_hint)
{
  return runtime_views::build_settings_select_list_view_for_section(section, snapshot, bindings, std::move(footer_hint));
}

}  // namespace ava::tui
