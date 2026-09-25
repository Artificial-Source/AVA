#include "sys.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_active_run_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/terminal.h"
#include "ava/core/Application.h"

#include <chrono>
#include <cstddef>
#include <utility>

namespace ava::tui {

// Mouse hit-testing/wheel.
RuntimeActiveRunController::InputHandling RuntimeActiveRunController::handle_mouse_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_state = draft_state_;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& completion_cache = renderer_.completion_cache;

  if (active_event.key == Key::MouseLeftPress || active_event.key == Key::MouseLeftClick || active_event.key == Key::MouseLeftDrag ||
      active_event.key == Key::MouseLeftRelease || active_event.key == Key::MousePointerCancel)
  {
    auto const begins_click = active_event.key == Key::MouseLeftPress || active_event.key == Key::MouseLeftClick;
    if (begins_click)
    {
      if (auto const clicked = slash_palette_selection_for_screen_position(snapshot, active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        selected_slash_command_index = *clicked;
        if (auto const disabled_reason = slash_command_selection_disabled_reason(draft.text, draft.cursor, snapshot.slash_commands, *clicked))
        {
          snapshot.status = "command disabled: " + *disabled_reason;
          ava::core::Application::instance().terminal_context().beep();
        }
        else
        {
          auto selection = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, *clicked);
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
          selected_slash_command_index = 0;
          snapshot.status = "command selected - press Enter to queue";
        }
        return to_input_handling(renderer_.request_render());
      }
      if (auto const clicked = detail::file_reference_palette_selection_for_screen_position_cached(snapshot, active_event.mouse_row, active_event.mouse_column,
                                                                                                   completion_cache, snapshot.file_references_generation))
      {
        renderer_.clear_transcript_selection();
        selected_slash_command_index = *clicked;
        if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(*clicked))
        {
          snapshot.status = "reference disabled: " + *disabled_reason;
          ava::core::Application::instance().terminal_context().beep();
        }
        else
        {
          auto selection = navigation_.selected_completion_text(*clicked);
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
          selected_slash_command_index = 0;
          snapshot.status = "file reference selected";
        }
        return to_input_handling(renderer_.request_render());
      }
      if (auto const clicked = detail::path_completion_palette_selection_for_screen_position_cached(snapshot, active_event.mouse_row, active_event.mouse_column,
                                                                                                    completion_cache, snapshot.file_references_generation))
      {
        renderer_.clear_transcript_selection();
        selected_slash_command_index = *clicked;
        if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(*clicked))
        {
          snapshot.status = "path disabled: " + *disabled_reason;
          ava::core::Application::instance().terminal_context().beep();
        }
        else
        {
          auto selection = navigation_.selected_completion_text(*clicked);
          draft_state.clear_selection();
          static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
          selected_slash_command_index = 0;
          path_completion_force_active = false;
          snapshot.status = "path selected";
        }
        return to_input_handling(renderer_.request_render());
      }
    }

    auto const transcript_mouse = renderer_.handle_transcript_selection_mouse(
        active_event, [&](std::size_t item) { return navigation_.toggle_tool_details_at(item); },
        [&](std::size_t item) { return navigation_.toggle_thinking_at(item); });
    if (transcript_mouse != TranscriptSelectionMouseResult::Ignored)
      return to_input_handling(renderer_.request_render());

    if (begins_click)
    {
      if (auto const cursor = composer_input_cursor_for_screen_position(snapshot, active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, *cursor);
        draft_state.clear_selection();
        if (active_event.key == Key::MouseLeftPress)
        {
          draft_state.draft_selection_anchor = draft.cursor;
          draft_state.draft_selection_cursor = draft.cursor;
          draft_state.mouse_selecting = true;
        }
        snapshot.status = "cursor moved";
        return to_input_handling(renderer_.request_render());
      }
    }
    else if (draft_state.mouse_selecting)
    {
      if (auto const cursor = composer_input_cursor_for_screen_position(snapshot, active_event.mouse_row, active_event.mouse_column))
      {
        auto const next_cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, *cursor);
        draft_state.draft_selection_cursor = next_cursor;
        draft.cursor = next_cursor;
        snapshot.status = draft_state.selection_bounds() ? "selection active" : "cursor moved";
      }
      if (active_event.key == Key::MouseLeftRelease)
        draft_state.mouse_selecting = false;
      return to_input_handling(renderer_.request_render());
    }
  }
  if (active_event.key == Key::MouseWheelUp)
  {
    navigation_.scroll_up(kTranscriptWheelScrollRows);
    return to_input_handling(renderer_.request_render());
  }
  if (active_event.key == Key::MouseWheelDown)
  {
    navigation_.scroll_down(kTranscriptWheelScrollRows);
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

// Active-run restricted toggles/model actions and Space fallback.
RuntimeActiveRunController::InputHandling RuntimeActiveRunController::handle_restricted_toggle_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.snapshot;

  if (is_action(active_event, TuiAction::DetailsToggle))
  {
    renderer_.synchronize_detached_transcript_layout();
    snapshot.tool_presentation = snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich : ToolPresentation::Expanded;
    snapshot.status = "tool details " + std::string(to_string(snapshot.tool_presentation));
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::OverviewToggle))
  {
    return to_input_handling(action_controller_.toggle_startup_overview());
  }
  if (is_action(active_event, TuiAction::VariantCycle))
  {
    snapshot.status = "reasoning can be changed between turns";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ReasoningSelect))
  {
    snapshot.status = "thinking mode can be changed between turns";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ThinkingToggle))
  {
    action_controller_.toggle_thinking_visibility();
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ModelSelect))
  {
    snapshot.status = "model selection is available between turns";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ModelCycleForward) || is_action(active_event, TuiAction::ModelCycleBackward))
  {
    snapshot.status = "model cycling is available between turns";
    return to_input_handling(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessageDequeue))
  {
    snapshot.status = "queued-message restore is available during active runs";
    return to_input_handling(renderer_.request_render());
  }
  if (active_event.key == Key::Space)
  {
    insert_active_text(active_input);
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

}  // namespace ava::tui
