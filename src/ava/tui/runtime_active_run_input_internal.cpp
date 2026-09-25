#include "sys.h"
#include "ava/tui/command_output.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_active_run_internal.h"
#include "ava/tui/runtime_active_run_state_internal.h"
#include "ava/tui/runtime_commands_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/terminal.h"
#include "ava/tui/tool_cards.h"
#include "ava/core/Application.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ava::tui {
using runtime_commands::search_command_argument;
using runtime_commands::shell_helper_submission;
using runtime_commands::tool_command_argument;
using runtime_transcript::push_history;

bool RuntimeActiveRunController::is_action(InputEvent const& event, TuiAction action) const
{
  return key_matches_action(options_.key_bindings, action, event.key);
}

RuntimeActiveRunController::InputHandling RuntimeActiveRunController::to_input_handling(bool render_result)
{
  return render_result ? InputHandling::Handled : InputHandling::RenderFailed;
}

ComposerSnapshot const& RuntimeActiveRunController::completion_snapshot()
{
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& completion_cache = renderer_.completion_cache;
  snapshot.input = draft.text;
  snapshot.input_cursor = draft.cursor;
  snapshot.selected_slash_command_index = selected_slash_command_index;
  snapshot.slash_palette_suppressed = slash_palette_suppressed;
  snapshot.path_completion_force_active = path_completion_force_active;
  detail::refresh_completion_match_cache(completion_cache, snapshot, snapshot.file_references_generation);
  return snapshot;
}

bool RuntimeActiveRunController::restore_latest_queued_message(RuntimeActiveRunState& state)
{
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& active_queues = state.active_queues;
  if (!active_queues || !active_queues->restore_latest)
  {
    snapshot.status = "active-run restore unavailable";
    return renderer_.request_render();
  }
  auto restored = active_queues->restore_latest();
  if (!restored)
  {
    snapshot.status = restored.error().format();
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  auto const restored_text = restored->steering ? "/steer " + restored->message : restored->message;
  draft_state_.clear_selection();
  static_cast<void>(replace_composer_draft(draft, restored_text));
  draft_scroll_offset = 0;
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  slash_palette_suppressed = false;
  path_completion_force_active = false;
  snapshot.status = restored->steering ? "steering restored" : "follow-up restored";
  return renderer_.request_render();
}

std::optional<bool> RuntimeActiveRunController::run_active_command(RuntimeActiveRunState& state)
{
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;
  auto& jump_mode = draft_state_.jump_mode;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& input_history = draft_state_.input_history;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& transcript_scroll_offset = renderer_.transcript_scroll_offset;
  auto& detached_new_output_count = renderer_.detached_new_output_count;
  auto& detached_sidebar_snapshot = renderer_.detached_sidebar_snapshot;
  auto& active_queues = state.active_queues;
  auto clear_local_command_draft = [&]() {
    draft_state_.clear_selection();
    reset_composer_draft(draft);
    jump_mode = ComposerJumpMode::None;
    draft_scroll_offset = 0;
    history_index.reset();
    draft_input.clear();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    path_completion_force_active = false;
  };
  if (draft.text == "/sidebar")
  {
    push_history(input_history, draft.text);
    draft_state_.clear_selection();
    reset_composer_draft(draft);
    jump_mode = ComposerJumpMode::None;
    draft_scroll_offset = 0;
    history_index.reset();
    draft_input.clear();
    selected_slash_command_index = 0;
    slash_palette_suppressed = false;
    path_completion_force_active = false;
    snapshot.sidebar_drawer_visible = true;
    snapshot.sidebar_drawer_scroll_offset = 0;
    transcript_scroll_offset = 0;
    detached_new_output_count = 0;
    detached_sidebar_snapshot.reset();
    snapshot.status = "session overview opened";
    return renderer_.request_render();
  }
  if (runtime_commands::exact_command(draft.text, "/overview"))
  {
    push_history(input_history, draft.text);
    clear_local_command_draft();
    return action_controller_.toggle_startup_overview();
  }
  if (draft.text.empty())
    return std::nullopt;
  auto const submitted_command = expanded_composer_draft_text(draft);
  if (auto stash_argument = runtime_commands::stash_command_argument(submitted_command))
  {
    push_history(input_history, submitted_command);
    clear_local_command_draft();
    if (stash_argument->empty())
      return prompt_stash_.open_selector();
    if (*stash_argument == "pop")
      return prompt_stash_.pop_latest();
    if (*stash_argument == "clear")
      return prompt_stash_.clear();
    snapshot.status = "invalid_argument: usage: /stash [pop|clear]";
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  if (runtime_commands::exact_command(submitted_command, "/jobs"))
  {
    push_history(input_history, submitted_command);
    clear_local_command_draft();
    static_cast<void>(subagent_workspace_.open_selector());
    return renderer_.request_render();
  }
  if (auto search_query = search_command_argument(submitted_command))
  {
    push_history(input_history, submitted_command);
    clear_local_command_draft();
    static_cast<void>(transcript_search_.open(std::move(*search_query)));
    return renderer_.request_render();
  }
  if (submitted_command == "/details" || submitted_command == "/details compact" || submitted_command == "/details rich" ||
      submitted_command == "/details expanded")
  {
    push_history(input_history, submitted_command);
    renderer_.synchronize_detached_transcript_layout();
    if (submitted_command == "/details compact")
      snapshot.tool_presentation = ToolPresentation::Compact;
    else if (submitted_command == "/details rich")
      snapshot.tool_presentation = ToolPresentation::Rich;
    else if (submitted_command == "/details expanded")
      snapshot.tool_presentation = ToolPresentation::Expanded;
    else
      snapshot.tool_presentation = snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich : ToolPresentation::Expanded;
    clear_local_command_draft();
    transcript_scroll_offset = 0;
    detached_new_output_count = 0;
    detached_sidebar_snapshot.reset();
    settle_local_command_status(snapshot, "tool details " + std::string(to_string(snapshot.tool_presentation)));
    return renderer_.request_render();
  }
  if (submitted_command == "/thinking" || submitted_command == "/thinking details")
  {
    push_history(input_history, submitted_command);
    clear_local_command_draft();
    if (submitted_command == "/thinking details")
    {
      if (!navigation_.toggle_latest_thinking_details())
      {
        snapshot.status = "no completed long thinking block to expand";
        ava::core::Application::instance().terminal_context().beep();
      }
    }
    else
    {
      snapshot.thinking_visible = !snapshot.thinking_visible;
      ++snapshot.transcript_generation;
      snapshot.status = snapshot.thinking_visible ? "thinking visible" : "thinking hidden";
    }
    return renderer_.request_render();
  }
  if (auto const tool_query = tool_command_argument(submitted_command))
  {
    push_history(input_history, submitted_command);
    auto const indexed_tool = latest_matching_indexed_tool(snapshot, *tool_query);
    auto const transcript_index = indexed_tool ? indexed_provider_tool_transcript_index(snapshot, *indexed_tool) : std::nullopt;
    if (transcript_index && navigation_.toggle_tool_details_at(*transcript_index))
    {
      auto const& tool = *snapshot.transcript[*transcript_index].tool;
      snapshot.status = (tool_query->empty() ? "latest tool details " : "matching tool details ") +
                        std::string(to_string(detail::tool_card_presentation(tool, snapshot.tool_presentation)));
    }
    else if (indexed_tool)
    {
      open_command_output(snapshot, submitted_command, {}, {indexed_tool->tool});
    }
    else
    {
      snapshot.status = tool_query->empty() ? "no tool details to show" : "no matching tool details to show";
      ava::core::Application::instance().terminal_context().beep();
    }
    clear_local_command_draft();
    return renderer_.request_render();
  }
  if (!active_queues || !active_queues->run_nonblocking_command)
    return std::nullopt;
  auto const dispatch = dispatch_tui_active_nonblocking_command_gated(completion_snapshot(), *active_queues, submitted_command);
  if (dispatch.kind == TuiActiveNonblockingCommandDispatchKind::Unrecognized)
    return std::nullopt;
  if (dispatch.kind == TuiActiveNonblockingCommandDispatchKind::Blocked)
  {
    snapshot.status = dispatch.status;
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  push_history(input_history, submitted_command);
  settle_local_command_completion(snapshot, submitted_command, dispatch.output);
  draft_state_.clear_selection();
  reset_composer_draft(draft);
  jump_mode = ComposerJumpMode::None;
  draft_scroll_offset = 0;
  transcript_scroll_offset = 0;
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  slash_palette_suppressed = false;
  path_completion_force_active = false;
  if (dispatch.output.empty())
    snapshot.status = "job command complete";
  return renderer_.request_render();
}

std::optional<bool> RuntimeActiveRunController::reject_disabled_visible_completion()
{
  auto& snapshot = presentation_state_.snapshot;
  auto& completion_cache = renderer_.completion_cache;
  auto const& current = completion_snapshot();
  if (auto const disabled_status = detail::disabled_visible_completion_selection_status(current, completion_cache))
  {
    snapshot.status = *disabled_status;
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  return std::nullopt;
}

bool RuntimeActiveRunController::queue_active_draft(RuntimeActiveRunState& state, bool follow_up_only)
{
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;
  auto& jump_mode = draft_state_.jump_mode;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& input_history = draft_state_.input_history;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& active_queues = state.active_queues;
  auto& run_cancel_requested = state.run_cancel_requested;
  if (auto handled = reject_disabled_visible_completion())
    return *handled;
  if (draft.text.empty())
  {
    snapshot.status = "type a follow-up before queueing";
    return renderer_.request_render();
  }
  if (!follow_up_only && draft.text == "/restore")
  {
    return restore_latest_queued_message(state);
  }
  auto const steering_prefix = std::string_view("/steer ");
  bool const steering_draft = !follow_up_only && draft.text.starts_with(steering_prefix);
  if ((draft.text.starts_with('/') || shell_helper_submission(draft.text)) && !steering_draft)
  {
    snapshot.status = "commands run between turns";
    return renderer_.request_render();
  }
  if (run_cancel_requested.load())
  {
    snapshot.status = "stop requested; queueing disabled";
    return renderer_.request_render();
  }
  if (!active_queues)
  {
    snapshot.status = "active-run queue unavailable";
    return renderer_.request_render();
  }
  if (steering_draft && !active_queues->queue_steering)
  {
    snapshot.status = "active-run steering unavailable";
    return renderer_.request_render();
  }
  if (!steering_draft && !active_queues->queue_follow_up)
  {
    snapshot.status = "active-run follow-up unavailable";
    return renderer_.request_render();
  }

  auto queued_text = expanded_composer_draft_text(draft);
  auto queued = steering_draft ? active_queues->queue_steering(queued_text.substr(steering_prefix.size())) : active_queues->queue_follow_up(queued_text);
  if (!queued)
  {
    snapshot.status = queued.error().format();
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  push_history(input_history, queued_text);
  draft_state_.clear_selection();
  reset_composer_draft(draft);
  jump_mode = ComposerJumpMode::None;
  draft_scroll_offset = 0;
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  slash_palette_suppressed = false;
  snapshot.status = steering_draft ? "steering queued" : "follow-up queued";
  return renderer_.request_render();
}

void RuntimeActiveRunController::insert_active_text(runtime_input::RuntimeInput const& active_input)
{
  auto& snapshot = presentation_state_.snapshot;
  auto& draft = draft_state_.draft;
  auto& draft_state = draft_state_;
  auto& pending_escape_clear = draft_state_.pending_escape_clear;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;
  pending_escape_clear = false;
  snapshot.local_command_feedback.reset();
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  slash_palette_suppressed = false;
  draft_scroll_offset = 0;
  auto const text = active_input.text.empty() ? std::string(1, active_input.event.character) : active_input.text;
  if (active_input.bracketed_paste)
  {
    static_cast<void>(draft_state.delete_selection());
    static_cast<void>(insert_composer_paste_text(draft, text));
    snapshot.status = "pasted into draft safely";
  }
  else
  {
    if (!draft_state.replace_selection(text))
      static_cast<void>(insert_composer_draft_text(draft, text));
  }
}

std::optional<bool> RuntimeActiveRunController::handle_transcript_search_input(runtime_input::RuntimeInput const& active_input)
{
  if (!transcript_search_.is_open())
    return std::nullopt;
  if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
    renderer_.wheel_governor.reset();
  if (active_input.resize)
  {
    transcript_search_.refresh_after_resize();
    return renderer_.render();
  }
  if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) &&
      !runtime_wheel_input_accepted(renderer_.wheel_governor, active_input.event.key))
  {
    return true;
  }
  return transcript_search_.handle_input(active_input.event).value_or(true);
}

bool RuntimeActiveRunController::handle_input(RuntimeActiveRunState& state, runtime_input::RuntimeInput const& active_input)
{
  auto& snapshot = presentation_state_.snapshot;
  if (snapshot.command_output && !snapshot.permission_prompt && !snapshot.question_prompt)
  {
    if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
      renderer_.wheel_governor.reset();
    if (active_input.resize)
      return renderer_.render();
    if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) &&
        !runtime_wheel_input_accepted(renderer_.wheel_governor, active_input.event.key))
    {
      return true;
    }
    auto const geometry = command_output_geometry(snapshot.width, snapshot.height);
    auto const output_input =
        handle_command_output_input(*snapshot.command_output, active_input.event, geometry.width, geometry.height, snapshot.tool_presentation);
    if (output_input.action == CommandOutputInputAction::Dismiss)
    {
      snapshot.command_output.reset();
      snapshot.status = "command output closed";
      return renderer_.request_render();
    }
    if (output_input.action == CommandOutputInputAction::Redraw)
    {
      snapshot.command_output->scroll_offset = output_input.scroll_offset;
      return renderer_.request_render();
    }
    if (active_input.event.key == Key::Character || active_input.event.key == Key::Space || active_input.event.key == Key::CtrlU)
    {
      snapshot.command_output.reset();
      snapshot.status.clear();
    }
    else
    {
      return true;
    }
  }
  if (subagent_workspace_.active())
  {
    if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
      renderer_.wheel_governor.reset();
    if (active_input.resize)
      return renderer_.render();
    if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) &&
        !runtime_wheel_input_accepted(renderer_.wheel_governor, active_input.event.key))
    {
      return true;
    }
    auto const handled = subagent_workspace_.handle_input(active_input.event);
    if (handled.beep)
      ava::core::Application::instance().terminal_context().beep();
    return !handled.changed || renderer_.request_render();
  }
  if (auto handled = handle_transcript_search_input(active_input))
    return *handled;
  if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
    renderer_.wheel_governor.reset();
  if (active_input.resize)
    return renderer_.render();
  if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) &&
      !runtime_wheel_input_accepted(renderer_.wheel_governor, active_input.event.key))
  {
    return true;
  }
  if (auto handled = prompt_stash_.handle_selector_input(active_input.event))
    return *handled;

  auto result = handle_preemptive_input(state, active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_completion_acceptance(active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_active_command_input(state, active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_composer_edit(active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_navigation_input(active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_navigation_next_input(active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_mouse_input(active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_restricted_toggle_input(active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  return true;
}

}  // namespace ava::tui
