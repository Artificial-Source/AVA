#include "sys.h"
#include "ava/tui/command_output.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_active_run_internal.h"
#include "ava/tui/runtime_commands_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_submit_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/tui/tool_cards.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <curses.h>

namespace ava::tui {
using runtime_commands::attach_command_argument;
using runtime_commands::copy_command_argument;
using runtime_commands::diff_command_argument;
using runtime_commands::exact_command;
using runtime_commands::fork_from_command_argument;
using runtime_commands::parse_copy_target;
using runtime_commands::reload_command_argument;
using runtime_commands::reload_target_from_argument;
using runtime_commands::ReloadTarget;
using runtime_commands::search_command_argument;
using runtime_commands::stash_command_argument;
using runtime_commands::tool_command_argument;
using runtime_transcript::copy_text_to_terminal_clipboard;
using runtime_transcript::diff_transcript_text;
using runtime_transcript::push_history;
using runtime_views::compact_path_leaf;

RuntimeSubmitController::RuntimeSubmitController(TuiRuntimeOptions& options, RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state,
                                                 RuntimeRenderer& renderer, RuntimeNavigationController& navigation, RuntimeActionController& action_controller,
                                                 RuntimeActiveRunController& active_run_controller, RuntimePromptStashController& prompt_stash,
                                                 TranscriptSearchController& transcript_search, RuntimeSubagentWorkspaceController& subagent_workspace,
                                                 ActiveSelectList& active_select_list)
    : options_(options),
      presentation_state_(presentation_state),
      draft_state_(draft_state),
      renderer_(renderer),
      navigation_(navigation),
      action_controller_(action_controller),
      active_run_controller_(active_run_controller),
      prompt_stash_(prompt_stash),
      transcript_search_(transcript_search),
      subagent_workspace_(subagent_workspace),
      active_select_list_(active_select_list)
{
}

RuntimeSubmitOutcome RuntimeSubmitController::submit(std::optional<std::string> forced_submission)
{
  auto& snapshot = presentation_state_.snapshot;
  auto& sidebar = presentation_state_.sidebar;
  auto& input_history = draft_state_.input_history;
  auto& history_index = draft_state_.history_index;
  auto& draft_input = draft_state_.draft_input;
  auto& draft = draft_state_.draft;
  auto& jump_mode = draft_state_.jump_mode;
  auto& selected_slash_command_index = draft_state_.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state_.path_completion_force_active;
  auto& draft_scroll_offset = draft_state_.draft_scroll_offset;
  auto& pending_escape_clear = draft_state_.pending_escape_clear;
  auto& transcript_scroll_offset = renderer_.transcript_scroll_offset;
  auto& detached_new_output_count = renderer_.detached_new_output_count;
  auto& detached_sidebar_snapshot = renderer_.detached_sidebar_snapshot;
  pending_escape_clear = false;
  std::optional<std::string> immediate_slash_submission;
  if (forced_submission)
  {
    immediate_slash_submission = std::move(forced_submission);
  }
  else if (((exact_command(draft.text, "/models") || exact_command(draft.text, "/model")) && options_.model_selector_view) ||
           (exact_command(draft.text, "/scoped-models") && options_.scoped_model_selector_view) ||
           ((exact_command(draft.text, "/sessions") || exact_command(draft.text, "/tree") || exact_command(draft.text, "/resume")) &&
            options_.session_selector_view) ||
           exact_command(draft.text, "/jobs") || exact_command(draft.text, "/overview") || exact_command(draft.text, "/stash"))
  {
    immediate_slash_submission = expanded_composer_draft_text(draft);
  }
  else if (!slash_palette_suppressed && slash_palette_visible(draft.text, draft.cursor, snapshot.slash_commands))
  {
    auto const matches = filter_slash_commands(draft.text, draft.cursor, snapshot.slash_commands);
    if (!matches.empty())
    {
      selected_slash_command_index = clamp_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
      if (auto const disabled_reason = slash_command_selection_disabled_reason(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index))
      {
        snapshot.status = "command disabled: " + *disabled_reason;
        static_cast<void>(beep());
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      auto const& selected_item = matches[selected_slash_command_index];
      auto const selection_text = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, selected_slash_command_index);
      if (!selected_item.argument_completion && selected_item.command == "/connect")
      {
        immediate_slash_submission = selection_text.text;
      }
      else
      {
        draft_state_.clear_selection();
        static_cast<void>(replace_composer_draft(draft, std::move(selection_text.text), selection_text.cursor));
        selected_slash_command_index = 0;
        path_completion_force_active = false;
        draft_scroll_offset = 0;
        history_index.reset();
        draft_input.clear();
        snapshot.status = "command selected - press Enter to run";
        snapshot.selected_slash_command_index = selected_slash_command_index;
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      selected_slash_command_index = 0;
      path_completion_force_active = false;
      snapshot.selected_slash_command_index = selected_slash_command_index;
    }
  }
  if (navigation_.file_reference_palette_active())
  {
    selected_slash_command_index = navigation_.clamp_completion(selected_slash_command_index);
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(selected_slash_command_index))
    {
      snapshot.status = "reference disabled: " + *disabled_reason;
      static_cast<void>(beep());
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    auto selection = navigation_.selected_completion_text(selected_slash_command_index);
    draft_state_.clear_selection();
    static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
    selected_slash_command_index = 0;
    path_completion_force_active = false;
    draft_scroll_offset = 0;
    history_index.reset();
    draft_input.clear();
    snapshot.status = "file reference selected";
    snapshot.selected_slash_command_index = selected_slash_command_index;
    if (!renderer_.render())
    {
      return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
    }
    return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
  }
  if (navigation_.path_completion_palette_active())
  {
    selected_slash_command_index = navigation_.clamp_completion(selected_slash_command_index);
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(selected_slash_command_index))
    {
      snapshot.status = "path disabled: " + *disabled_reason;
      static_cast<void>(beep());
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    auto selection = navigation_.selected_completion_text(selected_slash_command_index);
    draft_state_.clear_selection();
    static_cast<void>(replace_composer_draft(draft, std::move(selection.text), selection.cursor));
    selected_slash_command_index = 0;
    path_completion_force_active = false;
    draft_scroll_offset = 0;
    history_index.reset();
    draft_input.clear();
    snapshot.status = "path selected";
    snapshot.selected_slash_command_index = selected_slash_command_index;
    if (!renderer_.render())
    {
      return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
    }
    return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
  }
  auto submitted = immediate_slash_submission ? *immediate_slash_submission : expanded_composer_draft_text(draft);
  renderer_.clear_transcript_selection();
  draft_state_.clear_selection();
  reset_composer_draft(draft);
  jump_mode = ComposerJumpMode::None;
  draft_scroll_offset = 0;
  history_index.reset();
  draft_input.clear();
  selected_slash_command_index = 0;
  path_completion_force_active = false;
  if (!submitted.empty())
  {
    if (auto stash_argument = stash_command_argument(submitted))
    {
      push_history(input_history, submitted);
      bool success = true;
      if (stash_argument->empty())
        success = prompt_stash_.open_selector();
      else if (*stash_argument == "pop")
        success = prompt_stash_.pop_latest();
      else if (*stash_argument == "clear")
        success = prompt_stash_.clear();
      else
      {
        snapshot.status = "invalid_argument: usage: /stash [pop|clear]";
        static_cast<void>(beep());
        success = renderer_.request_render();
      }
      //FIXME: how is `success` related to terminal_write_failed ?!
      return {.disposition = success ? RuntimeSubmitDisposition::ContinueLoop : RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = !success};
    }
    if (exact_command(submitted, "/jobs"))
    {
      push_history(input_history, submitted);
      static_cast<void>(subagent_workspace_.open_selector());
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if ((exact_command(submitted, "/models") || exact_command(submitted, "/model")) && options_.model_selector_view)
    {
      push_history(input_history, submitted);
      if (!action_controller_.open_model_selector())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (exact_command(submitted, "/scoped-models") && options_.scoped_model_selector_view)
    {
      push_history(input_history, submitted);
      if (!action_controller_.open_scoped_model_selector())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if ((exact_command(submitted, "/sessions") || exact_command(submitted, "/tree") || exact_command(submitted, "/resume")) && options_.session_selector_view)
    {
      push_history(input_history, submitted);
      if (!action_controller_.open_session_selector())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto fork_from_query = fork_from_command_argument(submitted); fork_from_query && options_.on_open_fork_user_turn_selector)
    {
      push_history(input_history, submitted);
      if (!action_controller_.open_fork_user_turn_selector(*fork_from_query))
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto search_query = search_command_argument(submitted))
    {
      push_history(input_history, submitted);
      static_cast<void>(transcript_search_.open(std::move(*search_query)));
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto reload_target = reload_command_argument(submitted))
    {
      push_history(input_history, submitted);
      auto const parsed_reload_target = reload_target_from_argument(*reload_target);
      if (!parsed_reload_target)
      {
        open_command_error(snapshot, submitted,
                           "invalid_argument: unsupported reload target\n  target: " + *reload_target + "\n  supported: keybindings, theme");
        static_cast<void>(beep());
      }
      else if (*parsed_reload_target == ReloadTarget::DisplaySettings)
      {
        if (!options_.on_reload_display_settings)
        {
          open_command_error(snapshot, submitted, "display reload unavailable");
          static_cast<void>(beep());
        }
        else if (auto reloaded = options_.on_reload_display_settings())
        {
          auto status = reloaded->status.empty() ? std::string("display theme reloaded") : reloaded->status;
          presentation_state_.apply_runtime_state_snapshot(options_, std::move(*reloaded));
          settle_local_command_status(snapshot, std::move(status));
        }
        else
        {
          open_command_error(snapshot, submitted, reloaded.error().format());
          static_cast<void>(beep());
        }
      }
      else if (!options_.on_reload_key_bindings)
      {
        open_command_error(snapshot, submitted, "reload unavailable");
        static_cast<void>(beep());
      }
      else if (auto reloaded = options_.on_reload_key_bindings())
      {
        options_.key_bindings = std::move(reloaded->key_bindings);
        presentation_state_.apply_runtime_state_snapshot(options_, std::move(reloaded->state));
        settle_local_command_status(snapshot, "keybindings reloaded");
      }
      else
      {
        open_command_error(snapshot, submitted, reloaded.error().format());
        static_cast<void>(beep());
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/hotkeys" || submitted == "/keybindings")
    {
      push_history(input_history, submitted);
      snapshot.select_list = hotkeys_select_list_view(options_.key_bindings);
      active_select_list_ = ActiveSelectList::Hotkeys;
      snapshot.status = "keybindings opened";
      transcript_scroll_offset = 0;
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/sidebar")
    {
      push_history(input_history, submitted);
      snapshot.sidebar_drawer_visible = true;
      snapshot.sidebar_drawer_scroll_offset = 0;
      transcript_scroll_offset = 0;
      detached_new_output_count = 0;
      detached_sidebar_snapshot.reset();
      snapshot.status = "session overview opened";
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/settings")
    {
      push_history(input_history, submitted);
      presentation_state_.refresh_token_status(options_);
      presentation_state_.refresh_active_context_status(options_);
      presentation_state_.refresh_reasoning_status(options_);
      auto settings_snapshot = snapshot;
      settings_snapshot.sidebar = sidebar;
      snapshot.select_list = settings_select_list_view(settings_snapshot, options_.key_bindings);
      active_select_list_ = ActiveSelectList::Settings;
      snapshot.status = "settings opened";
      transcript_scroll_offset = 0;
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (exact_command(submitted, "/overview"))
    {
      push_history(input_history, submitted);
      if (!action_controller_.toggle_startup_overview())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto attach_target = attach_command_argument(submitted))
    {
      push_history(input_history, submitted);
      if (attach_target->empty())
      {
        open_command_error(snapshot, submitted, "invalid_argument: usage: /attach <image-path>");
        static_cast<void>(beep());
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      if (!options_.on_attach_image)
      {
        open_command_error(snapshot, submitted, "image attachment import unavailable");
        static_cast<void>(beep());
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      auto imported = options_.on_attach_image(*attach_target);
      if (!imported)
      {
        open_command_error(snapshot, submitted, imported.error().format());
        static_cast<void>(beep());
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      auto label = compact_path_leaf(*attach_target);
      if (!action_controller_.queue_pending_image_attachment(*imported, std::move(label), "attached image for next prompt"))
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto tool_query = tool_command_argument(submitted))
    {
      push_history(input_history, submitted);
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
        open_command_output(snapshot, submitted, {}, {indexed_tool->tool});
      }
      else
      {
        auto const status = tool_query->empty() ? std::string("no tool details to show") : std::string("no matching tool details to show");
        open_command_error(snapshot, submitted, status);
        static_cast<void>(beep());
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto diff_query = diff_command_argument(submitted))
    {
      push_history(input_history, submitted);
      auto diff_text = latest_indexed_tool_diff_copy_text(snapshot, *diff_query);
      if (diff_text)
      {
        auto const title = diff_query->empty() ? std::string_view("Latest tool diff:") : std::string_view("Matching tool diff:");
        open_command_output(snapshot, submitted, {diff_transcript_text(title, *diff_text)});
      }
      else
      {
        open_command_error(snapshot, submitted, diff_query->empty() ? "no tool diff to show" : "no matching tool diff to show");
        static_cast<void>(beep());
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto copy_target = copy_command_argument(submitted))
    {
      push_history(input_history, submitted);
      auto const target = parse_copy_target(*copy_target);
      // Exact target name "user" opens the public user-turn picker. No alias is
      // accepted; remaining text becomes the selector's initial filter query.
      if (target.name == "user" && options_.on_open_copy_user_turn_selector)
      {
        if (!action_controller_.open_copy_user_turn_selector(target.query))
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      std::optional<std::string> copy_text;
      std::string copied_status;
      std::string missing_status;
      bool valid_target = true;
      bool copied = false;
      bool handled_latest_assistant = false;
      if (target.name.empty() || target.name == "last")
      {
        auto const result = runtime_transcript::copy_latest_assistant_message(snapshot.transcript);
        copied = result == runtime_transcript::LatestAssistantCopyResult::RequestSent;
        snapshot.status = runtime_transcript::latest_assistant_copy_status(result);
        handled_latest_assistant = true;
      }
      else if (target.name == "tool" || target.name == "tools")
      {
        copy_text = latest_indexed_tool_copy_text(snapshot, target.query);
        copied_status = target.query.empty() ? "latest tool details copy request sent" : "matching tool details copy request sent";
        missing_status = target.query.empty() ? "no tool details to copy" : "no matching tool details to copy";
      }
      else if (target.name == "diff" || target.name == "diffs")
      {
        copy_text = latest_indexed_tool_diff_copy_text(snapshot, target.query);
        copied_status = target.query.empty() ? "latest tool diff copy request sent" : "matching tool diff copy request sent";
        missing_status = target.query.empty() ? "no tool diff to copy" : "no matching tool diff to copy";
      }
      else if (target.name == "permission" || target.name == "permissions")
      {
        copy_text = latest_indexed_permission_copy_text(snapshot, target.query);
        copied_status = target.query.empty() ? "latest permission details copy request sent" : "matching permission details copy request sent";
        missing_status = target.query.empty() ? "no permission details to copy" : "no matching permission details to copy";
      }
      else if (target.name == "user")
      {
        valid_target = false;
        snapshot.status = "copy user-turn selector unavailable";
      }
      else
      {
        valid_target = false;
        snapshot.status = "invalid_argument: unsupported copy target\n  target: " + target.name +
                          "\n  supported: user [query], tool [query], diff [query], permission [query]";
      }

      if (valid_target && !handled_latest_assistant)
      {
        copied = copy_text && copy_text_to_terminal_clipboard(*copy_text);
        if (copied)
        {
          snapshot.status = std::move(copied_status);
        }
        else
        {
          snapshot.status = copy_text ? "clipboard copy failed" : std::move(missing_status);
        }
      }
      if (copied)
      {
        settle_local_command_status(snapshot, snapshot.status);
      }
      else
      {
        open_command_error(snapshot, submitted, snapshot.status);
        static_cast<void>(beep());
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/details" || submitted == "/details compact" || submitted == "/details rich" || submitted == "/details expanded")
    {
      push_history(input_history, submitted);
      if (submitted == "/details compact")
        snapshot.tool_presentation = ToolPresentation::Compact;
      else if (submitted == "/details rich")
        snapshot.tool_presentation = ToolPresentation::Rich;
      else if (submitted == "/details expanded")
        snapshot.tool_presentation = ToolPresentation::Expanded;
      else
        snapshot.tool_presentation = snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich : ToolPresentation::Expanded;
      settle_local_command_status(snapshot, "tool details " + std::string(to_string(snapshot.tool_presentation)));
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/thinking" || submitted == "/thinking details")
    {
      push_history(input_history, submitted);
      if (submitted == "/thinking details")
      {
        auto const item_index = navigation_.toggle_latest_thinking_details();
        if (item_index && *item_index < snapshot.transcript.size())
        {
          settle_local_command_status(
              snapshot, snapshot.transcript[*item_index].thinking_expanded ? "latest thinking details expanded" : "latest thinking details collapsed");
        }
        else
        {
          snapshot.status = "no completed long thinking block to expand";
          static_cast<void>(beep());
        }
      }
      else
      {
        action_controller_.toggle_thinking_visibility();
        settle_local_command_status(snapshot, snapshot.status);
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    RuntimeActiveRunOutcome const outcome = active_run_controller_.run(std::move(submitted));
    return {.disposition = outcome.break_loop ? RuntimeSubmitDisposition::BreakLoop : RuntimeSubmitDisposition::ContinueLoop,
            .terminal_write_failed = outcome.terminal_write_failed, .terminal_signal_received = outcome.terminal_signal_received};
  }
  return {};
}

}  // namespace ava::tui
