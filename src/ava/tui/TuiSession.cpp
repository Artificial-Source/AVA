#include "sys.h"
#include "SelectorHost.h"
#include "TuiFrontEnd.h"
#include "TuiSession.h"
#include "command_output.h"
#include "composer_internal.h"
#include "prompt_stash_internal.h"
#include "runtime_actions_internal.h"
#include "runtime_active_run_internal.h"
#include "runtime_draft_internal.h"
#include "runtime_input_internal.h"
#include "runtime_internal.h"
#include "runtime_mermaid_internal.h"
#include "runtime_navigation_internal.h"
#include "runtime_plugin_ui_internal.h"
#include "runtime_prompts_internal.h"
#include "runtime_render_internal.h"
#include "runtime_state_internal.h"
#include "runtime_subagent_workspace_internal.h"
#include "runtime_submit_internal.h"
#include "runtime_transcript_search_internal.h"
#include "runtime_user_turn_selection_internal.h"
#include "runtime_views_internal.h"

#include <functional>
#include <utility>

namespace ava::tui {
namespace {
constexpr std::size_t kKeyboardScrollRows = 3;
constexpr auto kIdleInputPollDelay = std::chrono::milliseconds(250);
} // namespace
using Key = terminal::Key;

TuiSession::TuiSession(TuiFrontEnd& frontend, terminal::Context& terminal_context, std::string const& initial_status_line)
    : frontend_(frontend),
      terminal_context_(terminal_context),
      presentation_state_(&frontend, initial_status_line),
      renderer_(&frontend, presentation_state_.snapshot, presentation_state_.sidebar, draft_state_, &terminal_context),
      navigation_(frontend, presentation_state_.snapshot, presentation_state_.sidebar, draft_state_, renderer_),
      selectors_(presentation_state_.snapshot),
      settings_(frontend, presentation_state_, selectors_, terminal_context),
      session_selector_(frontend, presentation_state_, selectors_, renderer_, terminal_context),
      branch_summary_(frontend, presentation_state_, selectors_, renderer_, terminal_context),
      prompt_stash_(presentation_state_, draft_state_, renderer_, selectors_, frontend.key_bindings()),
      transcript_search_(presentation_state_, renderer_, navigation_, selectors_),
      prompt_coordinator_(frontend, presentation_state_.snapshot, presentation_state_.command_session_grants, renderer_, &selectors_),
      mermaid_presentation_(frontend.mermaid_bridge()),
      action_controller_(frontend, presentation_state_, draft_state_, renderer_, selectors_, session_selector_),
      subagent_workspace_(frontend, presentation_state_.snapshot, selectors_),
      session_transitions_(presentation_state_, draft_state_, renderer_, transcript_search_, subagent_workspace_, selectors_),
      active_run_controller_(frontend, presentation_state_, draft_state_, renderer_, prompt_coordinator_, prompt_stash_, plugin_ui_, navigation_,
                             action_controller_, transcript_search_, subagent_workspace_, session_transitions_,
                             std::bind_front(&TuiSession::service_mermaid_presentation, this)),
      submit_controller_(frontend, presentation_state_, draft_state_, renderer_, navigation_, action_controller_, active_run_controller_, prompt_stash_,
                         transcript_search_, subagent_workspace_, selectors_, settings_, session_selector_)
{
}

TuiSession::~TuiSession()
{
  // Each collaborator is still alive here; shutdown also covers early run returns.
  mermaid_presentation_.shutdown();
  plugin_ui_.shutdown(presentation_state_.snapshot);
}

bool TuiSession::stop_for_branch_summary(BranchSummaryOutcome outcome)
{
  if (outcome == BranchSummaryOutcome::RenderFailed)
    terminal_write_failed_ = true;
  return outcome == BranchSummaryOutcome::RenderFailed || outcome == BranchSummaryOutcome::ExitReady;
}

bool TuiSession::service_mermaid_presentation()
{
  auto old_anchor = detail::TranscriptViewportAnchor{};
  auto const preserve_viewport = renderer_.transcript_scroll_offset > 0;
  if (preserve_viewport && !renderer_.has_deferred_detached_transcript_update())
  {
    auto const old_max_scroll = detail::composer_max_transcript_scroll_offset_cached(
        presentation_state_.snapshot, presentation_state_.snapshot.width, presentation_state_.snapshot.height, renderer_.completion_cache,
        presentation_state_.snapshot.file_references_generation, renderer_.transcript_layout_cache, presentation_state_.snapshot.transcript_generation);
    old_anchor = detail::capture_transcript_viewport_anchor(renderer_.transcript_layout_cache.layout, old_max_scroll, renderer_.transcript_scroll_offset);
  }
  auto const update = mermaid_presentation_.service(presentation_state_.snapshot);
  if (!update.visual_changed)
    return true;
  transcript_search_.refresh_after_transcript_mutation(0, update.earliest_changed_item);
  if (preserve_viewport)
  {
    renderer_.defer_detached_transcript_update(old_anchor, 0);
    renderer_.synchronize_detached_transcript_layout();
  }
  return renderer_.request_render(FrameRenderKind::Full);
}

bool TuiSession::maybe_reload_display_settings()
{
  auto const outcome = action_controller_.maybe_reload_display_settings();
  if (outcome == DisplaySettingsReloadPollOutcome::TerminalFailure)
    return false;
  if (outcome != DisplaySettingsReloadPollOutcome::Applied)
    return true;

  settings_.rebase_after_display_reload();
  // Paint exactly once after overlay staging so the first frame is the final staged state.
  return renderer_.render();
}

bool TuiSession::toggle_startup_overview()
{
  settings_.close();
  return action_controller_.toggle_startup_overview();
}

RuntimeSubmitDisposition TuiSession::handle_submit(std::optional<std::string> forced_submission)
{
  auto const outcome = submit_controller_.submit(std::move(forced_submission));
  terminal_write_failed_ = outcome.terminal_write_failed;
  terminal_signal_received_ = outcome.terminal_signal_received;
  // A terminal signal must end submission; continuing would lose the session exit outcome.
  ASSERT(!terminal_signal_received_ || outcome.disposition == RuntimeSubmitDisposition::BreakLoop);
  return outcome.disposition;
}

void TuiSession::settle_opened_session(std::string status)
{
  auto committed_session_id = frontend_.read_committed_session_id();
  static_cast<void>(session_transitions_.apply_and_refresh(frontend_, std::move(committed_session_id)));
  if (!status.empty())
    settle_local_command_status(presentation_state_.snapshot, std::move(status));
}

bool TuiSession::synchronize_committed_session()
{
  auto const committed_session_id = frontend_.read_committed_session_id();
  return session_transitions_.apply(std::move(committed_session_id));
}

void TuiSession::select_slash_command()
{
  draft_state_.selected_slash_command_index = clamp_slash_palette_selection(
      draft_state_.draft.text, draft_state_.draft.cursor, presentation_state_.snapshot.slash_commands, draft_state_.selected_slash_command_index);
  if (auto const disabled_reason = slash_command_selection_disabled_reason(
          draft_state_.draft.text, draft_state_.draft.cursor, presentation_state_.snapshot.slash_commands, draft_state_.selected_slash_command_index))
  {
    presentation_state_.snapshot.status = "command disabled: " + *disabled_reason;
    terminal_context_.beep();
    return;
  }
  draft_state_.clear_selection();
  auto selection = slash_command_selection_text(draft_state_.draft.text, draft_state_.draft.cursor, presentation_state_.snapshot.slash_commands,
                                                draft_state_.selected_slash_command_index);
  static_cast<void>(replace_composer_draft(draft_state_.draft, std::move(selection.text), selection.cursor));
  draft_state_.selected_slash_command_index = 0;
  draft_state_.path_completion_force_active = false;
  draft_state_.draft_scroll_offset = 0;
  draft_state_.history_index.reset();
  draft_state_.draft_input.clear();
  presentation_state_.snapshot.status = "command selected - press Enter to run";
}

void TuiSession::select_file_reference()
{
  draft_state_.selected_slash_command_index = navigation_.clamp_completion(draft_state_.selected_slash_command_index);
  if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(draft_state_.selected_slash_command_index))
  {
    presentation_state_.snapshot.status = "reference disabled: " + *disabled_reason;
    terminal_context_.beep();
    return;
  }
  auto selection = navigation_.selected_completion_text(draft_state_.selected_slash_command_index);
  draft_state_.clear_selection();
  static_cast<void>(replace_composer_draft(draft_state_.draft, std::move(selection.text), selection.cursor));
  draft_state_.selected_slash_command_index = 0;
  draft_state_.path_completion_force_active = false;
  draft_state_.draft_scroll_offset = 0;
  draft_state_.history_index.reset();
  draft_state_.draft_input.clear();
  presentation_state_.snapshot.status = "file reference selected";
}

void TuiSession::select_path_completion()
{
  draft_state_.selected_slash_command_index = navigation_.clamp_completion(draft_state_.selected_slash_command_index);
  if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(draft_state_.selected_slash_command_index))
  {
    presentation_state_.snapshot.status = "path disabled: " + *disabled_reason;
    terminal_context_.beep();
    return;
  }
  auto selection = navigation_.selected_completion_text(draft_state_.selected_slash_command_index);
  draft_state_.clear_selection();
  static_cast<void>(replace_composer_draft(draft_state_.draft, std::move(selection.text), selection.cursor));
  draft_state_.selected_slash_command_index = 0;
  draft_state_.path_completion_force_active = false;
  draft_state_.draft_scroll_offset = 0;
  draft_state_.history_index.reset();
  draft_state_.draft_input.clear();
  presentation_state_.snapshot.status = "path selected";
}

bool TuiSession::force_path_completion()
{
  auto const was_suppressed = draft_state_.slash_palette_suppressed;
  draft_state_.slash_palette_suppressed = false;
  draft_state_.path_completion_force_active = true;
  auto const match_count = navigation_.completion_match_count();
  if (match_count == 0)
  {
    draft_state_.slash_palette_suppressed = was_suppressed;
    draft_state_.path_completion_force_active = false;
    return false;
  }
  draft_state_.pending_escape_clear = false;
  draft_state_.history_index.reset();
  draft_state_.draft_input.clear();
  draft_state_.selected_slash_command_index = 0;
  if (match_count == 1)
  {
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(0))
    {
      draft_state_.path_completion_force_active = false;
      presentation_state_.snapshot.status = "path disabled: " + *disabled_reason;
      terminal_context_.beep();
      return true;
    }
    auto selection = navigation_.selected_completion_text(0);
    draft_state_.clear_selection();
    static_cast<void>(replace_composer_draft(draft_state_.draft, std::move(selection.text), selection.cursor));
    draft_state_.path_completion_force_active = false;
    draft_state_.draft_scroll_offset = 0;
    presentation_state_.snapshot.status = "path selected";
    return true;
  }
  draft_state_.draft_scroll_offset = 0;
  presentation_state_.snapshot.status = "path suggestions";
  return true;
}

void TuiSession::insert_input_text(runtime_input::RuntimeInput const& input)
{
  auto const& event = input.event;
  draft_state_.pending_escape_clear = false;
  draft_state_.history_index.reset();
  draft_state_.draft_input.clear();
  draft_state_.selected_slash_command_index = 0;
  draft_state_.slash_palette_suppressed = false;
  draft_state_.draft_scroll_offset = 0;
  auto const text = input.text.empty() ? std::string(1, event.character) : input.text;
  if (input.bracketed_paste)
  {
    static_cast<void>(draft_state_.delete_selection());
    if (insert_composer_paste_text(draft_state_.draft, text))
      presentation_state_.snapshot.status = "pasted into draft safely";
  }
  else if (!draft_state_.replace_selection(text))
  {
    static_cast<void>(insert_composer_draft_text(draft_state_.draft, text));
  }
}

SelectListInputResult TuiSession::classify_selector_input(runtime_input::RuntimeInput const& input)
{
  if (input.event.key == Key::MouseLeftPress || input.event.key == Key::MouseLeftClick)
  {
    if (auto const clicked = select_list_selection_for_screen_position(presentation_state_.snapshot, input.event.mouse_row, input.event.mouse_column))
    {
      // Settings mouse clicks change selection/highlight only; Enter confirms.
      // Other selectors keep existing mouse-confirm behavior.
      auto action = selectors_.kind() == ActiveSelectList::Settings ? SelectListInputAction::Redraw : SelectListInputAction::Resolve;
      if (*clicked >= selectors_.view()->items.size() || !selectors_.view()->items[*clicked].enabled)
      {
        action = SelectListInputAction::Redraw;
      }
      return SelectListInputResult{.selected_item_index = *clicked, .query = selectors_.view()->query, .action = action};
    }
  }
  if (selectors_.kind() == ActiveSelectList::ScopedModels)
  {
    if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelsSave, input.event.key))
    {
      return SelectListInputResult{
          .selected_item_index = selectors_.view()->selected_item_index, .query = selectors_.view()->query, .action = SelectListInputAction::ModelsSave};
    }
    if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelsEnableAll, input.event.key))
    {
      return SelectListInputResult{
          .selected_item_index = selectors_.view()->selected_item_index, .query = selectors_.view()->query, .action = SelectListInputAction::ModelsEnableAll};
    }
    if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelsClearAll, input.event.key))
    {
      return SelectListInputResult{
          .selected_item_index = selectors_.view()->selected_item_index, .query = selectors_.view()->query, .action = SelectListInputAction::ModelsClearAll};
    }
    if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelsToggleProvider, input.event.key))
    {
      return SelectListInputResult{.selected_item_index = selectors_.view()->selected_item_index,
                                   .query = selectors_.view()->query,
                                   .action = SelectListInputAction::ModelsToggleProvider};
    }
    if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelsReorderUp, input.event.key))
    {
      return SelectListInputResult{
          .selected_item_index = selectors_.view()->selected_item_index, .query = selectors_.view()->query, .action = SelectListInputAction::ModelsReorderUp};
    }
    if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelsReorderDown, input.event.key))
    {
      return SelectListInputResult{
          .selected_item_index = selectors_.view()->selected_item_index, .query = selectors_.view()->query, .action = SelectListInputAction::ModelsReorderDown};
    }
  }
  return handle_select_list_input(*selectors_.view(), input.event, frontend_.key_bindings());
}

int TuiSession::run(std::function<bool()> const& before_shutdown)
{
  using Signals = core::Signals;
  if (Signals::received(terminal_signals))
    return 130;
  if (!service_mermaid_presentation() || !renderer_.render())
    return 1;

  while (true)
  {
    using Key = terminal::Key;
    if (!service_mermaid_presentation())
    {
      terminal_write_failed_ = true;
      break;
    }
    if (stop_for_branch_summary(branch_summary_.poll()))
      break;
    if (!renderer_.flush_pending_render_if_due())
    {
      terminal_write_failed_ = true;
      break;
    }
    auto const input_poll_started_at = std::chrono::steady_clock::now();
    if (subagent_workspace_.poll(input_poll_started_at) && !renderer_.request_render())
    {
      terminal_write_failed_ = true;
      break;
    }
    auto input_poll_delay = kIdleInputPollDelay;
    if (auto workspace_wait = subagent_workspace_.time_until_poll(input_poll_started_at))
      input_poll_delay = std::min(input_poll_delay, std::chrono::ceil<std::chrono::milliseconds>(*workspace_wait));
    if (renderer_.has_pending_render())
    {
      input_poll_delay = std::min(input_poll_delay, std::chrono::ceil<std::chrono::milliseconds>(renderer_.time_until_pending_render()));
    }
    auto const maybe_input = runtime_input::read_curses_input_with_timeout(input_poll_delay);
    if (!maybe_input)
    {
      if (subagent_workspace_.poll() && !renderer_.request_render())
      {
        terminal_write_failed_ = true;
        break;
      }
      if (!renderer_.flush_pending_render_if_due() || !maybe_reload_display_settings())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    auto const input = *maybe_input;
    if (Signals::received(terminal_signals))
    {
      bool const exit_requested = Signals::try_obtain(SIGTERM);
      if (exit_requested)
        terminal_signal_received_ = true;

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
      if (branch_summary_.active())
      {
        if (AI_LIKELY(exit_requested || Signals::try_obtain(SIGINT)))
        {
          // We have an active branch summary and successfully claimed either terminal signal.
          // Request cancellation; additionally request eventual exit for SIGTERM.
          auto const mode = exit_requested ? BranchSummaryCancelMode::ExitAfterSettlement : BranchSummaryCancelMode::ContinueSession;
          if (stop_for_branch_summary(branch_summary_.cancel(mode)))
            break;
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
        if (draft_state_.draft.text.empty())
        {
          // and an empty draft. Leave the loop too.
          terminal_signal_received_ = true;
          break;
        }
        // and nonempty draft. Clear draft and continue if successful.
        static_cast<void>(action_controller_.clear_draft_for_interrupt());
        presentation_state_.snapshot.selected_slash_command_index = draft_state_.selected_slash_command_index;
        if (!renderer_.render())
        {
          terminal_write_failed_ = true;
          break;
        }
        continue;
      }
      // Failed to claim either terminal signal; fall-through.
    }
    if (input.resize)
    {
      renderer_.wheel_governor.reset();
      transcript_search_.refresh_after_resize();
      if (!renderer_.render())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    if (!runtime_wheel_input_accepted(renderer_.wheel_governor, input.event.key))
      continue;
    if (presentation_state_.snapshot.command_output && !presentation_state_.snapshot.permission_prompt && !presentation_state_.snapshot.question_prompt)
    {
      auto const geometry = command_output_geometry(presentation_state_.snapshot.width, presentation_state_.snapshot.height);
      auto const output_input = handle_command_output_input(*presentation_state_.snapshot.command_output, input.event, geometry.width, geometry.height,
                                                            presentation_state_.snapshot.tool_presentation);
      if (output_input.action == CommandOutputInputAction::Dismiss)
      {
        presentation_state_.snapshot.command_output.reset();
        presentation_state_.snapshot.status = "command output closed";
        if (!renderer_.request_render())
        {
          terminal_write_failed_ = true;
          break;
        }
        continue;
      }
      if (output_input.action == CommandOutputInputAction::Redraw)
      {
        presentation_state_.snapshot.command_output->scroll_offset = output_input.scroll_offset;
        if (!renderer_.request_render())
        {
          terminal_write_failed_ = true;
          break;
        }
        continue;
      }
      // Typing or clearing starts the next draft directly; pointer input remains
      // captured by the modal so transcript/composer hit targets stay suppressed.
      if (input.event.key == Key::Character || input.event.key == Key::Space || input.event.key == Key::CtrlU)
      {
        presentation_state_.snapshot.command_output.reset();
        presentation_state_.snapshot.status.clear();
      }
      else
      {
        continue;
      }
    }
    if (subagent_workspace_.active())
    {
      auto const handled = subagent_workspace_.handle_input(input.event);
      if (handled.beep)
        terminal_context_.beep();
      if (handled.changed && !renderer_.request_render())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    if (branch_summary_.active())
    {
      if (stop_for_branch_summary(branch_summary_.handle_input(input.event)))
        break;
      continue;
    }
    clear_reasoning_feedback_for_user_input(presentation_state_.snapshot);
    if (auto handled = transcript_search_.handle_input(input.event))
    {
      if (!*handled)
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    if (auto handled = prompt_stash_.handle_selector_input(input.event))
    {
      if (!*handled)
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    if (selectors_.view())
    {
      settings_.synchronize_selector();
      if (selectors_.kind() != ActiveSelectList::Session)
        session_selector_.cancel_confirmation();

      auto input_result = classify_selector_input(input);
      if (input_result.action == SelectListInputAction::Redraw && selectors_.view())
      {
        session_selector_.cancel_confirmation();
        if (selectors_.kind() == ActiveSelectList::Settings)
          settings_.highlight(input_result.selected_item_index, std::move(input_result.query));
        else
          selectors_.update_selection(input_result.selected_item_index, std::move(input_result.query));
      }
      else if (input_result.action == SelectListInputAction::Cancel && selectors_.kind() == ActiveSelectList::Settings)
      {
        settings_.cancel();
      }
      else if (input_result.action == SelectListInputAction::Resolve && selectors_.kind() == ActiveSelectList::Settings)
      {
        auto result = settings_.resolve(input_result);
        if (auto* command = std::get_if<std::string>(&result))
        {
          bool const edits_keybindings = *command == "/keybindings set ";
          draft_state_.clear_selection();
          static_cast<void>(replace_composer_draft(draft_state_.draft, std::move(*command)));
          draft_state_.selected_slash_command_index = 0;
          draft_state_.path_completion_force_active = false;
          draft_state_.draft_scroll_offset = 0;
          draft_state_.history_index.reset();
          draft_state_.draft_input.clear();
          presentation_state_.snapshot.status = edits_keybindings ? "keybinding edit command drafted" : "command drafted";
        }
        else
        {
          switch (std::get<SettingsDialogRequest>(result))
          {
            case SettingsDialogRequest::Models:
              static_cast<void>(action_controller_.open_model_selector());
              break;
            case SettingsDialogRequest::ScopedModels:
              static_cast<void>(action_controller_.open_scoped_model_selector());
              break;
            case SettingsDialogRequest::Reasoning:
              static_cast<void>(action_controller_.open_reasoning_selector(false));
              break;
            case SettingsDialogRequest::Hotkeys:
              selectors_.open(ActiveSelectList::Hotkeys, hotkeys_select_list_view(frontend_.key_bindings()));
              presentation_state_.snapshot.status = "keybindings opened";
              renderer_.transcript_scroll_offset = 0;
              break;
            case SettingsDialogRequest::None:
              break;
          }
        }
      }
      else if (input_result.action == SelectListInputAction::Resolve && selectors_.kind() == ActiveSelectList::ScopedModels && selectors_.view())
      {
        auto const* selected_item = selectors_.selected_item(input_result.selected_item_index);
        if (!selected_item || !selected_item->enabled || selected_item->value.empty())
        {
          presentation_state_.snapshot.status = "scoped model cannot be toggled from this row";
          terminal_context_.beep();
        }
        else
        {
          auto updated = frontend_.on_scoped_model_toggled(*selectors_.view(), selected_item->value);
          if (updated)
          {
            selectors_.replace_preserving_selection(std::move(*updated), input_result.selected_item_index, input_result.query);
            presentation_state_.snapshot.status = "scoped model toggled";
          }
          else
          {
            presentation_state_.snapshot.status = updated.error().format();
            terminal_context_.beep();
          }
        }
      }
      else if (input_result.action == SelectListInputAction::ModelsEnableAll && selectors_.kind() == ActiveSelectList::ScopedModels && selectors_.view())
      {
        auto updated = frontend_.on_scoped_model_enable_all(*selectors_.view(), selectors_.visible_values(input_result.query));
        if (updated)
        {
          selectors_.replace_preserving_selection(std::move(*updated), input_result.selected_item_index, input_result.query);
          presentation_state_.snapshot.status = "scoped models enabled";
        }
        else
        {
          presentation_state_.snapshot.status = updated.error().format();
          terminal_context_.beep();
        }
      }
      else if (input_result.action == SelectListInputAction::ModelsClearAll && selectors_.kind() == ActiveSelectList::ScopedModels && selectors_.view())
      {
        auto updated = frontend_.on_scoped_model_clear_all(*selectors_.view(), selectors_.visible_values(input_result.query));
        if (updated)
        {
          selectors_.replace_preserving_selection(std::move(*updated), input_result.selected_item_index, input_result.query);
          presentation_state_.snapshot.status = "scoped models cleared";
        }
        else
        {
          presentation_state_.snapshot.status = updated.error().format();
          terminal_context_.beep();
        }
      }
      else if (input_result.action == SelectListInputAction::ModelsToggleProvider && selectors_.kind() == ActiveSelectList::ScopedModels && selectors_.view())
      {
        auto const* selected_item = selectors_.selected_item(input_result.selected_item_index);
        if (!selected_item || selected_item->value.empty())
        {
          presentation_state_.snapshot.status = "provider toggle unavailable from this row";
          terminal_context_.beep();
        }
        else
        {
          auto updated = frontend_.on_scoped_model_toggle_provider(*selectors_.view(), selected_item->value);
          if (updated)
          {
            selectors_.replace_preserving_selection(std::move(*updated), input_result.selected_item_index, input_result.query);
            presentation_state_.snapshot.status = "scoped provider toggled";
          }
          else
          {
            presentation_state_.snapshot.status = updated.error().format();
            terminal_context_.beep();
          }
        }
      }
      else if ((input_result.action == SelectListInputAction::ModelsReorderUp || input_result.action == SelectListInputAction::ModelsReorderDown) &&
               selectors_.kind() == ActiveSelectList::ScopedModels && selectors_.view())
      {
        auto const* selected_item = selectors_.selected_item(input_result.selected_item_index);
        if (!selected_item || selected_item->value.empty())
        {
          presentation_state_.snapshot.status = "scoped model reorder unavailable from this row";
          terminal_context_.beep();
        }
        else
        {
          auto updated =
              frontend_.on_scoped_model_reorder(*selectors_.view(), selected_item->value, input_result.action == SelectListInputAction::ModelsReorderUp);
          if (updated)
          {
            selectors_.replace_preserving_selection(std::move(*updated), input_result.selected_item_index, input_result.query);
            presentation_state_.snapshot.status = "scoped model order updated";
          }
          else
          {
            presentation_state_.snapshot.status = updated.error().format();
            terminal_context_.beep();
          }
        }
      }
      else if (input_result.action == SelectListInputAction::ModelsSave && selectors_.kind() == ActiveSelectList::ScopedModels && selectors_.view())
      {
        auto saved = frontend_.on_scoped_model_save();
        if (saved)
        {
          presentation_state_.snapshot.status = *saved;
        }
        else
        {
          presentation_state_.snapshot.status = saved.error().format();
          terminal_context_.beep();
        }
      }
      else if (input_result.action == SelectListInputAction::SummarizeParent && selectors_.kind() == ActiveSelectList::Session && selectors_.view())
      {
        session_selector_.cancel_confirmation();
        if (input_result.selected_item_index >= selectors_.view()->items.size() || !selectors_.view()->items[input_result.selected_item_index].enabled ||
            selectors_.view()->items[input_result.selected_item_index].value.empty())
        {
          presentation_state_.snapshot.status = "parent summary is unavailable from this row";
          terminal_context_.beep();
        }
        else
        {
          auto prior_view = *selectors_.view();
          prior_view.query = input_result.query;
          prior_view.selected_item_index = input_result.selected_item_index;
          auto const selected_value = prior_view.items[input_result.selected_item_index].value;
          if (stop_for_branch_summary(branch_summary_.start(std::move(prior_view), input_result.selected_item_index, selected_value)))
            break;
        }
      }
      else if (selectors_.kind() == ActiveSelectList::Session &&
               (input_result.action == SelectListInputAction::CycleSort || input_result.action == SelectListInputAction::ToggleNamedFilter ||
                input_result.action == SelectListInputAction::TogglePathDisplay || input_result.action == SelectListInputAction::ToggleArchivedFilter ||
                input_result.action == SelectListInputAction::ToggleLabelTimestamp))
      {
        session_selector_.browse(input_result);
      }
      else if (selectors_.kind() == ActiveSelectList::Session &&
               (input_result.action == SelectListInputAction::Rename || input_result.action == SelectListInputAction::Label))
      {
        if (auto command = session_selector_.draft_command(input_result))
        {
          draft_state_.clear_selection();
          static_cast<void>(replace_composer_draft(draft_state_.draft, std::move(*command)));
          draft_state_.draft_input.clear();
          draft_state_.history_index.reset();
          draft_state_.draft_scroll_offset = 0;
        }
      }
      else if ((input_result.action == SelectListInputAction::BranchParent || input_result.action == SelectListInputAction::BranchChild) &&
               selectors_.kind() == ActiveSelectList::Session)
      {
        if (auto opened = session_selector_.navigate_branch(input_result))
        {
          if (*opened)
            settle_opened_session(std::move(**opened));
          else
          {
            static_cast<void>(synchronize_committed_session());
            presentation_state_.snapshot.status = opened->error().format();
            terminal_context_.beep();
          }
        }
      }
      else if (input_result.action == SelectListInputAction::Archive || input_result.action == SelectListInputAction::ArchiveNoninvasive)
      {
        session_selector_.archive(input_result);
      }
      else if (input_result.action == SelectListInputAction::Resolve && selectors_.kind() == ActiveSelectList::Session)
      {
        auto opened = session_selector_.resolve(input_result);
        if (opened)
          settle_opened_session(std::move(*opened));
        else
        {
          static_cast<void>(synchronize_committed_session());
          presentation_state_.snapshot.status = opened.error().format();
          terminal_context_.beep();
        }
      }
      else if (input_result.action == SelectListInputAction::Resolve || input_result.action == SelectListInputAction::Cancel)
      {
        using namespace runtime_views;

        std::string selected_value;
        if (input_result.action == SelectListInputAction::Resolve && selectors_.view() && input_result.selected_item_index < selectors_.view()->items.size())
        {
          selected_value = selectors_.view()->items[input_result.selected_item_index].value;
        }
        auto const resolved_list = selectors_.kind();
        selectors_.close();
        if (resolved_list == ActiveSelectList::Overview)
          presentation_state_.snapshot.startup_overview.reset();
        session_selector_.cancel_confirmation();
        if (input_result.action == SelectListInputAction::Cancel)
        {
          presentation_state_.snapshot.status = resolved_list == ActiveSelectList::Reasoning ? "thinking mode unchanged" : "view canceled";
        }
        else if (resolved_list == ActiveSelectList::Hotkeys && !selected_value.empty())
        {
          auto draft_command = std::string("/keybindings set ") + selected_value + " ";
          draft_state_.clear_selection();
          static_cast<void>(replace_composer_draft(draft_state_.draft, std::move(draft_command)));
          draft_state_.selected_slash_command_index = 0;
          draft_state_.path_completion_force_active = false;
          draft_state_.draft_scroll_offset = 0;
          draft_state_.history_index.reset();
          draft_state_.draft_input.clear();
          presentation_state_.snapshot.status = "keybinding edit command drafted";
        }
        else if (resolved_list == ActiveSelectList::Model)
        {
          auto selected = dispatch_tui_selector_authority(presentation_state_.snapshot, selectors_, "switching model…",
                                                          std::bind_front(&RuntimeRenderer::render, &renderer_),
                                                          [&]() { return frontend_.on_model_selected(selected_value); });
          if (selected)
          {
            presentation_state_.refresh_after_backend_action(frontend_);
            presentation_state_.snapshot.status = std::move(*selected);
            if (!action_controller_.open_reasoning_selector(true))
            {
              terminal_write_failed_ = true;
              break;
            }
          }
          else
          {
            presentation_state_.snapshot.status = selected.error().format();
            terminal_context_.beep();
          }
        }
        else if (resolved_list == ActiveSelectList::Reasoning)
        {
          auto level = selected_value == "default" ? std::optional<std::string>{} : std::optional<std::string>{selected_value};
          auto selected = dispatch_tui_selector_authority(presentation_state_.snapshot, selectors_, "setting thinking mode…",
                                                          std::bind_front(&RuntimeRenderer::render, &renderer_),
                                                          [&]() { return frontend_.on_reasoning_selected(std::move(level)); });
          if (selected)
          {
            presentation_state_.refresh_after_backend_action(frontend_);
            presentation_state_.snapshot.status = std::move(*selected);
          }
          else
          {
            presentation_state_.snapshot.status = selected.error().format();
            terminal_context_.beep();
          }
        }
        else if (resolved_list == ActiveSelectList::ForkUserTurn)
        {
          if (selected_value.empty())
          {
            presentation_state_.snapshot.status = "selected user turn is unavailable";
            terminal_context_.beep();
          }
          else
          {
            // Paint truthful pending authority status before the blocking fork
            // callback, matching other session-open selectors.
            presentation_state_.snapshot.status = "forking session…";
            if (!renderer_.render())
            {
              terminal_write_failed_ = true;
              break;
            }
            auto opened = frontend_.on_fork_user_turn_selected(selected_value);
            if (opened)
            {
              // Session open and fork share the committed identity boundary.
              settle_opened_session(std::move(*opened));
            }
            else
            {
              // A fork may commit before an ancillary catalog refresh reports a
              // warning/failure. Reconcile committed identity on either result.
              static_cast<void>(synchronize_committed_session());
              presentation_state_.snapshot.status = opened.error().format();
              terminal_context_.beep();
            }
          }
        }
        else if (resolved_list == ActiveSelectList::CopyUserTurn)
        {
          auto decision = evaluate_copy_user_turn_selection(
              selected_value, [this](std::string_view entry_id) { return frontend_.on_read_user_turn_text(entry_id); },
              runtime_transcript::copy_text_to_terminal_clipboard);
          settle_local_command_status(presentation_state_.snapshot, decision.status);
          if (decision.beep)
            terminal_context_.beep();
        }
        else
        {
          presentation_state_.snapshot.status = "view closed";
        }
      }
      if (!renderer_.request_render())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    auto const event = input.event;
    if (auto handled = navigation_.handle_sidebar_drawer_input(event))
    {
      if (!*handled)
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }

    if (draft_state_.jump_mode != ComposerJumpMode::None)
    {
      if (key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ||
          key_matches_action(frontend_.key_bindings(), TuiAction::JumpBackward, event.key))
      {
        draft_state_.jump_mode = ComposerJumpMode::None;
        presentation_state_.snapshot.status = "jump cancelled";
      }
      else if (auto const target = printable_jump_target(input))
      {
        bool const forward = draft_state_.jump_mode == ComposerJumpMode::Forward;
        draft_state_.jump_mode = ComposerJumpMode::None;
        draft_state_.pending_escape_clear = false;
        draft_state_.history_index.reset();
        draft_state_.draft_input.clear();
        draft_state_.selected_slash_command_index = 0;
        draft_state_.slash_palette_suppressed = false;
        draft_state_.path_completion_force_active = false;
        draft_state_.draft_scroll_offset = 0;
        draft_state_.clear_selection();
        presentation_state_.snapshot.status = jump_composer_draft_to_character(draft_state_.draft, *target, forward)
                                                  ? (forward ? "jumped forward" : "jumped backward")
                                                  : "jump character not found";
      }
      else
      {
        draft_state_.jump_mode = ComposerJumpMode::None;
      }
      if (event.key == Key::Character || event.key == Key::Space || key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ||
          key_matches_action(frontend_.key_bindings(), TuiAction::JumpBackward, event.key))
      {
        presentation_state_.snapshot.selected_slash_command_index = draft_state_.selected_slash_command_index;
        if (!renderer_.request_render())
        {
          terminal_write_failed_ = true;
          break;
        }
        continue;
      }
    }
    bool const ctrl_d_delete_forward =
        event.key == Key::CtrlD && key_matches_action(frontend_.key_bindings(), TuiAction::DeleteForward, event.key) && !draft_state_.draft.text.empty();
    bool const delete_forward_action =
        key_matches_action(frontend_.key_bindings(), TuiAction::DeleteForward, event.key) && (event.key != Key::CtrlD || ctrl_d_delete_forward);

    if (key_matches_action(frontend_.key_bindings(), TuiAction::CopyLatestAssistant, event.key))
    {
      if (!action_controller_.copy_latest_assistant_message())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PromptStash, event.key))
    {
      if (!prompt_stash_.trigger())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    else if (event.key == Key::Character)
    {
      insert_input_text(input);
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::MessageFollowUp, event.key))
    {
      auto const action = handle_submit();
      if (action == RuntimeSubmitDisposition::BreakLoop)
        break;
      if (action == RuntimeSubmitDisposition::ContinueLoop)
        continue;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::NewLine, event.key))
    {
      draft_state_.insert_newline();
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ExternalEditor, event.key))
    {
      if (!action_controller_.open_external_editor())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::Suspend, event.key))
    {
      if (!action_controller_.suspend_to_background())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ClipboardPasteImage, event.key))
    {
      if (!action_controller_.paste_clipboard_image())
      {
        terminal_write_failed_ = true;
        break;
      }
      continue;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ||
             key_matches_action(frontend_.key_bindings(), TuiAction::JumpBackward, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.path_completion_force_active = false;
      draft_state_.jump_mode =
          key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ? ComposerJumpMode::Forward : ComposerJumpMode::Backward;
      presentation_state_.snapshot.status =
          key_matches_action(frontend_.key_bindings(), TuiAction::JumpForward, event.key) ? "jump forward: type character" : "jump backward: type character";
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteBackward, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.draft_scroll_offset = 0;
      if (!draft_state_.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::DeleteBackward));
    }
    else if (delete_forward_action)
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.draft_scroll_offset = 0;
      if (!draft_state_.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::DeleteForward));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteWordBackward, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.draft_scroll_offset = 0;
      if (!draft_state_.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::DeleteWordBackward));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteWordForward, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.draft_scroll_offset = 0;
      if (!draft_state_.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::DeleteWordForward));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteToLineStart, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.draft_scroll_offset = 0;
      if (!draft_state_.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::DeleteToLineStart));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::DeleteToLineEnd, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.draft_scroll_offset = 0;
      if (!draft_state_.delete_selection())
        static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::DeleteToLineEnd));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CopySelection, event.key) && draft_state_.selection_bounds())
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.path_completion_force_active = false;
      static_cast<void>(draft_state_.copy_selection(presentation_state_.snapshot));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CopySelection, event.key) && renderer_.has_transcript_selection())
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.path_completion_force_active = false;
      static_cast<void>(renderer_.copy_transcript_selection());
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ClearInput, event.key) &&
             (!draft_state_.draft.text.empty() || !key_matches_action(frontend_.key_bindings(), TuiAction::Interrupt, event.key)))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.path_completion_force_active = false;
      draft_state_.draft_scroll_offset = 0;
      draft_state_.clear_selection();
      presentation_state_.snapshot.status = apply_composer_draft_action(draft_state_.draft, TuiAction::ClearInput) ? "input cleared" : "input already empty";
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::AutocompleteAccept, event.key) && navigation_.slash_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      select_slash_command();
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::AutocompleteAccept, event.key) && navigation_.file_reference_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      select_file_reference();
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::AutocompleteAccept, event.key) && navigation_.path_completion_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      select_path_completion();
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::AutocompleteAccept, event.key) && force_path_completion())
    {
      draft_state_.pending_escape_clear = false;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ModeToggle, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.path_completion_force_active = false;
      if (auto result = frontend_.on_toggle_mode(); !result)
      {
        presentation_state_.snapshot.status = result.error().format();
      }
      else
      {
        presentation_state_.refresh_prompt_metadata();
        presentation_state_.snapshot.status = "mode switched to " + presentation_state_.snapshot.mode;
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::Interrupt, event.key))
    {
      draft_state_.path_completion_force_active = false;
      if (!draft_state_.draft.text.empty())
      {
        static_cast<void>(action_controller_.clear_draft_for_interrupt());
        presentation_state_.snapshot.selected_slash_command_index = draft_state_.selected_slash_command_index;
        if (!renderer_.render())
        {
          terminal_write_failed_ = true;
          break;
        }
        continue;
      }
      break;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::Exit, event.key))
    {
      break;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::VariantCycle, event.key))
    {
      draft_state_.pending_escape_clear = false;
      action_controller_.cycle_reasoning();
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ReasoningSelect, event.key))
    {
      draft_state_.pending_escape_clear = false;
      if (!action_controller_.open_reasoning_selector())
      {
        terminal_write_failed_ = true;
        break;
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ThinkingToggle, event.key))
    {
      draft_state_.pending_escape_clear = false;
      action_controller_.toggle_thinking_visibility();
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::OverviewToggle, event.key))
    {
      draft_state_.pending_escape_clear = false;
      if (!toggle_startup_overview())
      {
        terminal_write_failed_ = true;
        break;
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelSelect, event.key))
    {
      if (!action_controller_.open_model_selector())
      {
        terminal_write_failed_ = true;
        break;
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelCycleForward, event.key))
    {
      draft_state_.pending_escape_clear = false;
      action_controller_.cycle_model(true);
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::ModelCycleBackward, event.key))
    {
      draft_state_.pending_escape_clear = false;
      action_controller_.cycle_model(false);
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::SessionResume, event.key) ||
             key_matches_action(frontend_.key_bindings(), TuiAction::SessionTree, event.key))
    {
      draft_state_.pending_escape_clear = false;
      if (!session_selector_.open())
      {
        terminal_write_failed_ = true;
        break;
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::SessionNew, event.key) ||
             key_matches_action(frontend_.key_bindings(), TuiAction::SessionFork, event.key))
    {
      auto const action = handle_submit(key_matches_action(frontend_.key_bindings(), TuiAction::SessionNew, event.key) ? "/new" : "/fork");
      if (action == RuntimeSubmitDisposition::BreakLoop)
        break;
      if (action == RuntimeSubmitDisposition::ContinueLoop)
        continue;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::MessageDequeue, event.key))
    {
      draft_state_.pending_escape_clear = false;
      presentation_state_.snapshot.status = "queued-message restore is available during active runs";
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::TranscriptHalfPageUp, event.key))
    {
      navigation_.scroll_up(navigation_.transcript_page_size());
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::TranscriptHalfPageDown, event.key))
    {
      navigation_.scroll_down(navigation_.transcript_page_size());
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PageUp, event.key))
    {
      navigation_.scroll_up(navigation_.transcript_page_size());
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PageDown, event.key))
    {
      navigation_.scroll_down(navigation_.transcript_page_size());
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::MessagePrev, event.key))
    {
      navigation_.scroll_to_message_boundary(true);
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::MessageNext, event.key))
    {
      navigation_.scroll_to_message_boundary(false);
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::JumpToBottom, event.key))
    {
      navigation_.jump_to_bottom("live tail");
    }
    else if (event.key == Key::MouseWheelUp)
    {
      navigation_.scroll_up(kTranscriptWheelScrollRows);
    }
    else if (event.key == Key::MouseWheelDown)
    {
      navigation_.scroll_down(kTranscriptWheelScrollRows);
    }
    else if (event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick || event.key == Key::MouseLeftDrag || event.key == Key::MouseLeftRelease ||
             event.key == Key::MousePointerCancel)
    {
      draft_state_.pending_escape_clear = false;
      auto const begins_click = event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick;
      bool palette_claimed = false;
      if (begins_click)
      {
        if (auto const clicked = slash_palette_selection_for_screen_position(presentation_state_.snapshot, event.mouse_row, event.mouse_column))
        {
          renderer_.clear_transcript_selection();
          draft_state_.clear_selection();
          draft_state_.selected_slash_command_index = *clicked;
          select_slash_command();
          palette_claimed = true;
        }
        else if (auto const clicked = detail::file_reference_palette_selection_for_screen_position_cached(
                     presentation_state_.snapshot, event.mouse_row, event.mouse_column, renderer_.completion_cache,
                     presentation_state_.snapshot.file_references_generation))
        {
          renderer_.clear_transcript_selection();
          draft_state_.clear_selection();
          draft_state_.selected_slash_command_index = *clicked;
          select_file_reference();
          palette_claimed = true;
        }
        else if (auto const clicked = detail::path_completion_palette_selection_for_screen_position_cached(
                     presentation_state_.snapshot, event.mouse_row, event.mouse_column, renderer_.completion_cache,
                     presentation_state_.snapshot.file_references_generation))
        {
          renderer_.clear_transcript_selection();
          draft_state_.clear_selection();
          draft_state_.selected_slash_command_index = *clicked;
          select_path_completion();
          palette_claimed = true;
        }
      }
      if (!palette_claimed)
      {
        auto const transcript_mouse =
            renderer_.handle_transcript_selection_mouse(event, std::bind_front(&RuntimeNavigationController::toggle_tool_details_at, &navigation_),
                                                        std::bind_front(&RuntimeNavigationController::toggle_thinking_at, &navigation_));
        if (transcript_mouse == TranscriptSelectionMouseResult::Ignored)
        {
          if (begins_click)
          {
            if (auto const cursor = composer_input_cursor_for_screen_position(presentation_state_.snapshot, event.mouse_row, event.mouse_column))
            {
              renderer_.clear_transcript_selection();
              draft_state_.draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft_state_.draft, *cursor);
              draft_state_.clear_selection();
              if (event.key == Key::MouseLeftPress)
              {
                draft_state_.draft_selection_anchor = draft_state_.draft.cursor;
                draft_state_.draft_selection_cursor = draft_state_.draft.cursor;
                draft_state_.mouse_selecting = true;
              }
              draft_state_.draft.vertical_column = std::string::npos;
              draft_state_.draft.yank_start = std::string::npos;
              draft_state_.draft.yank_end = std::string::npos;
              draft_state_.history_index.reset();
              draft_state_.draft_input.clear();
              presentation_state_.snapshot.status = "cursor moved";
            }
          }
          else if (draft_state_.mouse_selecting)
          {
            if (auto const cursor = composer_input_cursor_for_screen_position(presentation_state_.snapshot, event.mouse_row, event.mouse_column))
            {
              auto const next_cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft_state_.draft, *cursor);
              draft_state_.draft_selection_cursor = next_cursor;
              draft_state_.draft.cursor = next_cursor;
              draft_state_.draft.vertical_column = std::string::npos;
              draft_state_.draft.yank_start = std::string::npos;
              draft_state_.draft.yank_end = std::string::npos;
              draft_state_.history_index.reset();
              draft_state_.draft_input.clear();
              presentation_state_.snapshot.status = draft_state_.selection_bounds() ? "selection active" : "cursor moved";
            }
            if (event.key == Key::MouseLeftRelease)
              draft_state_.mouse_selecting = false;
          }
        }
      }
    }
    else if (draft_state_.extend_selection_for_key(event.key, presentation_state_.snapshot))
    {
      renderer_.clear_transcript_selection();
    }
    else if (event.key == Key::CtrlHome)
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      draft_state_.draft.cursor = 0;
      draft_state_.draft.vertical_column = std::string::npos;
      draft_state_.draft.yank_start = std::string::npos;
      draft_state_.draft.yank_end = std::string::npos;
    }
    else if (event.key == Key::CtrlEnd)
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      draft_state_.draft.cursor = draft_state_.draft.text.size();
      draft_state_.draft.vertical_column = std::string::npos;
      draft_state_.draft.yank_start = std::string::npos;
      draft_state_.draft.yank_end = std::string::npos;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorLeft, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::CursorLeft));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorRight, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::CursorRight));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorLineStart, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::CursorLineStart));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorLineEnd, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::CursorLineEnd));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorWordLeft, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::CursorWordLeft));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorWordRight, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::CursorWordRight));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PalettePrev, event.key) && navigation_.slash_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      auto const match_count = filter_slash_commands(draft_state_.draft.text, draft_state_.draft.cursor, presentation_state_.snapshot.slash_commands).size();
      if (match_count == 0)
      {
        presentation_state_.snapshot.status = "no matching slash commands";
      }
      else
      {
        draft_state_.selected_slash_command_index = previous_slash_palette_selection(
            draft_state_.draft.text, draft_state_.draft.cursor, presentation_state_.snapshot.slash_commands, draft_state_.selected_slash_command_index);
        presentation_state_.snapshot.status = "command " + std::to_string(draft_state_.selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PalettePrev, event.key) && navigation_.file_reference_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      auto const match_count = navigation_.completion_match_count();
      if (match_count == 0)
      {
        presentation_state_.snapshot.status = "no matching file references";
      }
      else
      {
        draft_state_.selected_slash_command_index = navigation_.previous_completion(draft_state_.selected_slash_command_index);
        presentation_state_.snapshot.status = "reference " + std::to_string(draft_state_.selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PalettePrev, event.key) && navigation_.path_completion_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      auto const match_count = navigation_.completion_match_count();
      if (match_count == 0)
      {
        presentation_state_.snapshot.status = "no matching paths";
      }
      else
      {
        draft_state_.selected_slash_command_index = navigation_.previous_completion(draft_state_.selected_slash_command_index);
        presentation_state_.snapshot.status = "path " + std::to_string(draft_state_.selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::HistoryPrev, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.path_completion_force_active = false;
      draft_state_.draft_scroll_offset = 0;
      if (browse_composer_input_history(draft_state_.draft, draft_state_.input_history, draft_state_.history_index, draft_state_.draft_input, true))
      {
        presentation_state_.snapshot.status = "history previous";
      }
      else
      {
        navigation_.scroll_up(kKeyboardScrollRows);
      }
    }
    else if (event.key == Key::ArrowUp)
    {
      navigation_.scroll_up(kKeyboardScrollRows);
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorUp, event.key) &&
             apply_composer_draft_action(draft_state_.draft, TuiAction::CursorUp))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PaletteNext, event.key) && navigation_.slash_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      auto const match_count = filter_slash_commands(draft_state_.draft.text, draft_state_.draft.cursor, presentation_state_.snapshot.slash_commands).size();
      if (match_count == 0)
      {
        presentation_state_.snapshot.status = "no matching slash commands";
      }
      else
      {
        draft_state_.selected_slash_command_index = next_slash_palette_selection(
            draft_state_.draft.text, draft_state_.draft.cursor, presentation_state_.snapshot.slash_commands, draft_state_.selected_slash_command_index);
        presentation_state_.snapshot.status = "command " + std::to_string(draft_state_.selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PaletteNext, event.key) && navigation_.file_reference_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      auto const match_count = navigation_.completion_match_count();
      if (match_count == 0)
      {
        presentation_state_.snapshot.status = "no matching file references";
      }
      else
      {
        draft_state_.selected_slash_command_index = navigation_.next_completion(draft_state_.selected_slash_command_index);
        presentation_state_.snapshot.status = "reference " + std::to_string(draft_state_.selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PaletteNext, event.key) && navigation_.path_completion_palette_active())
    {
      draft_state_.pending_escape_clear = false;
      auto const match_count = navigation_.completion_match_count();
      if (match_count == 0)
      {
        presentation_state_.snapshot.status = "no matching paths";
      }
      else
      {
        draft_state_.selected_slash_command_index = navigation_.next_completion(draft_state_.selected_slash_command_index);
        presentation_state_.snapshot.status = "path " + std::to_string(draft_state_.selected_slash_command_index + 1) + "/" + std::to_string(match_count);
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::HistoryNext, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
      draft_state_.path_completion_force_active = false;
      draft_state_.draft_scroll_offset = 0;
      if (browse_composer_input_history(draft_state_.draft, draft_state_.input_history, draft_state_.history_index, draft_state_.draft_input, false))
      {
        presentation_state_.snapshot.status = draft_state_.history_index ? "history next" : "history draft";
      }
      else
      {
        navigation_.scroll_down(kKeyboardScrollRows);
      }
    }
    else if (event.key == Key::ArrowDown)
    {
      navigation_.scroll_down(kKeyboardScrollRows);
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::CursorDown, event.key) &&
             apply_composer_draft_action(draft_state_.draft, TuiAction::CursorDown))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.clear_selection();
      draft_state_.history_index.reset();
      draft_state_.draft_input.clear();
      draft_state_.selected_slash_command_index = 0;
      draft_state_.slash_palette_suppressed = false;
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::Undo, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.draft_scroll_offset = 0;
      draft_state_.clear_selection();
      presentation_state_.snapshot.status = apply_composer_draft_action(draft_state_.draft, TuiAction::Undo) ? "undo" : "nothing to undo";
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::Redo, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.draft_scroll_offset = 0;
      draft_state_.clear_selection();
      presentation_state_.snapshot.status = apply_composer_draft_action(draft_state_.draft, TuiAction::Redo) ? "redo" : "nothing to redo";
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::Yank, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.draft_scroll_offset = 0;
      draft_state_.clear_selection();
      presentation_state_.snapshot.status = apply_composer_draft_action(draft_state_.draft, TuiAction::Yank) ? "yanked text" : "nothing to yank";
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::YankPop, event.key))
    {
      draft_state_.pending_escape_clear = false;
      draft_state_.draft_scroll_offset = 0;
      draft_state_.clear_selection();
      presentation_state_.snapshot.status = apply_composer_draft_action(draft_state_.draft, TuiAction::YankPop) ? "yank-pop" : "nothing to yank-pop";
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::DetailsToggle, event.key))
    {
      draft_state_.pending_escape_clear = false;
      renderer_.synchronize_detached_transcript_layout();
      presentation_state_.snapshot.tool_presentation =
          presentation_state_.snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich : ToolPresentation::Expanded;
      presentation_state_.snapshot.status = "tool details " + std::string(to_string(presentation_state_.snapshot.tool_presentation));
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::PromptAllow, event.key) ||
             key_matches_action(frontend_.key_bindings(), TuiAction::PromptDeny, event.key))
    {
      draft_state_.pending_escape_clear = false;
      presentation_state_.snapshot.status = "prompt action is only available while a prompt is active";
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::Cancel, event.key))
    {
      if (draft_state_.selection_bounds())
      {
        draft_state_.clear_selection();
        draft_state_.pending_escape_clear = false;
        presentation_state_.snapshot.status.clear();
      }
      else if (renderer_.has_transcript_selection())
      {
        renderer_.clear_transcript_selection();
        draft_state_.pending_escape_clear = false;
        presentation_state_.snapshot.status.clear();
      }
      else if (navigation_.slash_palette_active() || navigation_.file_reference_palette_active() || navigation_.path_completion_palette_active())
      {
        draft_state_.pending_escape_clear = false;
        draft_state_.slash_palette_suppressed = true;
        draft_state_.selected_slash_command_index = 0;
        draft_state_.path_completion_force_active = false;
        draft_state_.history_index.reset();
        draft_state_.draft_input.clear();
        presentation_state_.snapshot.status.clear();
      }
      else if (!draft_state_.draft.text.empty())
      {
        if (draft_state_.pending_escape_clear)
        {
          static_cast<void>(apply_composer_draft_action(draft_state_.draft, TuiAction::ClearInput));
          draft_state_.selected_slash_command_index = 0;
          draft_state_.path_completion_force_active = false;
          draft_state_.history_index.reset();
          draft_state_.draft_input.clear();
          draft_state_.draft_scroll_offset = 0;
          draft_state_.pending_escape_clear = false;
          presentation_state_.snapshot.status = "input cleared";
        }
        else
        {
          draft_state_.pending_escape_clear = true;
          presentation_state_.snapshot.status = "press Esc again to clear";
        }
      }
      else
      {
        draft_state_.pending_escape_clear = false;
        presentation_state_.snapshot.status = "escape ignored";
      }
    }
    else if (key_matches_action(frontend_.key_bindings(), TuiAction::Submit, event.key))
    {
      if (event.key == Key::Enter && draft_state_.convert_backslash_enter_to_newline(presentation_state_.snapshot))
      {
        if (!renderer_.request_render())
        {
          terminal_write_failed_ = true;
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
      insert_input_text(input);
    }
    presentation_state_.snapshot.selected_slash_command_index = draft_state_.selected_slash_command_index;
    if (!renderer_.request_render())
    {
      terminal_write_failed_ = true;
      break;
    }
  }

  mermaid_presentation_.shutdown();
  plugin_ui_.shutdown(presentation_state_.snapshot);
  if (!before_shutdown())
    terminal_write_failed_ = true;
  return terminal_signal_received_ ? 130 : (terminal_write_failed_ ? 1 : 0);
}

} // namespace ava::tui
