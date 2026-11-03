// Routes ordinary active-turn input; worker prompt claims remain with ActiveRun.
#include "sys.h"
#include "ActiveRunInput.h"
#include "CommandOutput.h"
#include "ava/tui/ActiveRun.h"
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
#include "ava/tui/terminal/Context.h"
#include "ava/tui/tool_cards.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ava::tui {
using runtime_commands::search_command_argument;
using runtime_commands::shell_helper_submission;
using runtime_commands::tool_command_argument;
using runtime_input::printable_jump_target;
namespace {
constexpr std::size_t kKeyboardScrollRows = 3; // TUI keyboard scroll policy shared by these stages.
}

ActiveRunInput::ActiveRunInput(TuiFrontEnd& frontend, RuntimePresentationState& presentation, Composer& composer, RuntimeRenderer& renderer,
                               RuntimePromptStashController& stash, RuntimeNavigationController& navigation, RuntimeActionController& actions,
                               TranscriptSearchController& search, RuntimeSubagentWorkspaceController& workspace, ActiveRun& run, CommandOutput& output,
                               terminal::Context& terminal_context)
    : frontend_(frontend),
      presentation_state_(presentation),
      draft_state_(composer),
      renderer_(renderer),
      prompt_stash_(stash),
      navigation_(navigation),
      action_controller_(actions),
      transcript_search_(search),
      subagent_workspace_(workspace),
      run_(run),
      command_output_(output),
      terminal_context_(terminal_context),
      dispatcher_({std::bind_front(&ActiveRunInput::handle_modal_input, this), std::bind_front(&ActiveRunInput::handle_surface_input, this),
                   std::bind_front(&ActiveRunInput::handle_preemptive_input, this), std::bind_front(&ActiveRunInput::handle_completion_acceptance, this),
                   std::bind_front(&ActiveRunInput::handle_active_command_input, this), std::bind_front(&ActiveRunInput::handle_composer_edit, this),
                   std::bind_front(&ActiveRunInput::handle_navigation_input, this), std::bind_front(&ActiveRunInput::handle_navigation_next_input, this),
                   std::bind_front(&ActiveRunInput::handle_mouse_input, this), std::bind_front(&ActiveRunInput::handle_restricted_toggle_input, this)})
{
}

TuiInputOutcome ActiveRunInput::dispatch(runtime_input::RuntimeInput const& input)
{
  auto const result = dispatcher_.dispatch(input);
  // Unrecognized keys remain consumed by the active turn, never idle dispatch.
  return result == TuiInputOutcome::Pass ? TuiInputOutcome::Handled : result;
}

TuiInputOutcome ActiveRunInput::handle_modal_input(runtime_input::RuntimeInput const& input)
{
  auto const result = run_.handle_modal_input(input);
  return result ? render_outcome(*result) : TuiInputOutcome::Pass;
}

TuiInputOutcome ActiveRunInput::handle_surface_input(runtime_input::RuntimeInput const& input)
{
  using Key = terminal::Key;
  // Modal lifecycle arbitration already ran. Filter each wheel only once before
  // routing output/workspace/stash input, preserving their capture precedence.
  bool const wheel = input.event.key == Key::MouseWheelUp || input.event.key == Key::MouseWheelDown;
  if (!wheel)
    renderer_.reset_wheel_input();
  if (input.resize)
    return render_outcome(renderer_.render());
  if (wheel && !renderer_.accept_wheel_input(input.event.key))
    return TuiInputOutcome::Handled;
  switch (command_output_.handle_input(input.event))
  {
    case CommandOutputDisposition::Consumed:
      return TuiInputOutcome::Handled;
    case CommandOutputDisposition::Redraw:
      return render_outcome(renderer_.request_render());
    case CommandOutputDisposition::Pass:
      break;
  }
  if (subagent_workspace_.active())
  {
    auto const result = subagent_workspace_.handle_input(input.event);
    if (result.beep)
      terminal_context_.beep();
    return render_outcome(!result.changed || renderer_.request_render());
  }
  if (auto result = prompt_stash_.handle_selector_input(input.event))
    return render_outcome(*result);
  return TuiInputOutcome::Pass;
}

bool ActiveRunInput::apply_composer_result(ComposerInputResult result)
{
  presentation_state_.publish_composer_feedback(result);
  if (result.beep)
    terminal_context_.beep();
  return result.handled;
}

bool ActiveRunInput::is_action(InputEvent const& event, TuiAction action) const
{
  return key_matches_action(frontend_.key_bindings(), action, event.key);
}

TuiInputOutcome ActiveRunInput::render_outcome(bool render_result)
{
  return render_result ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
}

ComposerSnapshot const& ActiveRunInput::completion_snapshot()
{
  auto const& snapshot = presentation_state_.screen();
  presentation_state_.publish_completion(draft_state_);
  static_cast<void>(renderer_.refresh_completion_cache());
  return snapshot;
}

bool ActiveRunInput::restore_latest_queued_message()
{
  auto restored = run_.restore_message();
  if (!restored)
    return renderer_.request_render();
  auto const restored_text = restored->steering ? "/steer " + restored->message : restored->message;
  draft_state_.restore_draft(restored_text);
  presentation_state_.publish_status(restored->steering ? "steering restored" : "follow-up restored");
  return renderer_.request_render();
}

std::optional<bool> ActiveRunInput::run_active_command()
{
  auto const& snapshot = presentation_state_.screen();
  auto const& draft = draft_state_.draft();
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
    terminal_context_.beep();
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
      terminal_context_.beep();
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
        terminal_context_.beep();
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
      terminal_context_.beep();
    }
    draft_state_.reset_local_command();
    return renderer_.request_render();
  }
  auto const dispatch = run_.dispatch_command(completion_snapshot(), submitted_command);
  if (dispatch.kind == TuiActiveNonblockingCommandDispatchKind::Unrecognized)
    return std::nullopt;
  if (dispatch.kind == TuiActiveNonblockingCommandDispatchKind::Blocked)
  {
    presentation_state_.publish_status(dispatch.status);
    terminal_context_.beep();
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

std::optional<bool> ActiveRunInput::reject_disabled_visible_completion()
{
  auto const& current = completion_snapshot();
  if (auto const disabled_status = detail::disabled_visible_completion_selection_status(current, renderer_.completion_cache()))
  {
    presentation_state_.publish_status(*disabled_status);
    terminal_context_.beep();
    return renderer_.request_render();
  }
  return std::nullopt;
}

bool ActiveRunInput::queue_active_draft(bool follow_up_only)
{
  auto const& draft = draft_state_.draft();
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
  auto queued_text = expanded_composer_draft_text(draft);
  if (!run_.queue_message(steering_draft ? queued_text.substr(steering_prefix.size()) : queued_text, steering_draft))
    return renderer_.request_render();
  draft_state_.record_submission(queued_text);
  draft_state_.reset_after_queue();
  presentation_state_.publish_status(steering_draft ? "steering queued" : "follow-up queued");
  return renderer_.request_render();
}

detail::ActiveRunCancelDisposition detail::active_run_cancel_disposition(bool has_draft_selection, bool has_transcript_selection) noexcept
{
  if (has_draft_selection)
    return ActiveRunCancelDisposition::ClearDraftSelection;
  if (has_transcript_selection)
    return ActiveRunCancelDisposition::ClearTranscriptSelection;
  return ActiveRunCancelDisposition::RequestStop;
}

// Sidebar drawer, jump mode, cancel/copy/interrupt/exit, character/selection
// and CtrlHome/CtrlEnd. These preempt all draft, completion, and view input.
TuiInputOutcome ActiveRunInput::handle_preemptive_input(runtime_input::RuntimeInput const& active_input)
{
  using Key = terminal::Key;
  auto const& active_event = active_input.event;
  auto const& draft = draft_state_.draft();

  if (auto handled = navigation_.handle_sidebar_drawer_input(active_event))
    return render_outcome(*handled);
  if (draft_state_.jump_mode() != ComposerJumpMode::None)
  {
    if (is_action(active_event, TuiAction::JumpForward) || is_action(active_event, TuiAction::JumpBackward))
    {
      draft_state_.cancel_jump();
      presentation_state_.publish_status("jump cancelled");
      return render_outcome(renderer_.request_render());
    }
    if (auto const target = printable_jump_target(active_input))
    {
      bool const forward = draft_state_.jump_mode() == ComposerJumpMode::Forward;
      presentation_state_.publish_status(draft_state_.jump_to_character(*target) ? (forward ? "jumped forward" : "jumped backward")
                                                                                 : "jump character not found");
      return render_outcome(renderer_.request_render());
    }
    draft_state_.cancel_jump();
  }
  if (is_action(active_event, TuiAction::CopyLatestAssistant))
    return render_outcome(action_controller_.copy_latest_assistant_message());
  if (is_action(active_event, TuiAction::PromptStash))
    return render_outcome(prompt_stash_.trigger());
  if (active_event.key == Key::Escape || is_action(active_event, TuiAction::Cancel))
  {
    switch (detail::active_run_cancel_disposition(draft_state_.selection_bounds().has_value(), renderer_.transcript().has_selection()))
    {
      case detail::ActiveRunCancelDisposition::ClearDraftSelection:
        draft_state_.clear_selection();
        presentation_state_.publish_status({});
        return render_outcome(renderer_.request_render());
      case detail::ActiveRunCancelDisposition::ClearTranscriptSelection:
        renderer_.clear_transcript_selection();
        presentation_state_.publish_status({});
        return render_outcome(renderer_.request_render());
      case detail::ActiveRunCancelDisposition::RequestStop:
        return render_outcome(run_.request_stop());
    }
  }
  if (is_action(active_event, TuiAction::CopySelection) && draft_state_.selection_bounds())
  {
    draft_state_.cancel_escape();
    static_cast<void>(apply_composer_result(draft_state_.copy_selection()));
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CopySelection) && renderer_.transcript().has_selection())
  {
    draft_state_.cancel_escape();
    static_cast<void>(renderer_.copy_transcript_selection());
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::Interrupt))
  {
    if (!draft.text.empty())
    {
      static_cast<void>(action_controller_.clear_draft_for_interrupt());
      presentation_state_.publish_completion(draft_state_);
      return render_outcome(renderer_.render());
    }
    run_.request_close_after_submit();
    return TuiInputOutcome::Handled;
  }
  bool const active_ctrl_d_delete_forward = active_event.key == Key::CtrlD && is_action(active_event, TuiAction::DeleteForward) && !draft.text.empty();
  if (is_action(active_event, TuiAction::Exit) && !active_ctrl_d_delete_forward)
  {
    run_.request_close_after_submit();
    return TuiInputOutcome::Handled;
  }
  if (active_event.key == Key::Character)
  {
    presentation_state_.clear_local_feedback();
    static_cast<void>(apply_composer_result(draft_state_.insert_input(active_input, ComposerEditContext::ActiveRun)));
    return render_outcome(renderer_.request_render());
  }
  if (apply_composer_result(draft_state_.extend_selection_for_key(active_event.key)))
  {
    renderer_.clear_transcript_selection();
    return render_outcome(renderer_.request_render());
  }
  if (active_event.key == Key::CtrlHome || active_event.key == Key::CtrlEnd)
  {
    draft_state_.move_document_boundary(active_event.key == Key::CtrlEnd, ComposerEditContext::ActiveRun);
    return render_outcome(renderer_.request_render());
  }
  return TuiInputOutcome::Pass;
}

// Slash, file-reference, path-forced and open-selection autocomplete acceptance.
TuiInputOutcome ActiveRunInput::handle_completion_acceptance(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto const& snapshot = presentation_state_.screen();
  auto const& draft = draft_state_.draft();

  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.slash_palette_active())
  {
    draft_state_.select_completion(clamp_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index()));
    if (auto const disabled_reason =
            slash_command_selection_disabled_reason(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index()))
    {
      presentation_state_.publish_status("command disabled: " + *disabled_reason);
      terminal_context_.beep();
    }
    else
    {
      auto selection = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index());
      draft_state_.accept_completion(std::move(selection.text), selection.cursor);
      presentation_state_.publish_status("command selected - press Enter to queue");
    }
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.file_reference_palette_active())
  {
    draft_state_.select_completion(navigation_.clamp_completion(draft_state_.completion_index()));
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(draft_state_.completion_index()))
    {
      presentation_state_.publish_status("reference disabled: " + *disabled_reason);
      terminal_context_.beep();
      return render_outcome(renderer_.request_render());
    }
    auto selection = navigation_.selected_completion_text(draft_state_.completion_index());
    draft_state_.accept_completion(std::move(selection.text), selection.cursor);
    presentation_state_.publish_status("file reference selected");
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept) && navigation_.path_completion_palette_active())
  {
    draft_state_.select_completion(navigation_.clamp_completion(draft_state_.completion_index()));
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(draft_state_.completion_index()))
    {
      presentation_state_.publish_status("path disabled: " + *disabled_reason);
      terminal_context_.beep();
      return render_outcome(renderer_.request_render());
    }
    auto selection = navigation_.selected_completion_text(draft_state_.completion_index());
    draft_state_.accept_completion(std::move(selection.text), selection.cursor);
    presentation_state_.publish_status("path selected");
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::AutocompleteAccept) &&
      apply_composer_result(draft_state_.force_path_completion(completion_snapshot(), ComposerEditContext::ActiveRun)))
    return render_outcome(renderer_.request_render());
  return TuiInputOutcome::Pass;
}

// Restore, nonblocking command, disabled-completion rejection, steering/follow-up
// queue, NewLine/editor/suspend/image/dequeue/jump/submit.
TuiInputOutcome ActiveRunInput::handle_active_command_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;

  if (is_action(active_event, TuiAction::NewLine))
  {
    draft_state_.insert_newline();
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ExternalEditor))
  {
    return render_outcome(action_controller_.open_external_editor());
  }
  if (is_action(active_event, TuiAction::Suspend))
  {
    return render_outcome(action_controller_.suspend_to_background());
  }
  if (is_action(active_event, TuiAction::ClipboardPasteImage))
  {
    return render_outcome(action_controller_.paste_clipboard_image());
  }
  if (is_action(active_event, TuiAction::MessageDequeue))
  {
    return render_outcome(restore_latest_queued_message());
  }
  if (is_action(active_event, TuiAction::JumpForward) || is_action(active_event, TuiAction::JumpBackward))
  {
    draft_state_.begin_jump(is_action(active_event, TuiAction::JumpForward) ? ComposerJumpMode::Forward : ComposerJumpMode::Backward);
    presentation_state_.publish_status(is_action(active_event, TuiAction::JumpForward) ? "jump forward: type character" : "jump backward: type character");
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessageFollowUp))
  {
    renderer_.clear_transcript_selection();
    return render_outcome(queue_active_draft(true));
  }
  if (is_action(active_event, TuiAction::Submit))
  {
    if (auto handled = reject_disabled_visible_completion())
      return render_outcome(*handled);
    if (active_event.key == terminal::Key::Enter && apply_composer_result(draft_state_.convert_backslash_enter_to_newline()))
      return render_outcome(renderer_.request_render());
    renderer_.clear_transcript_selection();
    if (auto handled = run_active_command())
      return render_outcome(*handled);
    return render_outcome(queue_active_draft(false));
  }
  return TuiInputOutcome::Pass;
}

// Composer edit/delete/cursor/undo/redo/yank.
TuiInputOutcome ActiveRunInput::handle_composer_edit(runtime_input::RuntimeInput const& active_input)
{
  using Key = terminal::Key;
  auto const& active_event = active_input.event;
  bool const active_ctrl_d_delete_forward =
      active_event.key == Key::CtrlD && is_action(active_event, TuiAction::DeleteForward) && !draft_state_.draft().text.empty();
  for (auto const action :
       {TuiAction::DeleteBackward, TuiAction::DeleteForward, TuiAction::DeleteWordBackward, TuiAction::DeleteWordForward, TuiAction::DeleteToLineStart,
        TuiAction::DeleteToLineEnd, TuiAction::ClearInput, TuiAction::CursorLeft, TuiAction::CursorRight, TuiAction::CursorLineStart, TuiAction::CursorLineEnd,
        TuiAction::CursorWordLeft, TuiAction::CursorWordRight, TuiAction::Undo, TuiAction::Redo, TuiAction::Yank, TuiAction::YankPop})
  {
    if (!is_action(active_event, action) || (action == TuiAction::DeleteForward && active_event.key == Key::CtrlD && !active_ctrl_d_delete_forward))
      continue;
    static_cast<void>(draft_state_.apply_edit(action, ComposerEditContext::ActiveRun));
    return render_outcome(renderer_.request_render());
  }
  return TuiInputOutcome::Pass;
}

// Transcript/palette/history/vertical navigation in exact original order.
TuiInputOutcome ActiveRunInput::handle_navigation_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto const& snapshot = completion_snapshot();

  if (is_action(active_event, TuiAction::TranscriptHalfPageUp))
  {
    navigation_.scroll_up(navigation_.transcript_page_size());
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::TranscriptHalfPageDown))
  {
    navigation_.scroll_down(navigation_.transcript_page_size());
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PageUp))
  {
    navigation_.scroll_up(navigation_.transcript_page_size());
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PageDown))
  {
    navigation_.scroll_down(navigation_.transcript_page_size());
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessagePrev))
  {
    navigation_.scroll_to_message_boundary(true);
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessageNext))
  {
    navigation_.scroll_to_message_boundary(false);
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::JumpToBottom))
  {
    navigation_.jump_to_bottom("live tail");
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::PalettePrev) &&
      apply_composer_result(draft_state_.step_completion(CompletionStep::Previous, renderer_.completion_cache(), snapshot)))
    return render_outcome(renderer_.request_render());
  if (is_action(active_event, TuiAction::HistoryPrev))
  {
    if (draft_state_.browse_history(true))
    {
      presentation_state_.publish_status("history previous");
    }
    else
    {
      navigation_.scroll_up(kKeyboardScrollRows);
    }
    return render_outcome(renderer_.request_render());
  }
  if (active_event.key == terminal::Key::ArrowUp)
  {
    navigation_.scroll_up(kKeyboardScrollRows);
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CursorUp) && draft_state_.move_vertical(TuiAction::CursorUp))
    return render_outcome(renderer_.request_render());
  return TuiInputOutcome::Pass;
}

// Palette-next/history-next/arrow-down/cursor-down navigation in exact original order.
TuiInputOutcome ActiveRunInput::handle_navigation_next_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto const& snapshot = completion_snapshot();

  if (is_action(active_event, TuiAction::PaletteNext) &&
      apply_composer_result(draft_state_.step_completion(CompletionStep::Next, renderer_.completion_cache(), snapshot)))
    return render_outcome(renderer_.request_render());
  if (is_action(active_event, TuiAction::HistoryNext))
  {
    if (draft_state_.browse_history(false))
    {
      presentation_state_.publish_status(draft_state_.history_index() ? "history next" : "history draft");
    }
    else
    {
      navigation_.scroll_down(kKeyboardScrollRows);
    }
    return render_outcome(renderer_.request_render());
  }
  if (active_event.key == terminal::Key::ArrowDown)
  {
    navigation_.scroll_down(kKeyboardScrollRows);
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::CursorDown) && draft_state_.move_vertical(TuiAction::CursorDown))
    return render_outcome(renderer_.request_render());
  return TuiInputOutcome::Pass;
}

// Mouse hit-testing/wheel.
TuiInputOutcome ActiveRunInput::handle_mouse_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto const& snapshot = presentation_state_.screen();
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
          presentation_state_.publish_status("command disabled: " + *disabled_reason);
          terminal_context_.beep();
        }
        else
        {
          auto selection = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, *clicked);
          draft_state_.accept_completion(std::move(selection.text), selection.cursor, ComposerCompletionOrigin::Pointer);
          presentation_state_.publish_status("command selected - press Enter to queue");
        }
        return render_outcome(renderer_.request_render());
      }
      if (auto const clicked = renderer_.file_reference_completion_at(active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.select_completion(*clicked);
        if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(*clicked))
        {
          presentation_state_.publish_status("reference disabled: " + *disabled_reason);
          terminal_context_.beep();
        }
        else
        {
          auto selection = navigation_.selected_completion_text(*clicked);
          draft_state_.accept_completion(std::move(selection.text), selection.cursor, ComposerCompletionOrigin::Pointer);
          presentation_state_.publish_status("file reference selected");
        }
        return render_outcome(renderer_.request_render());
      }
      if (auto const clicked = renderer_.path_completion_at(active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.select_completion(*clicked);
        if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(*clicked))
        {
          presentation_state_.publish_status("path disabled: " + *disabled_reason);
          terminal_context_.beep();
        }
        else
        {
          auto selection = navigation_.selected_completion_text(*clicked);
          draft_state_.accept_completion(std::move(selection.text), selection.cursor, ComposerCompletionOrigin::PointerPath);
          presentation_state_.publish_status("path selected");
        }
        return render_outcome(renderer_.request_render());
      }
    }

    auto const transcript_mouse =
        renderer_.handle_transcript_selection_mouse(active_event, std::bind_front(&RuntimeNavigationController::toggle_tool_details_at, &navigation_),
                                                    std::bind_front(&RuntimeNavigationController::toggle_thinking_at, &navigation_));
    if (transcript_mouse != TranscriptSelectionMouseResult::Ignored)
      return render_outcome(renderer_.request_render());

    if (begins_click)
    {
      if (auto const cursor = composer_input_cursor_for_screen_position(snapshot, active_event.mouse_row, active_event.mouse_column))
      {
        renderer_.clear_transcript_selection();
        draft_state_.begin_pointer_selection(*cursor, active_event.key == Key::MouseLeftPress, ComposerEditContext::ActiveRun);
        presentation_state_.publish_status("cursor moved");
        return render_outcome(renderer_.request_render());
      }
    }
    else if (draft_state_.mouse_selecting())
    {
      if (auto const cursor = composer_input_cursor_for_screen_position(snapshot, active_event.mouse_row, active_event.mouse_column))
      {
        draft_state_.extend_pointer_selection(*cursor, ComposerEditContext::ActiveRun);
        presentation_state_.publish_status(draft_state_.selection_bounds() ? "selection active" : "cursor moved");
      }
      if (active_event.key == Key::MouseLeftRelease)
        draft_state_.end_pointer_selection();
      return render_outcome(renderer_.request_render());
    }
  }
  if (active_event.key == Key::MouseWheelUp)
  {
    navigation_.scroll_up(kTranscriptWheelScrollRows);
    return render_outcome(renderer_.request_render());
  }
  if (active_event.key == Key::MouseWheelDown)
  {
    navigation_.scroll_down(kTranscriptWheelScrollRows);
    return render_outcome(renderer_.request_render());
  }
  return TuiInputOutcome::Pass;
}

// Active-run restricted toggles/model actions and Space fallback.
TuiInputOutcome ActiveRunInput::handle_restricted_toggle_input(runtime_input::RuntimeInput const& active_input)
{
  auto const& active_event = active_input.event;
  auto const& snapshot = presentation_state_.screen();

  if (is_action(active_event, TuiAction::DetailsToggle))
  {
    renderer_.synchronize_detached_transcript_layout();
    presentation_state_.publish_tool_presentation(snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich
                                                                                                           : ToolPresentation::Expanded);
    presentation_state_.publish_status("tool details " + std::string(to_string(snapshot.tool_presentation)));
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::OverviewToggle))
  {
    return render_outcome(action_controller_.toggle_startup_overview());
  }
  if (is_action(active_event, TuiAction::VariantCycle))
  {
    presentation_state_.publish_status("reasoning can be changed between turns");
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ReasoningSelect))
  {
    presentation_state_.publish_status("thinking mode can be changed between turns");
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ThinkingToggle))
  {
    action_controller_.toggle_thinking_visibility();
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ModelSelect))
  {
    presentation_state_.publish_status("model selection is available between turns");
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::ModelCycleForward) || is_action(active_event, TuiAction::ModelCycleBackward))
  {
    presentation_state_.publish_status("model cycling is available between turns");
    return render_outcome(renderer_.request_render());
  }
  if (is_action(active_event, TuiAction::MessageDequeue))
  {
    presentation_state_.publish_status("queued-message restore is available during active runs");
    return render_outcome(renderer_.request_render());
  }
  if (active_event.key == terminal::Key::Space)
  {
    presentation_state_.clear_local_feedback();
    static_cast<void>(apply_composer_result(draft_state_.insert_input(active_input, ComposerEditContext::ActiveRun)));
    return render_outcome(renderer_.request_render());
  }
  return TuiInputOutcome::Pass;
}

} // namespace ava::tui
