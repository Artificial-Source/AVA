#include "sys.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/ActiveRunInbox.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/command_output.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_commands_internal.h"
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

bool ActiveRun::is_action(InputEvent const& event, TuiAction action) const
{
  return key_matches_action(frontend_.key_bindings(), action, event.key);
}

ActiveRun::InputHandling ActiveRun::to_input_handling(bool render_result)
{
  return render_result ? InputHandling::Handled : InputHandling::RenderFailed;
}

ComposerSnapshot const& ActiveRun::completion_snapshot()
{
  auto const& snapshot = presentation_state_.screen();
  presentation_state_.publish_completion(draft_state_);
  static_cast<void>(renderer_.refresh_completion_cache());
  return snapshot;
}

bool ActiveRun::restore_latest_queued_message()
{
  auto& active_queues = active_queues_;
  if (!active_queues || !active_queues->restore_latest)
  {
    presentation_state_.publish_status("active-run restore unavailable");
    return renderer_.request_render();
  }
  auto restored = active_queues->restore_latest();
  if (!restored)
  {
    presentation_state_.publish_status(restored.error().format());
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  auto const restored_text = restored->steering ? "/steer " + restored->message : restored->message;
  draft_state_.restore_draft(restored_text);
  presentation_state_.publish_status(restored->steering ? "steering restored" : "follow-up restored");
  return renderer_.request_render();
}

std::optional<bool> ActiveRun::run_active_command()
{
  auto const& snapshot = presentation_state_.screen();
  auto const& draft = draft_state_.draft();
  auto& active_queues = active_queues_;
  if (draft.text == "/sidebar")
  {
    draft_state_.record_submission(draft.text);
    draft_state_.reset_local_command();
    presentation_state_.publish_sidebar_drawer(true, 0);
    renderer_.show_live_tail(LiveTailReason::LocalCommand);
    presentation_state_.publish_status("session overview opened");
    return renderer_.request_render();
  }
  if (runtime_commands::exact_command(draft.text, "/overview"))
  {
    draft_state_.record_submission(draft.text);
    draft_state_.reset_local_command();
    return action_controller_.toggle_startup_overview();
  }
  if (draft.text.empty())
    return std::nullopt;
  auto const submitted_command = expanded_composer_draft_text(draft);
  if (auto stash_argument = runtime_commands::stash_command_argument(submitted_command))
  {
    draft_state_.record_submission(submitted_command);
    draft_state_.reset_local_command();
    if (stash_argument->empty())
      return prompt_stash_.open_selector();
    if (*stash_argument == "pop")
      return prompt_stash_.pop_latest();
    if (*stash_argument == "clear")
      return prompt_stash_.clear();
    presentation_state_.publish_status("invalid_argument: usage: /stash [pop|clear]");
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  if (runtime_commands::exact_command(submitted_command, "/jobs"))
  {
    draft_state_.record_submission(submitted_command);
    draft_state_.reset_local_command();
    static_cast<void>(subagent_workspace_.open_selector());
    return renderer_.request_render();
  }
  if (auto search_query = search_command_argument(submitted_command))
  {
    draft_state_.record_submission(submitted_command);
    draft_state_.reset_local_command();
    static_cast<void>(transcript_search_.open(std::move(*search_query)));
    return renderer_.request_render();
  }
  if (runtime_commands::local_display_settings_command(submitted_command))
  {
    draft_state_.record_submission(submitted_command);
    draft_state_.reset_local_command();
    auto applied = frontend_.on_local_display_settings_action(submitted_command);
    if (!applied)
    {
      presentation_state_.publish_local_error(submitted_command, applied.error().format());
      ava::core::Application::instance().terminal_context().beep();
    }
    else
    {
      presentation_state_.apply_local_display_settings(frontend_, std::move(applied->settings));
      presentation_state_.publish_local_completion(submitted_command, std::move(applied->output));
    }
    return renderer_.request_render();
  }
  if (submitted_command == "/details" || submitted_command == "/details compact" || submitted_command == "/details rich" ||
      submitted_command == "/details expanded")
  {
    draft_state_.record_submission(submitted_command);
    renderer_.synchronize_detached_transcript_layout();
    if (submitted_command == "/details compact")
      presentation_state_.publish_tool_presentation(ToolPresentation::Compact);
    else if (submitted_command == "/details rich")
      presentation_state_.publish_tool_presentation(ToolPresentation::Rich);
    else if (submitted_command == "/details expanded")
      presentation_state_.publish_tool_presentation(ToolPresentation::Expanded);
    else
      presentation_state_.publish_tool_presentation(snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich
                                                                                                             : ToolPresentation::Expanded);
    draft_state_.reset_local_command();
    renderer_.show_live_tail(LiveTailReason::LocalCommand);
    presentation_state_.publish_local_status("tool details " + std::string(to_string(snapshot.tool_presentation)));
    return renderer_.request_render();
  }
  if (submitted_command == "/thinking" || submitted_command == "/thinking details")
  {
    draft_state_.record_submission(submitted_command);
    draft_state_.reset_local_command();
    if (submitted_command == "/thinking details")
    {
      if (!navigation_.toggle_latest_thinking_details())
      {
        presentation_state_.publish_status("no completed long thinking block to expand");
        ava::core::Application::instance().terminal_context().beep();
      }
    }
    else
    {
      presentation_state_.publish_thinking_visibility(!snapshot.thinking_visible);
      presentation_state_.invalidate_transcript_layout();
      presentation_state_.publish_status(snapshot.thinking_visible ? "thinking visible" : "thinking hidden");
    }
    return renderer_.request_render();
  }
  if (auto const tool_query = tool_command_argument(submitted_command))
  {
    draft_state_.record_submission(submitted_command);
    auto const indexed_tool = latest_matching_indexed_tool(snapshot, *tool_query);
    auto const transcript_index = indexed_tool ? indexed_provider_tool_transcript_index(snapshot, *indexed_tool) : std::nullopt;
    if (transcript_index && navigation_.toggle_tool_details_at(*transcript_index))
    {
      auto const& tool = *snapshot.transcript[*transcript_index].tool;
      presentation_state_.publish_status((tool_query->empty() ? "latest tool details " : "matching tool details ") +
                                         std::string(to_string(detail::tool_card_presentation(tool, snapshot.tool_presentation))));
    }
    else if (indexed_tool)
    {
      presentation_state_.publish_local_output(submitted_command, {}, {indexed_tool->tool});
    }
    else
    {
      presentation_state_.publish_status(tool_query->empty() ? "no tool details to show" : "no matching tool details to show");
      ava::core::Application::instance().terminal_context().beep();
    }
    draft_state_.reset_local_command();
    return renderer_.request_render();
  }
  if (!active_queues || !active_queues->run_nonblocking_command)
    return std::nullopt;
  auto const dispatch = dispatch_tui_active_nonblocking_command_gated(completion_snapshot(), *active_queues, submitted_command);
  if (dispatch.kind == TuiActiveNonblockingCommandDispatchKind::Unrecognized)
    return std::nullopt;
  if (dispatch.kind == TuiActiveNonblockingCommandDispatchKind::Blocked)
  {
    presentation_state_.publish_status(dispatch.status);
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  draft_state_.record_submission(submitted_command);
  presentation_state_.publish_local_completion(submitted_command, dispatch.output);
  draft_state_.reset_local_command();
  renderer_.show_live_tail(LiveTailReason::Requested);
  if (dispatch.output.empty())
    presentation_state_.publish_status("job command complete");
  return renderer_.request_render();
}

std::optional<bool> ActiveRun::reject_disabled_visible_completion()
{
  auto const& current = completion_snapshot();
  if (auto const disabled_status = detail::disabled_visible_completion_selection_status(current, renderer_.completion_cache()))
  {
    presentation_state_.publish_status(*disabled_status);
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  return std::nullopt;
}

bool ActiveRun::queue_active_draft(bool follow_up_only)
{
  auto const& draft = draft_state_.draft();
  auto& active_queues = active_queues_;
  if (auto handled = reject_disabled_visible_completion())
    return *handled;
  if (draft.text.empty())
  {
    presentation_state_.publish_status("type a follow-up before queueing");
    return renderer_.request_render();
  }
  if (!follow_up_only && draft.text == "/restore")
  {
    return restore_latest_queued_message();
  }
  auto const steering_prefix = std::string_view("/steer ");
  bool const steering_draft = !follow_up_only && draft.text.starts_with(steering_prefix);
  if ((draft.text.starts_with('/') || shell_helper_submission(draft.text)) && !steering_draft)
  {
    presentation_state_.publish_status("commands run between turns");
    return renderer_.request_render();
  }
  if (inbox_->stop_requested())
  {
    presentation_state_.publish_status("stop requested; queueing disabled");
    return renderer_.request_render();
  }
  if (!active_queues)
  {
    presentation_state_.publish_status("active-run queue unavailable");
    return renderer_.request_render();
  }
  if (steering_draft && !active_queues->queue_steering)
  {
    presentation_state_.publish_status("active-run steering unavailable");
    return renderer_.request_render();
  }
  if (!steering_draft && !active_queues->queue_follow_up)
  {
    presentation_state_.publish_status("active-run follow-up unavailable");
    return renderer_.request_render();
  }

  auto queued_text = expanded_composer_draft_text(draft);
  auto queued = steering_draft ? active_queues->queue_steering(queued_text.substr(steering_prefix.size())) : active_queues->queue_follow_up(queued_text);
  if (!queued)
  {
    presentation_state_.publish_status(queued.error().format());
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  draft_state_.record_submission(queued_text);
  draft_state_.reset_after_queue();
  presentation_state_.publish_status(steering_draft ? "steering queued" : "follow-up queued");
  return renderer_.request_render();
}

bool ActiveRun::handle_search_input_event(runtime_input::RuntimeInput const& active_input)
{
  using Key = terminal::Key;
  if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
    renderer_.reset_wheel_input();
  if (active_input.resize)
  {
    transcript_search_.refresh_after_resize();
    return renderer_.render();
  }
  if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) && !renderer_.accept_wheel_input(active_input.event.key))
  {
    return true;
  }
  return transcript_search_.handle_input(active_input.event).value_or(true);
}

bool ActiveRun::dispatch_active_input(runtime_input::RuntimeInput const& active_input)
{
  using Key = terminal::Key;
  auto const& snapshot = presentation_state_.screen();
  if (snapshot.command_output && !snapshot.permission_prompt && !snapshot.question_prompt)
  {
    if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
      renderer_.reset_wheel_input();
    if (active_input.resize)
      return renderer_.render();
    if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) && !renderer_.accept_wheel_input(active_input.event.key))
    {
      return true;
    }
    auto const geometry = command_output_geometry(snapshot.width, snapshot.height);
    auto const output_input =
        handle_command_output_input(*snapshot.command_output, active_input.event, geometry.width, geometry.height, snapshot.tool_presentation);
    if (output_input.action == CommandOutputInputAction::Dismiss)
    {
      presentation_state_.publish_command_output(std::nullopt);
      presentation_state_.publish_status("command output closed");
      return renderer_.request_render();
    }
    if (output_input.action == CommandOutputInputAction::Redraw)
    {
      presentation_state_.publish_command_output_scroll(output_input.scroll_offset);
      return renderer_.request_render();
    }
    if (active_input.event.key == Key::Character || active_input.event.key == Key::Space || active_input.event.key == Key::CtrlU)
    {
      presentation_state_.publish_command_output(std::nullopt);
      presentation_state_.publish_status({});
    }
    else
    {
      return true;
    }
  }
  if (subagent_workspace_.active())
  {
    if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
      renderer_.reset_wheel_input();
    if (active_input.resize)
      return renderer_.render();
    if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) && !renderer_.accept_wheel_input(active_input.event.key))
    {
      return true;
    }
    auto const handled = subagent_workspace_.handle_input(active_input.event);
    if (handled.beep)
      ava::core::Application::instance().terminal_context().beep();
    return !handled.changed || renderer_.request_render();
  }
  if (transcript_search_.is_open())
    return handle_search_input_event(active_input);
  if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
    renderer_.reset_wheel_input();
  if (active_input.resize)
    return renderer_.render();
  if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) && !renderer_.accept_wheel_input(active_input.event.key))
  {
    return true;
  }
  if (auto handled = prompt_stash_.handle_selector_input(active_input.event))
    return *handled;

  auto result = handle_preemptive_input(active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_completion_acceptance(active_input);
  if (result != InputHandling::Unhandled)
    return result == InputHandling::Handled;
  result = handle_active_command_input(active_input);
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
