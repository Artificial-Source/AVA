#include "sys.h"
#include "ava/tui/command_output.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_active_run_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/runtime_mermaid_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_plugin_ui_internal.h"
#include "ava/tui/runtime_prompts_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_submit_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/runtime_user_turn_selection_internal.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/tui/session_grants.h"
#include "ava/tui/terminal.h"
#include "ava/tui/theme.h"
#include "ava/tui/tool_cards.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {
using runtime_input::printable_jump_target;
using runtime_input::read_curses_input_with_timeout;
using runtime_transcript::copy_text_from_answer;
using runtime_transcript::copy_text_to_terminal_clipboard;
using runtime_transcript::push_history;
using runtime_views::active_run_hint_for;
using runtime_views::branch_summary_callback_failure_status;
using runtime_views::branch_summary_input_intent;
using runtime_views::branch_summary_operation_view;
using runtime_views::branch_summary_terminal;
using runtime_views::branch_summary_terminal_status;
using runtime_views::BranchSummaryCallbackFailure;
using runtime_views::BranchSummaryInputIntent;
using runtime_views::DisplayPresentationBaseline;
using runtime_views::kSettingsDraftDetails;
using runtime_views::kSettingsDraftJobs;
using runtime_views::kSettingsDraftMcp;
using runtime_views::kSettingsDraftPermissions;
using runtime_views::kSettingsDraftPlugins;
using runtime_views::kSettingsDraftSessions;
using runtime_views::kSettingsDraftThinking;
using runtime_views::kSettingsDraftTools;
using runtime_views::kSettingsEditKeybindings;
using runtime_views::kSettingsImageWidthPrefix;
using runtime_views::kSettingsOpenKeybindings;
using runtime_views::kSettingsOpenModels;
using runtime_views::kSettingsOpenReasoning;
using runtime_views::kSettingsOpenScopedModels;
using runtime_views::kSettingsReloadKeybindings;
using runtime_views::reapply_settings_preview_after_display_reload;
using runtime_views::reselect_settings_display_row_after_rebuild;
using runtime_views::settings_action_is_previewable;
using runtime_views::settings_action_is_section;
using runtime_views::settings_preview_overlay_for_action;
using runtime_views::settings_section_for_action;
using runtime_views::SettingsNavigationState;

namespace {

constexpr std::size_t kKeyboardScrollRows = 3;
constexpr auto kIdleInputPollDelay = std::chrono::milliseconds(250);
constexpr auto kTerminalBackgroundProbeDeadline = std::chrono::milliseconds(50);

class RuntimeBeforeShutdownGuard
{
 public:
  explicit RuntimeBeforeShutdownGuard(TuiRuntimeOptions& options) : options_(options) { }
  RuntimeBeforeShutdownGuard(RuntimeBeforeShutdownGuard const&) = delete;
  RuntimeBeforeShutdownGuard& operator=(RuntimeBeforeShutdownGuard const&) = delete;
  ~RuntimeBeforeShutdownGuard() { static_cast<void>(invoke()); }

  [[nodiscard]] bool invoke() noexcept
  {
    if (invoked_)
      return true;
    invoked_ = true;
    if (!options_.on_before_tui_shutdown)
      return true;
    try
    {
      options_.on_before_tui_shutdown();
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  TuiRuntimeOptions& options_;
  bool invoked_ = false;
};

class ComposerTerminalGraphicsGuard
{
 public:
  ComposerTerminalGraphicsGuard() = default;
  ComposerTerminalGraphicsGuard(ComposerTerminalGraphicsGuard const&) = delete;
  ComposerTerminalGraphicsGuard& operator=(ComposerTerminalGraphicsGuard const&) = delete;
  ~ComposerTerminalGraphicsGuard() { detail::clear_composer_terminal_graphics(); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Session-scoped OSC 11 detection and startup-input FIFO ownership. Reset before
// each interactive TUI session and again on every exit so sequential sessions and
// tests never inherit a prior probe result or queued startup events.
class TerminalBackgroundDetectionGuard
{
 public:
  TerminalBackgroundDetectionGuard()
  {
    disarm_terminal_background_response_handler();
    reset_detected_terminal_background_appearance();
    runtime_input::clear_startup_input_queue();
  }

  TerminalBackgroundDetectionGuard(TerminalBackgroundDetectionGuard const&) = delete;
  TerminalBackgroundDetectionGuard& operator=(TerminalBackgroundDetectionGuard const&) = delete;

  ~TerminalBackgroundDetectionGuard()
  {
    disarm_terminal_background_response_handler();
    reset_detected_terminal_background_appearance();
    runtime_input::clear_startup_input_queue();
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

std::optional<std::string_view> optional_environment_view(char const* name)
{
  auto const* value = std::getenv(name);
  if (value == nullptr)
    return std::nullopt;
  return std::string_view(value);
}

void maybe_probe_terminal_background_appearance()
{
  if (!tui_theme_needs_terminal_background_probe())
    return;

  auto const tmux = optional_environment_view("TMUX");
  auto const term = optional_environment_view("TERM");
  if (!terminal_background_probe_environment_allows_query(tmux, term))
    return;

  arm_terminal_background_response_handler();
  if (write_terminal_background_query(stdout))
    runtime_input::drain_startup_probe_input(kTerminalBackgroundProbeDeadline);
  disarm_terminal_background_response_handler();
}

}  // namespace

void apply_reasoning_cycle_success(ComposerSnapshot& snapshot, std::string feedback)
{
  snapshot.status.clear();
  snapshot.reasoning_feedback = std::move(feedback);
}

void clear_reasoning_feedback_for_user_input(ComposerSnapshot& snapshot)
{
  snapshot.reasoning_feedback.reset();
  snapshot.local_command_feedback.reset();
}

void do_beep(terminal::Context& terminal_context)
{
  terminal_context.beep();
}

int run_interactive_composer(TuiRuntimeOptions options)
{
  RuntimeBeforeShutdownGuard before_shutdown(options);
  if (!terminal_is_tty())
  {
    std::cerr << "interactive TUI requires stdin and stdout to be terminals\n";
    return 1;
  }

  core::Application& application = core::Application::instance();
  terminal::Context& terminal_context = application.terminal_context();
  core::Signals& signals_manager = application.signals_manager();

  terminal_context.initialize();

  // Activate signal handlers for the TUI.
  signals_manager.activate_handlers({SIGTERM, SIGINT});
  signals_manager.default_handlers({SIGHUP});

  ComposerTerminalGraphicsGuard graphics_cleanup;
  apply_terminal_cursor_settings(options.cursor);
  // Probe the direct terminal background once after enter and before first paint.
  // No re-probe on suspend/resume and no late theme flip after presentation starts.
  TerminalBackgroundDetectionGuard terminal_background_detection;
  maybe_probe_terminal_background_appearance();

  RuntimePresentationState presentation_state(options);
  auto& snapshot = presentation_state.snapshot;
  auto& sidebar = presentation_state.sidebar;
  auto& command_session_grants = presentation_state.command_session_grants;
  RuntimePluginUiCoordinator plugin_ui;
  struct PluginUiShutdownGuard
  {
    RuntimePluginUiCoordinator* coordinator = nullptr;
    ComposerSnapshot* snapshot = nullptr;
    ~PluginUiShutdownGuard()
    {
      if (coordinator && snapshot)
        coordinator->shutdown(*snapshot);
    }
  } plugin_ui_shutdown{&plugin_ui, &snapshot};

  auto refresh_token_status = [&]() { presentation_state.refresh_token_status(options); };
  auto refresh_active_context_status = [&]() { presentation_state.refresh_active_context_status(options); };
  auto refresh_reasoning_status = [&]() { presentation_state.refresh_reasoning_status(options); };
  refresh_token_status();
  refresh_active_context_status();
  refresh_reasoning_status();

  bool terminal_write_failed = false;
  bool terminal_signal_received = false;
  RuntimeDraftState draft_state;
  auto& input_history = draft_state.input_history;
  auto& history_index = draft_state.history_index;
  auto& draft_input = draft_state.draft_input;
  auto& draft = draft_state.draft;
  auto& jump_mode = draft_state.jump_mode;
  auto& selected_slash_command_index = draft_state.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state.path_completion_force_active;
  auto& draft_scroll_offset = draft_state.draft_scroll_offset;
  auto& draft_selection_anchor = draft_state.draft_selection_anchor;
  auto& draft_selection_cursor = draft_state.draft_selection_cursor;
  auto& pending_escape_clear = draft_state.pending_escape_clear;
  RuntimeRenderer renderer(snapshot, sidebar, draft_state);
  RuntimeNavigationController navigation(options, snapshot, sidebar, draft_state, renderer);
  auto& transcript_scroll_offset = renderer.transcript_scroll_offset;
  auto& completion_cache = renderer.completion_cache;
  ActiveSelectList active_select_list = ActiveSelectList::None;
  RuntimePromptStashController prompt_stash(presentation_state, draft_state, renderer, active_select_list, options.key_bindings);
  struct BranchSummaryUiState
  {
    bool active = false;
    bool exit_requested = false;
    std::uint64_t generation = 0;
    TuiBranchSummaryPhase phase = TuiBranchSummaryPhase::Idle;
    SelectListView prior_session_view;
    std::size_t prior_selected_index = 0;
    std::string selected_source_value;
  } branch_summary_ui;
  bool branch_summary_exit_ready = false;
  auto apply_runtime_state_snapshot = [&](TuiRuntimeStateSnapshot state) {
    // Shared path with active-run: apply DTO then rebuild any open overview in place.
    apply_runtime_state_snapshot_with_overview_sync(options, presentation_state, active_select_list, std::move(state));
  };
  SettingsNavigationState settings_nav;
  bool settings_session_open = false;
  struct ClearThemePreviewOnExit
  {
    ~ClearThemePreviewOnExit() { clear_tui_theme_preview(); }
  } clear_theme_preview_on_exit;
  auto clear_settings_preview = [&]() {
    settings_nav.preview.cancel();
    settings_nav.preview.apply_image_overlay(snapshot);
  };
  auto begin_settings_preview_baseline = [&]() {
    settings_nav.preview.begin(
        DisplayPresentationBaseline{.show_images = snapshot.show_images, .image_width_cells = snapshot.image_width_cells, .cursor = snapshot.cursor});
  };
  auto ensure_settings_session = [&]() {
    if (settings_session_open)
      return;
    settings_nav.reset();
    begin_settings_preview_baseline();
    settings_session_open = true;
  };
  auto rebuild_settings_view = [&](SettingsSection section, std::string query = {}, std::optional<std::size_t> selected = std::nullopt) {
    ensure_settings_session();
    auto settings_snapshot = snapshot;
    settings_snapshot.sidebar = sidebar;
    // Build rows from authoritative presentation so preview overlays do not rewrite candidate labels.
    settings_snapshot.show_images = settings_nav.preview.authoritative.show_images;
    settings_snapshot.image_width_cells = settings_nav.preview.authoritative.image_width_cells;
    settings_snapshot.cursor = settings_nav.preview.authoritative.cursor;
    // active_tui_theme() consults the presentation overlay; clear it only while constructing rows.
    auto const restage_theme_preview = settings_nav.preview.active();
    settings_nav.preview.clear_theme_overlay();
    auto view = settings_select_list_view_for_section(section, settings_snapshot, options.key_bindings);
    if (restage_theme_preview)
      settings_nav.preview.apply_theme_overlay();
    view.query = std::move(query);
    if (selected)
      view.selected_item_index = *selected;
    view.selected_item_index = clamp_select_list_selection(view, view.selected_item_index);
    snapshot.select_list = std::move(view);
    active_select_list = ActiveSelectList::Settings;
    settings_nav.section = section;
    settings_nav.preview.apply_image_overlay(snapshot);
  };
  auto apply_settings_highlight_preview = [&]() {
    if (active_select_list != ActiveSelectList::Settings || !snapshot.select_list || !settings_nav.in_display())
    {
      if (settings_nav.preview.active())
      {
        settings_nav.preview.cancel();
        settings_nav.preview.apply_image_overlay(snapshot);
      }
      return;
    }
    auto const index = snapshot.select_list->selected_item_index;
    if (index >= snapshot.select_list->items.size())
    {
      settings_nav.preview.cancel();
      settings_nav.preview.apply_image_overlay(snapshot);
      return;
    }
    auto const& item = snapshot.select_list->items[index];
    if (auto overlay = settings_preview_overlay_for_action(item.value, snapshot))
      settings_nav.preview.update(std::move(*overlay));
    else
      settings_nav.preview.cancel();
    settings_nav.preview.apply_image_overlay(snapshot);
  };
  auto close_settings = [&]() {
    snapshot.select_list.reset();
    active_select_list = ActiveSelectList::None;
    clear_settings_preview();
    settings_nav.reset();
    settings_session_open = false;
  };
  TranscriptSearchController transcript_search(presentation_state, renderer, navigation, active_select_list);
  std::optional<PendingSessionArchiveAction> session_archive_confirmation;
  RuntimePromptCoordinator prompt_coordinator(options, snapshot, command_session_grants, renderer, &active_select_list);
  [[maybe_unused]] auto permission_resolver = prompt_coordinator.permission_resolver();
  [[maybe_unused]] auto question_resolver = prompt_coordinator.question_resolver();
  auto render = [&]() -> bool { return renderer.render(); };

  auto restore_branch_summary_selector = [&](SelectListView view) {
    snapshot.select_list = runtime_views::restore_branch_summary_session_view(std::move(view), branch_summary_ui.prior_session_view,
                                                                              branch_summary_ui.selected_source_value, branch_summary_ui.prior_selected_index);
    active_select_list = ActiveSelectList::Session;
  };
  auto present_branch_summary_status = [&](std::string status) {
    snapshot.status = status;
    if (snapshot.select_list)
      snapshot.select_list->subtitle = std::move(status);
  };
  auto settle_branch_summary = [&](TuiBranchSummarySnapshot const& operation) -> bool {
    auto restored = branch_summary_ui.prior_session_view;
    bool refresh_failed = false;
    if (operation.refresh_required)
    {
      if (!options.on_branch_summary_refresh_catalog)
      {
        refresh_failed = true;
      }
      else
      {
        try
        {
          auto refreshed = options.on_branch_summary_refresh_catalog();
          if (refreshed)
            restored = std::move(*refreshed);
          else
            refresh_failed = true;
        }
        catch (...)
        {
          refresh_failed = true;
        }
      }
    }
    auto const exit_requested = branch_summary_ui.exit_requested;
    restore_branch_summary_selector(std::move(restored));
    present_branch_summary_status(refresh_failed ? std::string(branch_summary_callback_failure_status(BranchSummaryCallbackFailure::Refresh))
                                                 : branch_summary_terminal_status(operation));
    branch_summary_ui.active = false;
    branch_summary_ui.phase = operation.phase;
    if (exit_requested)
    {
      snapshot.select_list.reset();
      active_select_list = ActiveSelectList::None;
      branch_summary_exit_ready = true;
      return true;
    }
    return renderer.request_render();
  };
  auto apply_branch_summary_snapshot = [&](TuiBranchSummarySnapshot const& operation) -> bool {
    if (!branch_summary_ui.active || operation.generation != branch_summary_ui.generation)
      return true;
    branch_summary_ui.phase = operation.phase;
    if (branch_summary_terminal(operation))
      return settle_branch_summary(operation);
    snapshot.select_list = branch_summary_operation_view(operation);
    active_select_list = ActiveSelectList::BranchSummary;
    return renderer.request_render();
  };
  auto poll_branch_summary = [&]() -> bool {
    if (!branch_summary_ui.active)
      return true;
    if (!options.branch_summary_snapshot)
    {
      TuiBranchSummarySnapshot failed;
      failed.generation = branch_summary_ui.generation;
      failed.phase = TuiBranchSummaryPhase::Failed;
      failed.failure_code = TuiBranchSummaryFailureCode::Internal;
      auto settled = settle_branch_summary(failed);
      present_branch_summary_status(std::string(branch_summary_callback_failure_status(BranchSummaryCallbackFailure::Snapshot)));
      return settled;
    }
    try
    {
      return apply_branch_summary_snapshot(options.branch_summary_snapshot());
    }
    catch (...)
    {
      TuiBranchSummarySnapshot failed;
      failed.generation = branch_summary_ui.generation;
      failed.phase = TuiBranchSummaryPhase::Failed;
      failed.failure_code = TuiBranchSummaryFailureCode::Internal;
      auto settled = settle_branch_summary(failed);
      present_branch_summary_status(std::string(branch_summary_callback_failure_status(BranchSummaryCallbackFailure::Snapshot)));
      return settled;
    }
  };
  auto begin_branch_summary = [&](SelectListView const& session_view, std::size_t selected_index, std::string selected_value) -> bool {
    if (!options.on_branch_summary_prepare || !options.branch_summary_snapshot || !options.on_branch_summary_confirm || !options.on_branch_summary_cancel ||
        !options.on_branch_summary_refresh_catalog)
    {
      present_branch_summary_status("parent summary is unavailable");
      return renderer.request_render();
    }
    ava::core::Result<TuiBranchSummarySnapshot> prepared =
        std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "parent summary prepare callback failed"));
    try
    {
      prepared = options.on_branch_summary_prepare(selected_value);
    }
    catch (...)
    {
    }
    if (!prepared)
    {
      present_branch_summary_status(std::string(branch_summary_callback_failure_status(BranchSummaryCallbackFailure::Prepare)));
      return renderer.request_render();
    }
    branch_summary_ui.active = true;
    branch_summary_ui.exit_requested = false;
    branch_summary_ui.generation = prepared->generation;
    branch_summary_ui.phase = prepared->phase;
    branch_summary_ui.prior_session_view = session_view;
    branch_summary_ui.prior_selected_index = selected_index;
    branch_summary_ui.selected_source_value = std::move(selected_value);
    return apply_branch_summary_snapshot(*prepared);
  };
  auto cancel_branch_summary = [&](bool exit_requested) -> bool {
    if (!branch_summary_ui.active)
      return true;
    branch_summary_ui.exit_requested = branch_summary_ui.exit_requested || exit_requested;
    ava::core::Result<bool> canceled = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "cancel callback unavailable"));
    try
    {
      if (options.on_branch_summary_cancel)
        canceled = options.on_branch_summary_cancel(branch_summary_ui.generation);
    }
    catch (...)
    {
    }
    if (!canceled || !*canceled)
    {
      present_branch_summary_status(std::string(branch_summary_callback_failure_status(BranchSummaryCallbackFailure::Cancel)));
      return renderer.request_render();
    }
    snapshot.status = "canceling parent summary";
    return poll_branch_summary();
  };
  auto confirm_branch_summary = [&]() -> bool {
    if (!branch_summary_ui.active || branch_summary_ui.phase != TuiBranchSummaryPhase::AwaitingConfirmation)
      return true;
    ava::core::Result<bool> confirmed = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "confirm callback unavailable"));
    try
    {
      if (options.on_branch_summary_confirm)
        confirmed = options.on_branch_summary_confirm(branch_summary_ui.generation);
    }
    catch (...)
    {
    }
    if (!confirmed || !*confirmed)
    {
      present_branch_summary_status(std::string(branch_summary_callback_failure_status(BranchSummaryCallbackFailure::Confirm)));
      return renderer.request_render();
    }
    snapshot.status = "parent summary generation started";
    return poll_branch_summary();
  };

  RuntimeMermaidPresentationController mermaid_presentation(options.mermaid_render);
  auto service_mermaid_presentation = [&]() -> bool {
    auto old_anchor = detail::TranscriptViewportAnchor{};
    auto const preserve_viewport = transcript_scroll_offset > 0;
    if (preserve_viewport && !renderer.has_deferred_detached_transcript_update())
    {
      auto const old_max_scroll =
          detail::composer_max_transcript_scroll_offset_cached(snapshot, snapshot.width, snapshot.height, completion_cache, snapshot.file_references_generation,
                                                               renderer.transcript_layout_cache, snapshot.transcript_generation);
      old_anchor = detail::capture_transcript_viewport_anchor(renderer.transcript_layout_cache.layout, old_max_scroll, transcript_scroll_offset);
    }
    auto const update = mermaid_presentation.service(snapshot);
    if (!update.visual_changed)
      return true;
    transcript_search.refresh_after_transcript_mutation(0, update.earliest_changed_item);
    if (preserve_viewport)
    {
      renderer.defer_detached_transcript_update(old_anchor, 0);
      renderer.synchronize_detached_transcript_layout();
    }
    return renderer.request_render(FrameRenderKind::Full);
  };

  RuntimeActionController action_controller(options, presentation_state, draft_state, renderer, active_select_list, session_archive_confirmation);
  RuntimeSubagentWorkspaceController subagent_workspace(options, snapshot);
  RuntimeActiveRunController active_run_controller(options, presentation_state, draft_state, renderer, prompt_coordinator, prompt_stash, plugin_ui, navigation,
                                                   action_controller, transcript_search, subagent_workspace, service_mermaid_presentation);
  auto maybe_reload_display_settings = [&]() -> bool {
    auto const outcome = action_controller.maybe_reload_display_settings();
    if (outcome == DisplaySettingsReloadPollOutcome::TerminalFailure)
      return false;
    if (outcome != DisplaySettingsReloadPollOutcome::Applied)
      return true;

    // Applied is an explicit app signal. Always rebase/reapply while settings are open — never
    // infer from post-hydration value equality (overlay can mask true→false authority changes).
    if (settings_session_open && active_select_list == ActiveSelectList::Settings)
    {
      // Capture the actionable selection identity before hydration rebuild can shift indexes.
      std::string selected_action_value;
      std::optional<std::size_t> prior_selected_index;
      std::string query;
      std::string staged_overlay_action;
      if (snapshot.select_list)
      {
        query = snapshot.select_list->query;
        prior_selected_index = snapshot.select_list->selected_item_index;
        if (*prior_selected_index < snapshot.select_list->items.size())
        {
          auto const& item = snapshot.select_list->items[*prior_selected_index];
          if (!item.value.empty())
            selected_action_value = item.value;
        }
      }
      if (settings_nav.preview.overlay && !settings_nav.preview.overlay->action_token.empty())
        staged_overlay_action = settings_nav.preview.overlay->action_token;

      reapply_settings_preview_after_display_reload(settings_nav.preview, snapshot);
      if (settings_nav.in_display() && snapshot.select_list)
      {
        rebuild_settings_view(settings_nav.section, query, std::nullopt);
        if (snapshot.select_list)
        {
          snapshot.select_list->selected_item_index =
              reselect_settings_display_row_after_rebuild(*snapshot.select_list, selected_action_value, staged_overlay_action, prior_selected_index);
          // Keep overlay and Enter target aligned with the restored row identity.
          apply_settings_highlight_preview();
        }
      }
    }
    // Paint exactly once after overlay staging so the first frame is the final staged state.
    return render();
  };
  auto clear_draft_for_interrupt = [&]() { return action_controller.clear_draft_for_interrupt(); };
  auto open_external_editor = [&]() -> bool { return action_controller.open_external_editor(); };
  auto suspend_to_background = [&]() -> bool { return action_controller.suspend_to_background(); };
  auto paste_clipboard_image = [&]() -> bool { return action_controller.paste_clipboard_image(); };
  auto cycle_reasoning = [&]() { action_controller.cycle_reasoning(); };
  auto toggle_thinking_visibility = [&]() { action_controller.toggle_thinking_visibility(); };
  auto open_model_selector = [&]() -> bool { return action_controller.open_model_selector(); };
  auto open_reasoning_selector = [&](bool chained = false) -> bool { return action_controller.open_reasoning_selector(chained); };
  auto open_scoped_model_selector = [&]() -> bool { return action_controller.open_scoped_model_selector(); };
  auto open_session_selector = [&]() -> bool { return action_controller.open_session_selector(); };
  auto toggle_startup_overview = [&]() -> bool {
    if (settings_session_open)
      close_settings();
    return action_controller.toggle_startup_overview();
  };
  auto cycle_model = [&](bool forward) { action_controller.cycle_model(forward); };

  auto slash_palette_active = [&]() { return navigation.slash_palette_active(); };
  auto file_reference_palette_active = [&]() { return navigation.file_reference_palette_active(); };
  auto path_completion_palette_active = [&]() { return navigation.path_completion_palette_active(); };
  auto completion_match_count = [&]() { return navigation.completion_match_count(); };
  auto clamp_completion = [&](std::size_t selected) { return navigation.clamp_completion(selected); };
  auto previous_completion = [&](std::size_t selected) { return navigation.previous_completion(selected); };
  auto next_completion = [&](std::size_t selected) { return navigation.next_completion(selected); };
  auto selected_completion_disabled_reason = [&](std::size_t selected) { return navigation.selected_completion_disabled_reason(selected); };
  auto selected_completion_text = [&](std::size_t selected) { return navigation.selected_completion_text(selected); };

  auto scroll_up = [&](std::size_t amount) { navigation.scroll_up(amount); };
  auto scroll_down = [&](std::size_t amount) { navigation.scroll_down(amount); };
  auto toggle_tool_details_at = [&](std::size_t item_index) { return navigation.toggle_tool_details_at(item_index); };
  auto toggle_thinking_at = [&](std::size_t item_index) { return navigation.toggle_thinking_at(item_index); };

  auto handle_sidebar_drawer_input = [&](InputEvent const& event) -> std::optional<bool> { return navigation.handle_sidebar_drawer_input(event); };

  auto jump_to_bottom = [&](std::string status) { navigation.jump_to_bottom(std::move(status)); };
  auto scroll_to_message_boundary = [&](bool previous) { navigation.scroll_to_message_boundary(previous); };

  RuntimeSubmitController submit_controller(options, presentation_state, draft_state, renderer, navigation, action_controller, active_run_controller,
                                            prompt_stash, transcript_search, subagent_workspace, active_select_list);
  auto handle_submit = [&](std::optional<std::string> forced_submission = std::nullopt) {
    auto const outcome = submit_controller.submit(std::move(forced_submission));
    terminal_write_failed = outcome.terminal_write_failed;
    terminal_signal_received = outcome.terminal_signal_received;
    // outcome.terminal_signal_received an only be true if outcome.disposition is set to BreakLoop.
    ASSERT(!terminal_signal_received || outcome.disposition == RuntimeSubmitDisposition::BreakLoop);
    return outcome.disposition;
  };

  using Signals = core::Signals;
  if (Signals::received(terminal_signals))
    return 130;
  if (!service_mermaid_presentation() || !render())
    return 1;

  while (true)
  {
    if (!service_mermaid_presentation())
    {
      terminal_write_failed = true;
      break;
    }
    if (!poll_branch_summary())
    {
      terminal_write_failed = true;
      break;
    }
    if (branch_summary_exit_ready)
      break;
    if (!renderer.flush_pending_render_if_due())
    {
      terminal_write_failed = true;
      break;
    }
    auto const input_poll_started_at = std::chrono::steady_clock::now();
    if (subagent_workspace.poll(input_poll_started_at) && !renderer.request_render())
    {
      terminal_write_failed = true;
      break;
    }
    auto input_poll_delay = kIdleInputPollDelay;
    if (auto workspace_wait = subagent_workspace.time_until_poll(input_poll_started_at))
      input_poll_delay = std::min(input_poll_delay, std::chrono::ceil<std::chrono::milliseconds>(*workspace_wait));
    if (renderer.has_pending_render())
    {
      input_poll_delay = std::min(input_poll_delay, std::chrono::ceil<std::chrono::milliseconds>(renderer.time_until_pending_render()));
    }
    auto const maybe_input = read_curses_input_with_timeout(input_poll_delay);
    if (!maybe_input)
    {
      if (subagent_workspace.poll() && !renderer.request_render())
      {
        terminal_write_failed = true;
        break;
      }
      if (!renderer.flush_pending_render_if_due() || !maybe_reload_display_settings())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    auto const input = *maybe_input;
    if (Signals::received(terminal_signals))
    {
      bool const exit_requested = Signals::try_obtain(SIGTERM);
      if (exit_requested)
        terminal_signal_received = true;

      // ┌─────────────────────┬─────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────┐
      // │Successfully claimed │UI state                                 │Intended action                                                      │
      // ├─────────────────────┼─────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┤
      // │SIGTERM or SIGINT    │Active branch summary                    │Request cancellation; additionally request eventual exit for SIGTERM │
      // ├─────────────────────┼─────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┤
      // │SIGTERM              │No active branch summary                 │Leave the loop (break), regardless of draft contents                 │
      // ├─────────────────────┼─────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┤
      // │SIGINT               │No active branch summary, empty draft    │Leave the loop too (break)                                           │
      // ├─────────────────────┼─────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┤
      // │SIGINT               │No active branch summary, nonempty draft │Clear draft and continue                                             │
      // └─────────────────────┴─────────────────────────────────────────┴─────────────────────────────────────────────────────────────────────┘
      if (branch_summary_ui.active)
      {
        if (AI_LIKELY(exit_requested || Signals::try_obtain(SIGINT)))
        {
          // We have an active branch summary and successfully claimed either terminal signal.
          // Request cancellation; additionally request eventual exit for SIGTERM.
          if (!cancel_branch_summary(exit_requested))
          {
            terminal_write_failed = true;
            break;
          }
          continue;
        }
        // We failed to claim either terminal signal; fall-through.
      }
      else if (exit_requested)
      {
        // Claimed SIGTERM and no active branch summary. Leave the loop, regardless of draft contents.
        break;
      }
      else if (Signals::try_obtain(SIGINT))
      {
        // Claimed SIGINT, no active branch summary
        if (draft.text.empty())
        {
          // and an empty draft. Leave the loop too.
          terminal_signal_received = true;
          break;
        }
        // and nonempty draft. Clear draft and continue if successful.
        static_cast<void>(clear_draft_for_interrupt());
        snapshot.selected_slash_command_index = selected_slash_command_index;
        if (!render())
        {
          terminal_write_failed = true;
          break;
        }
        continue;
      }
      // Failed to claim either terminal signal; fall-through.
    }
    if (input.resize)
    {
      renderer.wheel_governor.reset();
      transcript_search.refresh_after_resize();
      if (!render())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    if (!runtime_wheel_input_accepted(renderer.wheel_governor, input.event.key))
      continue;
    if (snapshot.command_output && !snapshot.permission_prompt && !snapshot.question_prompt)
    {
      auto const geometry = command_output_geometry(snapshot.width, snapshot.height);
      auto const output_input = handle_command_output_input(*snapshot.command_output, input.event, geometry.width, geometry.height, snapshot.tool_presentation);
      if (output_input.action == CommandOutputInputAction::Dismiss)
      {
        snapshot.command_output.reset();
        snapshot.status = "command output closed";
        if (!renderer.request_render())
        {
          terminal_write_failed = true;
          break;
        }
        continue;
      }
      if (output_input.action == CommandOutputInputAction::Redraw)
      {
        snapshot.command_output->scroll_offset = output_input.scroll_offset;
        if (!renderer.request_render())
        {
          terminal_write_failed = true;
          break;
        }
        continue;
      }
      // Typing or clearing starts the next draft directly; pointer input remains
      // captured by the modal so transcript/composer hit targets stay suppressed.
      if (input.event.key == Key::Character || input.event.key == Key::Space || input.event.key == Key::CtrlU)
      {
        snapshot.command_output.reset();
        snapshot.status.clear();
      }
      else
      {
        continue;
      }
    }
    if (subagent_workspace.active())
    {
      auto const handled = subagent_workspace.handle_input(input.event);
      if (handled.beep)
        do_beep(terminal_context);
      if (handled.changed && !renderer.request_render())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    if (branch_summary_ui.active)
    {
      bool success = true;
      switch (branch_summary_input_intent(branch_summary_ui.phase, input.event, options.key_bindings))
      {
        case BranchSummaryInputIntent::Exit:
          success = cancel_branch_summary(true);
          break;
        case BranchSummaryInputIntent::Cancel:
          success = cancel_branch_summary(false);
          break;
        case BranchSummaryInputIntent::Confirm:
          success = confirm_branch_summary();
          break;
        case BranchSummaryInputIntent::Block:
          do_beep(terminal_context);
          break;
      }
      if (!success)
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    clear_reasoning_feedback_for_user_input(snapshot);
    if (auto handled = transcript_search.handle_input(input.event))
    {
      if (!*handled)
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    if (auto handled = prompt_stash.handle_selector_input(input.event))
    {
      if (!*handled)
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    if (snapshot.select_list)
    {
      if (active_select_list == ActiveSelectList::Settings)
        ensure_settings_session();
      else if (settings_session_open)
      {
        // Another selector replaced settings: drop any presentation-only preview.
        clear_settings_preview();
        settings_nav.reset();
        settings_session_open = false;
      }
      auto input_result = [&]() {
        if (input.event.key == Key::MouseLeftPress || input.event.key == Key::MouseLeftClick)
        {
          if (auto const clicked = select_list_selection_for_screen_position(snapshot, input.event.mouse_row, input.event.mouse_column))
          {
            // Settings mouse clicks change selection/highlight only; Enter confirms.
            // Other selectors keep existing mouse-confirm behavior.
            auto action = active_select_list == ActiveSelectList::Settings ? SelectListInputAction::Redraw : SelectListInputAction::Resolve;
            if (*clicked >= snapshot.select_list->items.size() || !snapshot.select_list->items[*clicked].enabled)
            {
              action = SelectListInputAction::Redraw;
            }
            return SelectListInputResult{.selected_item_index = *clicked, .query = snapshot.select_list->query, .action = action};
          }
        }
        if (active_select_list == ActiveSelectList::ScopedModels)
        {
          auto const scoped_action = [&](TuiAction action) { return key_matches_action(options.key_bindings, action, input.event.key); };
          if (scoped_action(TuiAction::ModelsSave))
          {
            return SelectListInputResult{.selected_item_index = snapshot.select_list->selected_item_index,
                                         .query = snapshot.select_list->query,
                                         .action = SelectListInputAction::ModelsSave};
          }
          if (scoped_action(TuiAction::ModelsEnableAll))
          {
            return SelectListInputResult{.selected_item_index = snapshot.select_list->selected_item_index,
                                         .query = snapshot.select_list->query,
                                         .action = SelectListInputAction::ModelsEnableAll};
          }
          if (scoped_action(TuiAction::ModelsClearAll))
          {
            return SelectListInputResult{.selected_item_index = snapshot.select_list->selected_item_index,
                                         .query = snapshot.select_list->query,
                                         .action = SelectListInputAction::ModelsClearAll};
          }
          if (scoped_action(TuiAction::ModelsToggleProvider))
          {
            return SelectListInputResult{.selected_item_index = snapshot.select_list->selected_item_index,
                                         .query = snapshot.select_list->query,
                                         .action = SelectListInputAction::ModelsToggleProvider};
          }
          if (scoped_action(TuiAction::ModelsReorderUp))
          {
            return SelectListInputResult{.selected_item_index = snapshot.select_list->selected_item_index,
                                         .query = snapshot.select_list->query,
                                         .action = SelectListInputAction::ModelsReorderUp};
          }
          if (scoped_action(TuiAction::ModelsReorderDown))
          {
            return SelectListInputResult{.selected_item_index = snapshot.select_list->selected_item_index,
                                         .query = snapshot.select_list->query,
                                         .action = SelectListInputAction::ModelsReorderDown};
          }
        }
        return handle_select_list_input(*snapshot.select_list, input.event, options.key_bindings);
      }();
      auto preserve_session_selector_state = [&](SelectListView next_view, std::string status) {
        session_archive_confirmation.reset();
        auto const query = input_result.query;
        std::string selected_value;
        if (input_result.selected_item_index < snapshot.select_list->items.size())
        {
          selected_value = snapshot.select_list->items[input_result.selected_item_index].value;
        }
        next_view.query = query;
        if (!selected_value.empty())
        {
          for (std::size_t index = 0; index < next_view.items.size(); ++index)
          {
            if (next_view.items[index].value == selected_value)
            {
              next_view.selected_item_index = index;
              break;
            }
          }
        }
        next_view.selected_item_index = clamp_select_list_selection(next_view, next_view.selected_item_index);
        snapshot.select_list = std::move(next_view);
        snapshot.status = std::move(status);
      };
      auto selected_select_list_item = [&]() -> SelectListItemView const* {
        if (!snapshot.select_list || input_result.selected_item_index >= snapshot.select_list->items.size())
          return nullptr;
        return &snapshot.select_list->items[input_result.selected_item_index];
      };
      auto visible_select_list_values = [&]() {
        std::vector<std::string> values;
        if (!snapshot.select_list)
          return values;
        auto current = *snapshot.select_list;
        current.query = input_result.query;
        current.selected_item_index = input_result.selected_item_index;
        for (auto const index : filter_select_list_items(current))
        {
          if (index < current.items.size() && current.items[index].enabled && !current.items[index].value.empty())
          {
            values.push_back(current.items[index].value);
          }
        }
        return values;
      };
      auto apply_opened_session_snapshot = [&](TuiRuntimeStateSnapshot state, bool) {
        auto status = state.status;
        static_cast<void>(apply_runtime_state_snapshot_with_presentation_transition(options, presentation_state, draft_state, renderer, transcript_search,
                                                                                    subagent_workspace, active_select_list, std::move(state)));
        if (!status.empty())
          settle_local_command_status(snapshot, std::move(status));
      };
      if (input_result.action == SelectListInputAction::Redraw && snapshot.select_list)
      {
        session_archive_confirmation.reset();
        snapshot.select_list->selected_item_index = input_result.selected_item_index;
        snapshot.select_list->query = std::move(input_result.query);
        if (active_select_list == ActiveSelectList::Settings)
          apply_settings_highlight_preview();
      }
      else if (input_result.action == SelectListInputAction::Cancel && active_select_list == ActiveSelectList::Settings && snapshot.select_list)
      {
        // Esc in a section returns to root (restoring root query/selection when available).
        // Esc at root closes. If a section is open without a root frame (reload/confirm edge),
        // still pop to root rather than trapping the user in the section.
        if (!settings_nav.is_root())
        {
          std::string root_query;
          std::optional<std::size_t> root_selected;
          if (settings_nav.root_frame)
          {
            root_query = settings_nav.root_frame->query;
            root_selected = settings_nav.root_frame->selected_item_index;
          }
          settings_nav.root_frame.reset();
          clear_settings_preview();
          begin_settings_preview_baseline();
          rebuild_settings_view(SettingsSection::Root, std::move(root_query), root_selected);
          snapshot.status = "settings";
        }
        else
        {
          close_settings();
          snapshot.status = "view canceled";
        }
      }
      else if (input_result.action == SelectListInputAction::Resolve && active_select_list == ActiveSelectList::Settings && snapshot.select_list)
      {
        auto const* selected_item = selected_select_list_item();
        std::string selected_value = selected_item ? selected_item->value : std::string{};
        auto const current_query = input_result.query;
        auto const current_selected = input_result.selected_item_index;

        if (auto section = settings_section_for_action(selected_value))
        {
          // Section Enter rebuilds in place rather than generic teardown-first Resolve.
          settings_nav.root_frame =
              runtime_views::SettingsFrameState{.section = SettingsSection::Root, .query = current_query, .selected_item_index = current_selected};
          clear_settings_preview();
          begin_settings_preview_baseline();
          rebuild_settings_view(*section);
          snapshot.status = "settings section opened";
        }
        else if (selected_value == kSettingsOpenModels)
        {
          close_settings();
          static_cast<void>(open_model_selector());
        }
        else if (selected_value == kSettingsOpenScopedModels)
        {
          close_settings();
          static_cast<void>(open_scoped_model_selector());
        }
        else if (selected_value == kSettingsOpenReasoning)
        {
          close_settings();
          static_cast<void>(open_reasoning_selector(false));
        }
        else if (selected_value == kSettingsOpenKeybindings)
        {
          close_settings();
          snapshot.select_list = hotkeys_select_list_view(options.key_bindings);
          active_select_list = ActiveSelectList::Hotkeys;
          snapshot.status = "keybindings opened";
          transcript_scroll_offset = 0;
        }
        else if (selected_value == kSettingsEditKeybindings)
        {
          close_settings();
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, "/keybindings set "));
          selected_slash_command_index = 0;
          path_completion_force_active = false;
          draft_scroll_offset = 0;
          history_index.reset();
          draft_input.clear();
          snapshot.status = "keybinding edit command drafted";
        }
        else if (selected_value == kSettingsReloadKeybindings)
        {
          close_settings();
          if (!options.on_reload_key_bindings)
          {
            snapshot.status = "reload unavailable";
            do_beep(terminal_context);
          }
          else
          {
            auto reloaded = options.on_reload_key_bindings();
            if (!reloaded)
            {
              snapshot.status = reloaded.error().format();
              do_beep(terminal_context);
            }
            else
            {
              options.key_bindings = std::move(reloaded->key_bindings);
              snapshot.active_run_hint = active_run_hint_for(options.key_bindings);
              // Success feedback stays on the transient status surface from the applied DTO.
              apply_runtime_state_snapshot(std::move(reloaded->state));
            }
          }
        }
        else if (selected_value == kSettingsDraftPermissions || selected_value == kSettingsDraftTools || selected_value == kSettingsDraftPlugins ||
                 selected_value == kSettingsDraftMcp || selected_value == kSettingsDraftJobs || selected_value == kSettingsDraftSessions ||
                 selected_value == kSettingsDraftThinking || selected_value == kSettingsDraftDetails)
        {
          std::string draft_command = "/help";
          if (selected_value == kSettingsDraftPermissions)
            draft_command = "/permissions";
          else if (selected_value == kSettingsDraftTools)
            draft_command = "/details";
          else if (selected_value == kSettingsDraftPlugins)
            draft_command = "/plugins";
          else if (selected_value == kSettingsDraftMcp)
            draft_command = "/mcp";
          else if (selected_value == kSettingsDraftJobs)
            draft_command = "/jobs";
          else if (selected_value == kSettingsDraftSessions)
            draft_command = "/sessions";
          else if (selected_value == kSettingsDraftThinking)
            draft_command = "/thinking";
          else if (selected_value == kSettingsDraftDetails)
            draft_command = "/details";
          close_settings();
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, std::move(draft_command)));
          selected_slash_command_index = 0;
          path_completion_force_active = false;
          draft_scroll_offset = 0;
          history_index.reset();
          draft_input.clear();
          snapshot.status = "command drafted";
        }
        else if (selected_value.empty() || (selected_item && !selected_item->enabled))
        {
          snapshot.status = "settings action unavailable from this row";
          do_beep(terminal_context);
        }
        else if (options.on_settings_selected)
        {
          // Confirm persists exactly once through the app callback; preview is cleared first so
          // authoritative reload becomes the new baseline.
          auto const confirm_token = selected_value;
          clear_settings_preview();
          auto selected = options.on_settings_selected(confirm_token);
          if (selected)
          {
            auto status = selected->status;
            apply_runtime_state_snapshot(std::move(*selected));
            begin_settings_preview_baseline();
            if (settings_nav.in_display())
            {
              // Remain in the presentation section with refreshed authoritative rows after a
              // successful confirm. Feedback stays on the transient status surface; local
              // settings actions never append chat/transcript items.
              auto const receipt = status.empty() ? std::string("display setting saved") : status;
              rebuild_settings_view(settings_nav.section, current_query, current_selected);
              snapshot.status = receipt;
            }
            else
            {
              close_settings();
            }
          }
          else
          {
            // Error restores latest authoritative presentation and keeps the section open.
            begin_settings_preview_baseline();
            settings_nav.preview.apply_image_overlay(snapshot);
            snapshot.status = selected.error().format();
            do_beep(terminal_context);
          }
        }
        else
        {
          close_settings();
          snapshot.status = "view closed";
        }
      }
      else if (input_result.action == SelectListInputAction::Resolve && active_select_list == ActiveSelectList::ScopedModels && snapshot.select_list)
      {
        auto const* selected_item = selected_select_list_item();
        if (!selected_item || !selected_item->enabled || selected_item->value.empty())
        {
          snapshot.status = "scoped model cannot be toggled from this row";
          do_beep(terminal_context);
        }
        else if (!options.on_scoped_model_toggled)
        {
          snapshot.status = "scoped model toggle unavailable";
          do_beep(terminal_context);
        }
        else
        {
          auto updated = options.on_scoped_model_toggled(*snapshot.select_list, selected_item->value);
          if (updated)
          {
            preserve_session_selector_state(std::move(*updated), "scoped model toggled");
          }
          else
          {
            snapshot.status = updated.error().format();
            do_beep(terminal_context);
          }
        }
      }
      else if (input_result.action == SelectListInputAction::ModelsEnableAll && active_select_list == ActiveSelectList::ScopedModels && snapshot.select_list)
      {
        if (!options.on_scoped_model_enable_all)
        {
          snapshot.status = "scoped model enable-all unavailable";
          do_beep(terminal_context);
        }
        else
        {
          auto updated = options.on_scoped_model_enable_all(*snapshot.select_list, visible_select_list_values());
          if (updated)
          {
            preserve_session_selector_state(std::move(*updated), "scoped models enabled");
          }
          else
          {
            snapshot.status = updated.error().format();
            do_beep(terminal_context);
          }
        }
      }
      else if (input_result.action == SelectListInputAction::ModelsClearAll && active_select_list == ActiveSelectList::ScopedModels && snapshot.select_list)
      {
        if (!options.on_scoped_model_clear_all)
        {
          snapshot.status = "scoped model clear-all unavailable";
          do_beep(terminal_context);
        }
        else
        {
          auto updated = options.on_scoped_model_clear_all(*snapshot.select_list, visible_select_list_values());
          if (updated)
          {
            preserve_session_selector_state(std::move(*updated), "scoped models cleared");
          }
          else
          {
            snapshot.status = updated.error().format();
            do_beep(terminal_context);
          }
        }
      }
      else if (input_result.action == SelectListInputAction::ModelsToggleProvider && active_select_list == ActiveSelectList::ScopedModels &&
               snapshot.select_list)
      {
        auto const* selected_item = selected_select_list_item();
        if (!selected_item || selected_item->value.empty())
        {
          snapshot.status = "provider toggle unavailable from this row";
          do_beep(terminal_context);
        }
        else if (!options.on_scoped_model_toggle_provider)
        {
          snapshot.status = "provider toggle unavailable";
          do_beep(terminal_context);
        }
        else
        {
          auto updated = options.on_scoped_model_toggle_provider(*snapshot.select_list, selected_item->value);
          if (updated)
          {
            preserve_session_selector_state(std::move(*updated), "scoped provider toggled");
          }
          else
          {
            snapshot.status = updated.error().format();
            do_beep(terminal_context);
          }
        }
      }
      else if ((input_result.action == SelectListInputAction::ModelsReorderUp || input_result.action == SelectListInputAction::ModelsReorderDown) &&
               active_select_list == ActiveSelectList::ScopedModels && snapshot.select_list)
      {
        auto const* selected_item = selected_select_list_item();
        if (!selected_item || selected_item->value.empty())
        {
          snapshot.status = "scoped model reorder unavailable from this row";
          do_beep(terminal_context);
        }
        else if (!options.on_scoped_model_reorder)
        {
          snapshot.status = "scoped model reorder unavailable";
          do_beep(terminal_context);
        }
        else
        {
          auto updated =
              options.on_scoped_model_reorder(*snapshot.select_list, selected_item->value, input_result.action == SelectListInputAction::ModelsReorderUp);
          if (updated)
          {
            preserve_session_selector_state(std::move(*updated), "scoped model order updated");
          }
          else
          {
            snapshot.status = updated.error().format();
            do_beep(terminal_context);
          }
        }
      }
      else if (input_result.action == SelectListInputAction::ModelsSave && active_select_list == ActiveSelectList::ScopedModels && snapshot.select_list)
      {
        if (!options.on_scoped_model_save)
        {
          snapshot.status = "scoped model save unavailable";
          do_beep(terminal_context);
        }
        else
        {
          auto saved = options.on_scoped_model_save();
          if (saved)
          {
            snapshot.status = *saved;
          }
          else
          {
            snapshot.status = saved.error().format();
            do_beep(terminal_context);
          }
        }
      }
      else if (input_result.action == SelectListInputAction::SummarizeParent && active_select_list == ActiveSelectList::Session && snapshot.select_list)
      {
        session_archive_confirmation.reset();
        if (input_result.selected_item_index >= snapshot.select_list->items.size() || !snapshot.select_list->items[input_result.selected_item_index].enabled ||
            snapshot.select_list->items[input_result.selected_item_index].value.empty())
        {
          snapshot.status = "parent summary is unavailable from this row";
          do_beep(terminal_context);
        }
        else
        {
          auto prior_view = *snapshot.select_list;
          prior_view.query = input_result.query;
          prior_view.selected_item_index = input_result.selected_item_index;
          auto const selected_value = prior_view.items[input_result.selected_item_index].value;
          if (!begin_branch_summary(prior_view, input_result.selected_item_index, selected_value))
          {
            terminal_write_failed = true;
            break;
          }
        }
      }
      else if (input_result.action == SelectListInputAction::CycleSort && active_select_list == ActiveSelectList::Session && snapshot.select_list &&
               options.on_session_selector_sort_cycle)
      {
        preserve_session_selector_state(options.on_session_selector_sort_cycle(), "session selector sort cycled");
      }
      else if (input_result.action == SelectListInputAction::ToggleNamedFilter && active_select_list == ActiveSelectList::Session && snapshot.select_list &&
               options.on_session_selector_named_filter_toggle)
      {
        preserve_session_selector_state(options.on_session_selector_named_filter_toggle(), "session selector filter toggled");
      }
      else if (input_result.action == SelectListInputAction::TogglePathDisplay && active_select_list == ActiveSelectList::Session && snapshot.select_list &&
               options.on_session_selector_path_display_toggle)
      {
        preserve_session_selector_state(options.on_session_selector_path_display_toggle(), "session selector path display toggled");
      }
      else if (input_result.action == SelectListInputAction::ToggleArchivedFilter && active_select_list == ActiveSelectList::Session && snapshot.select_list &&
               options.on_session_selector_archived_filter_toggle)
      {
        preserve_session_selector_state(options.on_session_selector_archived_filter_toggle(), "session selector archived filter toggled");
      }
      else if (input_result.action == SelectListInputAction::ToggleLabelTimestamp && active_select_list == ActiveSelectList::Session && snapshot.select_list &&
               options.on_session_selector_label_timestamp_toggle)
      {
        preserve_session_selector_state(options.on_session_selector_label_timestamp_toggle(), "session selector label timestamps toggled");
      }
      else if (input_result.action == SelectListInputAction::Rename && active_select_list == ActiveSelectList::Session && snapshot.select_list)
      {
        if (input_result.selected_item_index < snapshot.select_list->items.size() && snapshot.select_list->items[input_result.selected_item_index].enabled &&
            !snapshot.select_list->items[input_result.selected_item_index].value.empty())
        {
          auto const selected_value = snapshot.select_list->items[input_result.selected_item_index].value;
          snapshot.select_list.reset();
          active_select_list = ActiveSelectList::None;
          auto draft_text = "/sessions rename " + selected_value + " ";
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, std::move(draft_text)));
          draft_input.clear();
          history_index.reset();
          draft_scroll_offset = 0;
          snapshot.status = "session rename draft ready";
        }
        else
        {
          snapshot.status = "session cannot be renamed from this row";
          do_beep(terminal_context);
        }
      }
      else if (input_result.action == SelectListInputAction::Label && active_select_list == ActiveSelectList::Session && snapshot.select_list)
      {
        if (input_result.selected_item_index < snapshot.select_list->items.size() && snapshot.select_list->items[input_result.selected_item_index].enabled &&
            !snapshot.select_list->items[input_result.selected_item_index].value.empty())
        {
          auto const selected_value = snapshot.select_list->items[input_result.selected_item_index].value;
          snapshot.select_list.reset();
          active_select_list = ActiveSelectList::None;
          auto draft_text = "/sessions labels " + selected_value + " ";
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, std::move(draft_text)));
          draft_input.clear();
          history_index.reset();
          draft_scroll_offset = 0;
          snapshot.status = "session labels draft ready";
        }
        else
        {
          snapshot.status = "session cannot be labeled from this row";
          do_beep(terminal_context);
        }
      }
      else if ((input_result.action == SelectListInputAction::BranchParent || input_result.action == SelectListInputAction::BranchChild) &&
               active_select_list == ActiveSelectList::Session && snapshot.select_list)
      {
        session_archive_confirmation.reset();
        auto const* selected_item = selected_select_list_item();
        if (!selected_item || !selected_item->enabled || selected_item->value.empty())
        {
          snapshot.status = "session branch navigation unavailable from this row";
          do_beep(terminal_context);
        }
        else if (input_result.action == SelectListInputAction::BranchParent && !options.on_session_selector_branch_parent)
        {
          snapshot.status = "session parent navigation unavailable";
          do_beep(terminal_context);
        }
        else if (input_result.action == SelectListInputAction::BranchChild && !options.on_session_selector_branch_child)
        {
          snapshot.status = "session child navigation unavailable";
          do_beep(terminal_context);
        }
        else
        {
          auto const selected_value = selected_item->value;
          auto opened = dispatch_tui_selector_authority(snapshot, "opening session…", render, [&]() {
            return input_result.action == SelectListInputAction::BranchParent ? options.on_session_selector_branch_parent(selected_value)
                                                                              : options.on_session_selector_branch_child(selected_value);
          });
          if (opened)
          {
            snapshot.select_list.reset();
            active_select_list = ActiveSelectList::None;
            apply_opened_session_snapshot(std::move(*opened), true);
          }
          else
          {
            snapshot.status = opened.error().format();
            do_beep(terminal_context);
          }
        }
      }
      else if (input_result.action == SelectListInputAction::Archive || input_result.action == SelectListInputAction::ArchiveNoninvasive)
      {
        bool const noninvasive_archive = input_result.action == SelectListInputAction::ArchiveNoninvasive;
        auto const* selected_item = selected_select_list_item();
        if (active_select_list != ActiveSelectList::Session || !snapshot.select_list)
        {
          snapshot.select_list.reset();
          active_select_list = ActiveSelectList::None;
          session_archive_confirmation.reset();
          snapshot.status = "view canceled";
        }
        else if (!selected_item || !selected_item->enabled || selected_item->value.empty())
        {
          session_archive_confirmation.reset();
          snapshot.status = "session cannot be archived or restored from this row";
          do_beep(terminal_context);
        }
        else
        {
          bool const archive = selected_item->badge != "archived";
          if (archive && selected_item->current)
          {
            session_archive_confirmation.reset();
            snapshot.status = "switch sessions before archiving the active session";
            do_beep(terminal_context);
          }
          else if (archive && !options.on_session_selector_archive)
          {
            session_archive_confirmation.reset();
            snapshot.status = "session archive unavailable";
            do_beep(terminal_context);
          }
          else if (!archive && !options.on_session_selector_unarchive)
          {
            session_archive_confirmation.reset();
            snapshot.status = "session restore unavailable";
            do_beep(terminal_context);
          }
          else if (session_archive_confirmation && session_archive_confirmation->session_id == selected_item->value &&
                   session_archive_confirmation->archive == archive)
          {
            auto updated = archive ? options.on_session_selector_archive(selected_item->value) : options.on_session_selector_unarchive(selected_item->value);
            session_archive_confirmation.reset();
            if (updated)
            {
              preserve_session_selector_state(std::move(*updated), archive ? "session archived" : "session restored");
            }
            else
            {
              snapshot.status = updated.error().format();
              do_beep(terminal_context);
            }
          }
          else
          {
            session_archive_confirmation = PendingSessionArchiveAction{.session_id = selected_item->value, .archive = archive};
            snapshot.status = std::string("press ") + (noninvasive_archive ? "Ctrl+Backspace" : "Ctrl+D") + " again to " + (archive ? "archive " : "restore ") +
                              selected_item->value;
          }
        }
      }
      else if (input_result.action == SelectListInputAction::Resolve || input_result.action == SelectListInputAction::Cancel)
      {
        std::string selected_value;
        if (input_result.action == SelectListInputAction::Resolve && snapshot.select_list &&
            input_result.selected_item_index < snapshot.select_list->items.size())
        {
          selected_value = snapshot.select_list->items[input_result.selected_item_index].value;
        }
        auto const resolved_list = active_select_list;
        snapshot.select_list.reset();
        active_select_list = ActiveSelectList::None;
        session_archive_confirmation.reset();
        if (input_result.action == SelectListInputAction::Cancel)
        {
          snapshot.status = resolved_list == ActiveSelectList::Reasoning ? "thinking mode unchanged" : "view canceled";
        }
        else if (resolved_list == ActiveSelectList::Settings && selected_value == kSettingsOpenModels)
        {
          static_cast<void>(open_model_selector());
        }
        else if (resolved_list == ActiveSelectList::Settings && selected_value == kSettingsOpenScopedModels)
        {
          static_cast<void>(open_scoped_model_selector());
        }
        else if (resolved_list == ActiveSelectList::Settings && selected_value == kSettingsOpenKeybindings)
        {
          snapshot.select_list = hotkeys_select_list_view(options.key_bindings);
          active_select_list = ActiveSelectList::Hotkeys;
          snapshot.status = "keybindings opened";
          transcript_scroll_offset = 0;
        }
        else if (resolved_list == ActiveSelectList::Hotkeys && !selected_value.empty())
        {
          auto draft_command = std::string("/keybindings set ") + selected_value + " ";
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, std::move(draft_command)));
          selected_slash_command_index = 0;
          path_completion_force_active = false;
          draft_scroll_offset = 0;
          history_index.reset();
          draft_input.clear();
          snapshot.status = "keybinding edit command drafted";
        }
        else if (resolved_list == ActiveSelectList::Settings && selected_value == kSettingsEditKeybindings)
        {
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, "/keybindings set "));
          selected_slash_command_index = 0;
          path_completion_force_active = false;
          draft_scroll_offset = 0;
          history_index.reset();
          draft_input.clear();
          snapshot.status = "keybinding edit command drafted";
        }
        else if (resolved_list == ActiveSelectList::Settings && selected_value == kSettingsReloadKeybindings)
        {
          if (!options.on_reload_key_bindings)
          {
            snapshot.status = "reload unavailable";
            do_beep(terminal_context);
          }
          else
          {
            auto reloaded = options.on_reload_key_bindings();
            if (!reloaded)
            {
              snapshot.status = reloaded.error().format();
              do_beep(terminal_context);
            }
            else
            {
              options.key_bindings = std::move(reloaded->key_bindings);
              snapshot.active_run_hint = active_run_hint_for(options.key_bindings);
              // Success feedback stays on the transient status surface from the applied DTO.
              apply_runtime_state_snapshot(std::move(reloaded->state));
            }
          }
        }
        else if (resolved_list == ActiveSelectList::Model && options.on_model_selected)
        {
          auto selected = dispatch_tui_selector_authority(snapshot, "switching model…", render, [&]() { return options.on_model_selected(selected_value); });
          if (selected)
          {
            apply_runtime_state_snapshot(std::move(*selected));
            if (!open_reasoning_selector(true))
            {
              terminal_write_failed = true;
              break;
            }
          }
          else
          {
            snapshot.status = selected.error().format();
            do_beep(terminal_context);
          }
        }
        else if (resolved_list == ActiveSelectList::Reasoning && options.on_reasoning_selected)
        {
          auto level = selected_value == "default" ? std::optional<std::string>{} : std::optional<std::string>{selected_value};
          auto selected =
              dispatch_tui_selector_authority(snapshot, "setting thinking mode…", render, [&]() { return options.on_reasoning_selected(std::move(level)); });
          if (selected)
          {
            apply_runtime_state_snapshot(std::move(*selected));
          }
          else
          {
            snapshot.status = selected.error().format();
            do_beep(terminal_context);
          }
        }
        else if (resolved_list == ActiveSelectList::Session && options.on_session_selected)
        {
          auto selected = dispatch_tui_selector_authority(snapshot, "opening session…", render, [&]() { return options.on_session_selected(selected_value); });
          if (selected)
          {
            apply_opened_session_snapshot(std::move(*selected), false);
          }
          else
          {
            snapshot.status = selected.error().format();
            do_beep(terminal_context);
          }
        }
        else if (resolved_list == ActiveSelectList::ForkUserTurn && options.on_fork_user_turn_selected)
        {
          auto const presentation_session_id = snapshot.session_id;
          auto const presentation_session_path = presentation_state.sidebar.session_path;
          UserTurnForkSelectionDecision decision;
          if (selected_value.empty())
          {
            decision =
                evaluate_fork_user_turn_selection(selected_value, presentation_session_id, presentation_session_path, options.on_fork_user_turn_selected);
          }
          else
          {
            // Paint truthful pending authority status before the blocking fork
            // callback, matching other session-open selectors.
            snapshot.status = "forking session…";
            if (!render())
            {
              terminal_write_failed = true;
              break;
            }
            decision =
                evaluate_fork_user_turn_selection(selected_value, presentation_session_id, presentation_session_path, options.on_fork_user_turn_selected);
          }
          if (decision.action == UserTurnForkSelectionAction::ApplyOpenedSession && decision.opened_snapshot)
          {
            // Same transition boundary as session open: clear the prior
            // transcript only when session identity actually changed.
            apply_opened_session_snapshot(std::move(*decision.opened_snapshot), true);
          }
          else
          {
            snapshot.status = std::move(decision.status);
            if (decision.beep)
              do_beep(terminal_context);
          }
        }
        else if (resolved_list == ActiveSelectList::CopyUserTurn && options.on_read_user_turn_text)
        {
          auto decision = evaluate_copy_user_turn_selection(selected_value, options.on_read_user_turn_text, copy_text_to_terminal_clipboard);
          settle_local_command_status(snapshot, decision.status);
          if (decision.beep)
            do_beep(terminal_context);
        }
        else if (resolved_list == ActiveSelectList::Settings && options.on_settings_selected)
        {
          auto selected = options.on_settings_selected(selected_value);
          if (selected)
          {
            // Success feedback stays on the transient status surface from the applied DTO.
            apply_runtime_state_snapshot(std::move(*selected));
          }
          else
          {
            snapshot.status = selected.error().format();
            do_beep(terminal_context);
          }
        }
        else
        {
          snapshot.status = "view closed";
        }
      }
      if (!renderer.request_render())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    auto const event = input.event;
    if (auto handled = handle_sidebar_drawer_input(event))
    {
      if (!*handled)
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    auto is_action = [&](TuiAction action) { return key_matches_action(options.key_bindings, action, event.key); };
    auto select_slash_command = [&]() {
      selected_slash_command_index = clamp_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
      if (auto const disabled_reason = slash_command_selection_disabled_reason(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index))
      {
        snapshot.status = "command disabled: " + *disabled_reason;
        do_beep(terminal_context);
        return;
      }
      draft_state.clear_selection();
      auto selection = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
      static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
      selected_slash_command_index = 0;
      path_completion_force_active = false;
      draft_scroll_offset = 0;
      history_index.reset();
      draft_input.clear();
      snapshot.status = "command selected - press Enter to run";
    };
    auto select_file_reference = [&]() {
      selected_slash_command_index = clamp_completion(selected_slash_command_index);
      if (auto const disabled_reason = selected_completion_disabled_reason(selected_slash_command_index))
      {
        snapshot.status = "reference disabled: " + *disabled_reason;
        do_beep(terminal_context);
        return;
      }
      auto selection = selected_completion_text(selected_slash_command_index);
      draft_state.clear_selection();
      static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
      selected_slash_command_index = 0;
      path_completion_force_active = false;
      draft_scroll_offset = 0;
      history_index.reset();
      draft_input.clear();
      snapshot.status = "file reference selected";
    };
    auto select_path_completion = [&]() {
      selected_slash_command_index = clamp_completion(selected_slash_command_index);
      if (auto const disabled_reason = selected_completion_disabled_reason(selected_slash_command_index))
      {
        snapshot.status = "path disabled: " + *disabled_reason;
        do_beep(terminal_context);
        return;
      }
      auto selection = selected_completion_text(selected_slash_command_index);
      draft_state.clear_selection();
      static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
      selected_slash_command_index = 0;
      path_completion_force_active = false;
      draft_scroll_offset = 0;
      history_index.reset();
      draft_input.clear();
      snapshot.status = "path selected";
    };
    auto force_path_completion = [&]() {
      auto const was_suppressed = slash_palette_suppressed;
      slash_palette_suppressed = false;
      path_completion_force_active = true;
      auto const match_count = completion_match_count();
      if (match_count == 0)
      {
        slash_palette_suppressed = was_suppressed;
        path_completion_force_active = false;
        return false;
      }
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      if (match_count == 1)
      {
        if (auto const disabled_reason = selected_completion_disabled_reason(0))
        {
          path_completion_force_active = false;
          snapshot.status = "path disabled: " + *disabled_reason;
          do_beep(terminal_context);
          return true;
        }
        auto selection = selected_completion_text(0);
        draft_state.clear_selection();
        static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
        path_completion_force_active = false;
        draft_scroll_offset = 0;
        snapshot.status = "path selected";
        return true;
      }
      draft_scroll_offset = 0;
      snapshot.status = "path suggestions";
      return true;
    };
    if (jump_mode != ComposerJumpMode::None)
    {
      if (is_action(TuiAction::JumpForward) || is_action(TuiAction::JumpBackward))
      {
        jump_mode = ComposerJumpMode::None;
        snapshot.status = "jump cancelled";
      }
      else if (auto const target = printable_jump_target(input))
      {
        bool const forward = jump_mode == ComposerJumpMode::Forward;
        jump_mode = ComposerJumpMode::None;
        pending_escape_clear = false;
        history_index.reset();
        draft_input.clear();
        selected_slash_command_index = 0;
        slash_palette_suppressed = false;
        path_completion_force_active = false;
        draft_scroll_offset = 0;
        draft_state.clear_selection();
        snapshot.status =
            jump_composer_draft_to_character(draft, *target, forward) ? (forward ? "jumped forward" : "jumped backward") : "jump character not found";
      }
      else
      {
        jump_mode = ComposerJumpMode::None;
      }
      if (event.key == Key::Character || event.key == Key::Space || is_action(TuiAction::JumpForward) || is_action(TuiAction::JumpBackward))
      {
        snapshot.selected_slash_command_index = selected_slash_command_index;
        if (!renderer.request_render())
        {
          terminal_write_failed = true;
          break;
        }
        continue;
      }
    }
    bool const ctrl_d_delete_forward = event.key == Key::CtrlD && is_action(TuiAction::DeleteForward) && !draft.text.empty();
    bool const delete_forward_action = is_action(TuiAction::DeleteForward) && (event.key != Key::CtrlD || ctrl_d_delete_forward);

    auto insert_input_text = [&]() {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      draft_scroll_offset = 0;
      auto const text = input.text.empty() ? std::string(1, event.character) : input.text;
      if (input.bracketed_paste)
      {
        static_cast<void>(draft_state.delete_selection());
        if (insert_composer_paste_text(draft, text))
          snapshot.status = "pasted into draft safely";
      }
      else if (!draft_state.replace_selection(text))
      {
        static_cast<void>(insert_composer_draft_text(draft, text));
      }
    };
    if (is_action(TuiAction::CopyLatestAssistant))
    {
      if (!action_controller.copy_latest_assistant_message())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    else if (is_action(TuiAction::PromptStash))
    {
      if (!prompt_stash.trigger())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    else if (event.key == Key::Character)
    {
      insert_input_text();
    }
    else if (is_action(TuiAction::MessageFollowUp))
    {
      auto const action = handle_submit();
      if (action == RuntimeSubmitDisposition::BreakLoop)
        break;
      if (action == RuntimeSubmitDisposition::ContinueLoop)
        continue;
    }
    else if (is_action(TuiAction::NewLine))
    {
      draft_state.insert_newline();
    }
    else if (is_action(TuiAction::ExternalEditor))
    {
      if (!open_external_editor())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    else if (is_action(TuiAction::Suspend))
    {
      if (!suspend_to_background())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    else if (is_action(TuiAction::ClipboardPasteImage))
    {
      if (!paste_clipboard_image())
      {
        terminal_write_failed = true;
        break;
      }
      continue;
    }
    else if (is_action(TuiAction::JumpForward) || is_action(TuiAction::JumpBackward))
    {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      path_completion_force_active = false;
      jump_mode = is_action(TuiAction::JumpForward) ? ComposerJumpMode::Forward : ComposerJumpMode::Backward;
      snapshot.status = is_action(TuiAction::JumpForward) ? "jump forward: type character" : "jump backward: type character";
    }
    else if (is_action(TuiAction::DeleteBackward))
    {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      draft_scroll_offset = 0;
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteBackward));
    }
    else if (delete_forward_action)
    {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      draft_scroll_offset = 0;
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteForward));
    }
    else if (is_action(TuiAction::DeleteWordBackward))
    {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      draft_scroll_offset = 0;
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteWordBackward));
    }
    else if (is_action(TuiAction::DeleteWordForward))
    {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      draft_scroll_offset = 0;
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteWordForward));
    }
    else if (is_action(TuiAction::DeleteToLineStart))
    {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      draft_scroll_offset = 0;
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteToLineStart));
    }
    else if (is_action(TuiAction::DeleteToLineEnd))
    {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      draft_scroll_offset = 0;
      if (!draft_state.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft, TuiAction::DeleteToLineEnd));
    }
    else if (is_action(TuiAction::CopySelection) && draft_state.selection_bounds())
    {
      pending_escape_clear = false;
      path_completion_force_active = false;
      static_cast<void>(draft_state.copy_selection(snapshot));
    }
    else if (is_action(TuiAction::CopySelection) && renderer.has_transcript_selection())
    {
      pending_escape_clear = false;
      path_completion_force_active = false;
      static_cast<void>(renderer.copy_transcript_selection());
    }
    else if (is_action(TuiAction::ClearInput) && (!draft.text.empty() || !is_action(TuiAction::Interrupt)))
    {
      pending_escape_clear = false;
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      path_completion_force_active = false;
      draft_scroll_offset = 0;
      draft_state.clear_selection();
      snapshot.status = apply_composer_draft_action(draft, TuiAction::ClearInput) ? "input cleared" : "input already empty";
    }
    else if (is_action(TuiAction::AutocompleteAccept) && slash_palette_active())
    {
      pending_escape_clear = false;
      select_slash_command();
    }
    else if (is_action(TuiAction::AutocompleteAccept) && file_reference_palette_active())
    {
      pending_escape_clear = false;
      select_file_reference();
    }
    else if (is_action(TuiAction::AutocompleteAccept) && path_completion_palette_active())
    {
      pending_escape_clear = false;
      select_path_completion();
    }
    else if (is_action(TuiAction::AutocompleteAccept) && force_path_completion())
    {
      pending_escape_clear = false;
    }
    else if (is_action(TuiAction::ModeToggle))
    {
      pending_escape_clear = false;
      path_completion_force_active = false;
      if (!options.on_toggle_mode)
      {
        snapshot.status = "mode toggle unavailable";
      }
      else if (auto result = options.on_toggle_mode(); !result)
      {
        snapshot.status = result.error().format();
      }
      else
      {
        snapshot.mode = *result;
        snapshot.status = "mode switched to " + snapshot.mode;
      }
    }
    else if (is_action(TuiAction::Interrupt))
    {
      path_completion_force_active = false;
      if (!draft.text.empty())
      {
        static_cast<void>(clear_draft_for_interrupt());
        snapshot.selected_slash_command_index = selected_slash_command_index;
        if (!render())
        {
          terminal_write_failed = true;
          break;
        }
        continue;
      }
      break;
    }
    else if (is_action(TuiAction::Exit))
    {
      break;
    }
    else if (is_action(TuiAction::VariantCycle))
    {
      pending_escape_clear = false;
      cycle_reasoning();
    }
    else if (is_action(TuiAction::ReasoningSelect))
    {
      pending_escape_clear = false;
      if (!open_reasoning_selector())
      {
        terminal_write_failed = true;
        break;
      }
    }
    else if (is_action(TuiAction::ThinkingToggle))
    {
      pending_escape_clear = false;
      toggle_thinking_visibility();
    }
    else if (is_action(TuiAction::OverviewToggle))
    {
      pending_escape_clear = false;
      if (!toggle_startup_overview())
      {
        terminal_write_failed = true;
        break;
      }
    }
    else if (is_action(TuiAction::ModelSelect))
    {
      if (!open_model_selector())
      {
        terminal_write_failed = true;
        break;
      }
    }
    else if (is_action(TuiAction::ModelCycleForward))
    {
      pending_escape_clear = false;
      cycle_model(true);
    }
    else if (is_action(TuiAction::ModelCycleBackward))
    {
      pending_escape_clear = false;
      cycle_model(false);
    }
    else if (is_action(TuiAction::SessionResume) || is_action(TuiAction::SessionTree))
    {
      if (!open_session_selector())
      {
        terminal_write_failed = true;
        break;
      }
    }
    else if (is_action(TuiAction::SessionNew) || is_action(TuiAction::SessionFork))
    {
      auto const action = handle_submit(is_action(TuiAction::SessionNew) ? "/new" : "/fork");
      if (action == RuntimeSubmitDisposition::BreakLoop)
        break;
      if (action == RuntimeSubmitDisposition::ContinueLoop)
        continue;
    }
    else if (is_action(TuiAction::MessageDequeue))
    {
      pending_escape_clear = false;
      snapshot.status = "queued-message restore is available during active runs";
    }
    else if (is_action(TuiAction::TranscriptHalfPageUp))
    {
      scroll_up(navigation.transcript_page_size());
    }
    else if (is_action(TuiAction::TranscriptHalfPageDown))
    {
      scroll_down(navigation.transcript_page_size());
    }
    else if (is_action(TuiAction::PageUp))
    {
      scroll_up(navigation.transcript_page_size());
    }
    else if (is_action(TuiAction::PageDown))
    {
      scroll_down(navigation.transcript_page_size());
    }
    else if (is_action(TuiAction::MessagePrev))
    {
      scroll_to_message_boundary(true);
    }
    else if (is_action(TuiAction::MessageNext))
    {
      scroll_to_message_boundary(false);
    }
    else if (is_action(TuiAction::JumpToBottom))
    {
      jump_to_bottom("live tail");
    }
    else if (event.key == Key::MouseWheelUp)
    {
      scroll_up(kTranscriptWheelScrollRows);
    }
    else if (event.key == Key::MouseWheelDown)
    {
      scroll_down(kTranscriptWheelScrollRows);
    }
    else if (event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick || event.key == Key::MouseLeftDrag || event.key == Key::MouseLeftRelease ||
             event.key == Key::MousePointerCancel)
    {
      pending_escape_clear = false;
      auto const begins_click = event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick;
      bool palette_claimed = false;
      if (begins_click)
      {
        if (auto const clicked = slash_palette_selection_for_screen_position(snapshot, event.mouse_row, event.mouse_column))
        {
          renderer.clear_transcript_selection();
          draft_state.clear_selection();
          selected_slash_command_index = *clicked;
          select_slash_command();
          palette_claimed = true;
        }
        else if (auto const clicked = detail::file_reference_palette_selection_for_screen_position_cached(
                     snapshot, event.mouse_row, event.mouse_column, completion_cache, snapshot.file_references_generation))
        {
          renderer.clear_transcript_selection();
          draft_state.clear_selection();
          selected_slash_command_index = *clicked;
          select_file_reference();
          palette_claimed = true;
        }
        else if (auto const clicked = detail::path_completion_palette_selection_for_screen_position_cached(
                     snapshot, event.mouse_row, event.mouse_column, completion_cache, snapshot.file_references_generation))
        {
          renderer.clear_transcript_selection();
          draft_state.clear_selection();
          selected_slash_command_index = *clicked;
          select_path_completion();
          palette_claimed = true;
        }
      }
      if (!palette_claimed)
      {
        auto const transcript_mouse = renderer.handle_transcript_selection_mouse(event, toggle_tool_details_at, toggle_thinking_at);
        if (transcript_mouse == TranscriptSelectionMouseResult::Ignored)
        {
          if (begins_click)
          {
            if (auto const cursor = composer_input_cursor_for_screen_position(snapshot, event.mouse_row, event.mouse_column))
            {
              renderer.clear_transcript_selection();
              draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, *cursor);
              draft_state.clear_selection();
              if (event.key == Key::MouseLeftPress)
              {
                draft_selection_anchor = draft.cursor;
                draft_selection_cursor = draft.cursor;
                draft_state.mouse_selecting = true;
              }
              draft.vertical_column = std::string::npos;
              draft.yank_start = std::string::npos;
              draft.yank_end = std::string::npos;
              history_index.reset();
              draft_input.clear();
              snapshot.status = "cursor moved";
            }
          }
          else if (draft_state.mouse_selecting)
          {
            if (auto const cursor = composer_input_cursor_for_screen_position(snapshot, event.mouse_row, event.mouse_column))
            {
              auto const next_cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, *cursor);
              draft_selection_cursor = next_cursor;
              draft.cursor = next_cursor;
              draft.vertical_column = std::string::npos;
              draft.yank_start = std::string::npos;
              draft.yank_end = std::string::npos;
              history_index.reset();
              draft_input.clear();
              snapshot.status = draft_state.selection_bounds() ? "selection active" : "cursor moved";
            }
            if (event.key == Key::MouseLeftRelease)
              draft_state.mouse_selecting = false;
          }
        }
      }
    }
    else if (draft_state.extend_selection_for_key(event.key, snapshot))
    {
      renderer.clear_transcript_selection();
    }
    else if (event.key == Key::CtrlHome)
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      draft.cursor = 0;
      draft.vertical_column = std::string::npos;
      draft.yank_start = std::string::npos;
      draft.yank_end = std::string::npos;
    }
    else if (event.key == Key::CtrlEnd)
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      draft.cursor = draft.text.size();
      draft.vertical_column = std::string::npos;
      draft.yank_start = std::string::npos;
      draft.yank_end = std::string::npos;
    }
    else if (is_action(TuiAction::CursorLeft))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorLeft));
    }
    else if (is_action(TuiAction::CursorRight))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorRight));
    }
    else if (is_action(TuiAction::CursorLineStart))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorLineStart));
    }
    else if (is_action(TuiAction::CursorLineEnd))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorLineEnd));
    }
    else if (is_action(TuiAction::CursorWordLeft))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorWordLeft));
    }
    else if (is_action(TuiAction::CursorWordRight))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft, TuiAction::CursorWordRight));
    }
    else if (is_action(TuiAction::PalettePrev) && slash_palette_active())
    {
      pending_escape_clear = false;
      auto const match_count = filter_slash_commands(draft.text, draft.cursor, snapshot.slash_commands).size();
      if (match_count == 0)
      {
        snapshot.status = "no matching slash commands";
      }
      else
      {
        selected_slash_command_index = previous_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
        snapshot.status = "command " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (is_action(TuiAction::PalettePrev) && file_reference_palette_active())
    {
      pending_escape_clear = false;
      auto const match_count = completion_match_count();
      if (match_count == 0)
      {
        snapshot.status = "no matching file references";
      }
      else
      {
        selected_slash_command_index = previous_completion(selected_slash_command_index);
        snapshot.status = "reference " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (is_action(TuiAction::PalettePrev) && path_completion_palette_active())
    {
      pending_escape_clear = false;
      auto const match_count = completion_match_count();
      if (match_count == 0)
      {
        snapshot.status = "no matching paths";
      }
      else
      {
        selected_slash_command_index = previous_completion(selected_slash_command_index);
        snapshot.status = "path " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (is_action(TuiAction::HistoryPrev))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      path_completion_force_active = false;
      draft_scroll_offset = 0;
      if (browse_composer_input_history(draft, input_history, history_index, draft_input, true))
      {
        snapshot.status = "history previous";
      }
      else
      {
        scroll_up(kKeyboardScrollRows);
      }
    }
    else if (event.key == Key::ArrowUp)
    {
      scroll_up(kKeyboardScrollRows);
    }
    else if (is_action(TuiAction::CursorUp) && apply_composer_draft_action(draft, TuiAction::CursorUp))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
    }
    else if (is_action(TuiAction::PaletteNext) && slash_palette_active())
    {
      pending_escape_clear = false;
      auto const match_count = filter_slash_commands(draft.text, draft.cursor, snapshot.slash_commands).size();
      if (match_count == 0)
      {
        snapshot.status = "no matching slash commands";
      }
      else
      {
        selected_slash_command_index = next_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
        snapshot.status = "command " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (is_action(TuiAction::PaletteNext) && file_reference_palette_active())
    {
      pending_escape_clear = false;
      auto const match_count = completion_match_count();
      if (match_count == 0)
      {
        snapshot.status = "no matching file references";
      }
      else
      {
        selected_slash_command_index = next_completion(selected_slash_command_index);
        snapshot.status = "reference " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (is_action(TuiAction::PaletteNext) && path_completion_palette_active())
    {
      pending_escape_clear = false;
      auto const match_count = completion_match_count();
      if (match_count == 0)
      {
        snapshot.status = "no matching paths";
      }
      else
      {
        selected_slash_command_index = next_completion(selected_slash_command_index);
        snapshot.status = "path " + std::to_string(selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (is_action(TuiAction::HistoryNext))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
      path_completion_force_active = false;
      draft_scroll_offset = 0;
      if (browse_composer_input_history(draft, input_history, history_index, draft_input, false))
      {
        snapshot.status = history_index ? "history next" : "history draft";
      }
      else
      {
        scroll_down(kKeyboardScrollRows);
      }
    }
    else if (event.key == Key::ArrowDown)
    {
      scroll_down(kKeyboardScrollRows);
    }
    else if (is_action(TuiAction::CursorDown) && apply_composer_draft_action(draft, TuiAction::CursorDown))
    {
      pending_escape_clear = false;
      draft_state.clear_selection();
      history_index.reset();
      draft_input.clear();
      selected_slash_command_index = 0;
      slash_palette_suppressed = false;
    }
    else if (is_action(TuiAction::Undo))
    {
      pending_escape_clear = false;
      draft_scroll_offset = 0;
      draft_state.clear_selection();
      snapshot.status = apply_composer_draft_action(draft, TuiAction::Undo) ? "undo" : "nothing to undo";
    }
    else if (is_action(TuiAction::Redo))
    {
      pending_escape_clear = false;
      draft_scroll_offset = 0;
      draft_state.clear_selection();
      snapshot.status = apply_composer_draft_action(draft, TuiAction::Redo) ? "redo" : "nothing to redo";
    }
    else if (is_action(TuiAction::Yank))
    {
      pending_escape_clear = false;
      draft_scroll_offset = 0;
      draft_state.clear_selection();
      snapshot.status = apply_composer_draft_action(draft, TuiAction::Yank) ? "yanked text" : "nothing to yank";
    }
    else if (is_action(TuiAction::YankPop))
    {
      pending_escape_clear = false;
      draft_scroll_offset = 0;
      draft_state.clear_selection();
      snapshot.status = apply_composer_draft_action(draft, TuiAction::YankPop) ? "yank-pop" : "nothing to yank-pop";
    }
    else if (is_action(TuiAction::DetailsToggle))
    {
      pending_escape_clear = false;
      renderer.synchronize_detached_transcript_layout();
      snapshot.tool_presentation = snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich : ToolPresentation::Expanded;
      snapshot.status = "tool details " + std::string(to_string(snapshot.tool_presentation));
    }
    else if (is_action(TuiAction::PromptAllow) || is_action(TuiAction::PromptDeny))
    {
      pending_escape_clear = false;
      snapshot.status = "prompt action is only available while a prompt is active";
    }
    else if (is_action(TuiAction::Cancel))
    {
      if (draft_state.selection_bounds())
      {
        draft_state.clear_selection();
        pending_escape_clear = false;
        snapshot.status.clear();
      }
      else if (renderer.has_transcript_selection())
      {
        renderer.clear_transcript_selection();
        pending_escape_clear = false;
        snapshot.status.clear();
      }
      else if (slash_palette_active() || file_reference_palette_active() || path_completion_palette_active())
      {
        pending_escape_clear = false;
        slash_palette_suppressed = true;
        selected_slash_command_index = 0;
        path_completion_force_active = false;
        history_index.reset();
        draft_input.clear();
        snapshot.status.clear();
      }
      else if (!draft.text.empty())
      {
        if (pending_escape_clear)
        {
          static_cast<void>(apply_composer_draft_action(draft, TuiAction::ClearInput));
          selected_slash_command_index = 0;
          path_completion_force_active = false;
          history_index.reset();
          draft_input.clear();
          draft_scroll_offset = 0;
          pending_escape_clear = false;
          snapshot.status = "input cleared";
        }
        else
        {
          pending_escape_clear = true;
          snapshot.status = "press Esc again to clear";
        }
      }
      else
      {
        pending_escape_clear = false;
        snapshot.status = "escape ignored";
      }
    }
    else if (is_action(TuiAction::Submit))
    {
      if (event.key == Key::Enter && draft_state.convert_backslash_enter_to_newline(snapshot))
      {
        if (!renderer.request_render())
        {
          terminal_write_failed = true;
          break;
        }
        continue;
      }
      auto const action = handle_submit();
      if (action == RuntimeSubmitDisposition::BreakLoop)
        break;
      if (action == RuntimeSubmitDisposition::ContinueLoop)
        continue;
    }
    else if (event.key == Key::Space)
    {
      insert_input_text();
    }
    snapshot.selected_slash_command_index = selected_slash_command_index;
    if (!renderer.request_render())
    {
      terminal_write_failed = true;
      break;
    }
  }

  mermaid_presentation.shutdown();
  plugin_ui.shutdown(snapshot);
  if (!before_shutdown.invoke())
    terminal_write_failed = true;
  return terminal_signal_received ? 130 : (terminal_write_failed ? 1 : 0);
}

ava::core::Result<TuiRuntimeStateSnapshot> dispatch_tui_selector_authority(ComposerSnapshot& snapshot, std::string pending_status,
                                                                           std::function<bool()> const& render,
                                                                           std::function<ava::core::Result<TuiRuntimeStateSnapshot>()> const& callback)
{
  snapshot.select_list.reset();
  snapshot.status = std::move(pending_status);
  if (!render())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to paint selector authority status");
    snapshot.status = error.format();
    return std::unexpected(std::move(error));
  }
  auto result = callback();
  if (!result)
    snapshot.status = result.error().format();
  return result;
}

}  // namespace ava::tui
