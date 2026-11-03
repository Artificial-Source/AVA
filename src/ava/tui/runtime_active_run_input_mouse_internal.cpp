#include "sys.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/Composer.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/terminal.h"
#include "ava/core/Application.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <utility>

namespace ava::tui {

// Mouse hit-testing/wheel.
ActiveRun::InputHandling ActiveRun::handle_mouse_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.screen();
  auto const& draft = draft_state_.draft();

  using Key = terminal::Key;
  if (active_event.key == Key::MouseLeftPress || active_event.key == Key::MouseLeftClick || active_event.key == Key::MouseLeftDrag ||
      active_event.key == Key::MouseLeftRelease || active_event.key == Key::MousePointerCancel)
  {
    auto const begins_click = active_event.key == Key::MouseLeftPress || active_event.key == Key::MouseLeftClick;
    if (begins_click)
    {
      if (auto const clicked = slash_palette_selection_for_screen_position(snapshot, active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.select_completion(*clicked);
        if (auto const disabled_reason = slash_command_selection_disabled_reason(draft.text, draft.cursor, snapshot.slash_commands, *clicked))
        {
          snapshot.status = "command disabled: " + *disabled_reason;
          ava::core::Application::instance().terminal_context().beep();
        }
        else
        {
          auto selection = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, *clicked);
          draft_state_.accept_completion(std::move(selection.text), selection.cursor, ComposerCompletionOrigin::Pointer);
          snapshot.status = "command selected - press Enter to queue";
        }
        return to_input_handling(renderer_.request_render());
      }
      if (auto const clicked = renderer_.file_reference_completion_at(active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.select_completion(*clicked);
        if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(*clicked))
        {
          snapshot.status = "reference disabled: " + *disabled_reason;
          ava::core::Application::instance().terminal_context().beep();
        }
        else
        {
          auto selection = navigation_.selected_completion_text(*clicked);
          draft_state_.accept_completion(std::move(selection.text), selection.cursor, ComposerCompletionOrigin::Pointer);
          snapshot.status = "file reference selected";
        }
        return to_input_handling(renderer_.request_render());
      }
      if (auto const clicked = renderer_.path_completion_at(active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.select_completion(*clicked);
        if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(*clicked))
        {
          snapshot.status = "path disabled: " + *disabled_reason;
          ava::core::Application::instance().terminal_context().beep();
        }
        else
        {
          auto selection = navigation_.selected_completion_text(*clicked);
          draft_state_.accept_completion(std::move(selection.text), selection.cursor, ComposerCompletionOrigin::PointerPath);
          snapshot.status = "path selected";
        }
        return to_input_handling(renderer_.request_render());
      }
    }

    auto const transcript_mouse =
        renderer_.handle_transcript_selection_mouse(active_event, std::bind_front(&RuntimeNavigationController::toggle_tool_details_at, &navigation_),
                                                    std::bind_front(&RuntimeNavigationController::toggle_thinking_at, &navigation_));
    if (transcript_mouse != TranscriptSelectionMouseResult::Ignored)
      return to_input_handling(renderer_.request_render());

    if (begins_click)
    {
      if (auto const cursor = composer_input_cursor_for_screen_position(snapshot, active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.begin_pointer_selection(*cursor, active_event.key == Key::MouseLeftPress, ComposerEditContext::ActiveRun);
        snapshot.status = "cursor moved";
        return to_input_handling(renderer_.request_render());
      }
    }
    else if (draft_state_.mouse_selecting())
    {
      if (auto const cursor = composer_input_cursor_for_screen_position(snapshot, active_event.mouse_row, active_event.mouse_column))
      {
        draft_state_.extend_pointer_selection(*cursor, ComposerEditContext::ActiveRun);
        snapshot.status = draft_state_.selection_bounds() ? "selection active" : "cursor moved";
      }
      if (active_event.key == Key::MouseLeftRelease)
        draft_state_.end_pointer_selection();
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
ActiveRun::InputHandling ActiveRun::handle_restricted_toggle_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto& snapshot = presentation_state_.screen();

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
  if (active_event.key == terminal::Key::Space)
  {
    draft_state_.insert_input(active_input, snapshot, ComposerEditContext::ActiveRun);
    return to_input_handling(renderer_.request_render());
  }
  return InputHandling::Unhandled;
}

}  // namespace ava::tui
