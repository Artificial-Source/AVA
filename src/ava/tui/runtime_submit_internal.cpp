#include "sys.h"
#include "SelectorHost.h"
#include "SessionSelector.h"
#include "SettingsDialog.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/command_output.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_commands_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_submit_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/tui/tool_cards.h"
#include "ava/core/Application.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
using runtime_views::compact_path_leaf;

RuntimeSubmitController::RuntimeSubmitController(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state,
                                                 RuntimeRenderer& renderer, RuntimeNavigationController& navigation, RuntimeActionController& action_controller,
                                                 ActiveRun& active_run_controller, RuntimePromptStashController& prompt_stash,
                                                 TranscriptSearchController& transcript_search, RuntimeSubagentWorkspaceController& subagent_workspace,
                                                 SelectorHost& selectors, SettingsDialog& settings_dialog, SessionSelector& session_selector)
    : frontend_(frontend),
      presentation_state_(presentation_state),
      draft_state_(draft_state),
      renderer_(renderer),
      navigation_(navigation),
      action_controller_(action_controller),
      active_run_controller_(active_run_controller),
      prompt_stash_(prompt_stash),
      transcript_search_(transcript_search),
      subagent_workspace_(subagent_workspace),
      selectors_(selectors),
      settings_dialog_(settings_dialog),
      session_selector_(session_selector)
{
}

RuntimeSubmitOutcome RuntimeSubmitController::submit(std::optional<std::string> forced_submission)
{
  auto const& snapshot = presentation_state_.screen();
  auto const& draft = draft_state_.draft();
  draft_state_.cancel_escape();
  std::optional<std::string> immediate_slash_submission;
  if (forced_submission)
  {
    immediate_slash_submission = std::move(forced_submission);
  }
  else if ((exact_command(draft.text, "/models") || exact_command(draft.text, "/model") || exact_command(draft.text, "/scoped-models") ||
            exact_command(draft.text, "/sessions") || exact_command(draft.text, "/tree") || exact_command(draft.text, "/resume") ||
            exact_command(draft.text, "/jobs") || exact_command(draft.text, "/overview") || exact_command(draft.text, "/stash")))
  {
    immediate_slash_submission = expanded_composer_draft_text(draft);
  }
  else if (!draft_state_.palette_suppressed() && slash_palette_visible(draft.text, draft.cursor, snapshot.slash_commands))
  {
    auto const matches = filter_slash_commands(draft.text, draft.cursor, snapshot.slash_commands);
    if (!matches.empty())
    {
      draft_state_.select_completion(clamp_slash_palette_selection(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index()));
      if (auto const disabled_reason =
              slash_command_selection_disabled_reason(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index()))
      {
        presentation_state_.publish_status("command disabled: " + *disabled_reason);
        ava::core::Application::instance().terminal_context().beep();
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      auto const& selected_item = matches[draft_state_.completion_index()];
      auto const selection_text = slash_command_selection_text(draft.text, draft.cursor, snapshot.slash_commands, draft_state_.completion_index());
      if (!selected_item.argument_completion && selected_item.command == "/connect")
      {
        immediate_slash_submission = selection_text.text;
      }
      else
      {
        draft_state_.accept_completion(std::move(selection_text.text), selection_text.cursor);
        presentation_state_.publish_status("command selected - press Enter to run");
        presentation_state_.publish_completion(draft_state_);
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      draft_state_.reset_completion();
      presentation_state_.publish_completion(draft_state_);
    }
  }
  if (navigation_.file_reference_palette_active())
  {
    draft_state_.select_completion(navigation_.clamp_completion(draft_state_.completion_index()));
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(draft_state_.completion_index()))
    {
      presentation_state_.publish_status("reference disabled: " + *disabled_reason);
      ava::core::Application::instance().terminal_context().beep();
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    auto selection = navigation_.selected_completion_text(draft_state_.completion_index());
    draft_state_.accept_completion(std::move(selection.text), selection.cursor);
    presentation_state_.publish_status("file reference selected");
    presentation_state_.publish_completion(draft_state_);
    if (!renderer_.render())
    {
      return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
    }
    return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
  }
  if (navigation_.path_completion_palette_active())
  {
    draft_state_.select_completion(navigation_.clamp_completion(draft_state_.completion_index()));
    if (auto const disabled_reason = navigation_.selected_completion_disabled_reason(draft_state_.completion_index()))
    {
      presentation_state_.publish_status("path disabled: " + *disabled_reason);
      ava::core::Application::instance().terminal_context().beep();
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    auto selection = navigation_.selected_completion_text(draft_state_.completion_index());
    draft_state_.accept_completion(std::move(selection.text), selection.cursor);
    presentation_state_.publish_status("path selected");
    presentation_state_.publish_completion(draft_state_);
    if (!renderer_.render())
    {
      return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
    }
    return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
  }
  auto submitted = immediate_slash_submission ? *immediate_slash_submission : expanded_composer_draft_text(draft);
  renderer_.clear_transcript_selection();
  draft_state_.reset_after_submit();
  if (!submitted.empty())
  {
    if (auto stash_argument = stash_command_argument(submitted))
    {
      draft_state_.record_submission(submitted);
      bool success = true;
      if (stash_argument->empty())
        success = prompt_stash_.open_selector();
      else if (*stash_argument == "pop")
        success = prompt_stash_.pop_latest();
      else if (*stash_argument == "clear")
        success = prompt_stash_.clear();
      else
      {
        presentation_state_.publish_status("invalid_argument: usage: /stash [pop|clear]");
        ava::core::Application::instance().terminal_context().beep();
        success = renderer_.request_render();
      }
      //FIXME: how is `success` related to terminal_write_failed ?!
      return {.disposition = success ? RuntimeSubmitDisposition::ContinueLoop : RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = !success};
    }
    if (exact_command(submitted, "/jobs"))
    {
      draft_state_.record_submission(submitted);
      static_cast<void>(subagent_workspace_.open_selector());
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (exact_command(submitted, "/models") || exact_command(submitted, "/model"))
    {
      draft_state_.record_submission(submitted);
      if (!action_controller_.open_model_selector())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (exact_command(submitted, "/scoped-models"))
    {
      draft_state_.record_submission(submitted);
      if (!action_controller_.open_scoped_model_selector())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (exact_command(submitted, "/sessions") || exact_command(submitted, "/tree") || exact_command(submitted, "/resume"))
    {
      draft_state_.record_submission(submitted);
      if (!session_selector_.open())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto fork_from_query = fork_from_command_argument(submitted))
    {
      draft_state_.record_submission(submitted);
      if (!action_controller_.open_fork_user_turn_selector(*fork_from_query))
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto search_query = search_command_argument(submitted))
    {
      draft_state_.record_submission(submitted);
      static_cast<void>(transcript_search_.open(std::move(*search_query)));
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto reload_target = reload_command_argument(submitted))
    {
      // Only theme/display and keybinding aliases stay on the TUI hot-reload path.
      // Bare /reload remains local keybindings; /reload all and every other or invalid
      // target fall through so command_reload's canonical validation and reports win.
      if (auto const parsed_reload_target = reload_target_from_argument(*reload_target))
      {
        draft_state_.record_submission(submitted);
        if (*parsed_reload_target == ReloadTarget::DisplaySettings)
        {
          if (auto reloaded = frontend_.on_reload_display_settings())
          {
            auto status = reloaded->status.empty() ? std::string("display theme reloaded") : reloaded->status;
            presentation_state_.apply_local_display_settings(frontend_, std::move(reloaded->settings));
            presentation_state_.publish_local_status(std::move(status));
          }
          else
          {
            presentation_state_.publish_local_error(submitted, reloaded.error().format());
            ava::core::Application::instance().terminal_context().beep();
          }
        }
        else if (auto reloaded = frontend_.on_reload_key_bindings())
        {
          presentation_state_.apply_local_key_bindings(frontend_, std::move(*reloaded));
          presentation_state_.publish_local_status("keybindings reloaded");
        }
        else
        {
          presentation_state_.publish_local_error(submitted, reloaded.error().format());
          ava::core::Application::instance().terminal_context().beep();
        }
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
    }
    if (runtime_commands::local_display_settings_command(submitted))
    {
      draft_state_.record_submission(submitted);
      auto applied = frontend_.on_local_display_settings_action(submitted);
      if (!applied)
      {
        presentation_state_.publish_local_error(submitted, applied.error().format());
        ava::core::Application::instance().terminal_context().beep();
      }
      else
      {
        presentation_state_.apply_local_display_settings(frontend_, std::move(applied->settings));
        presentation_state_.publish_local_completion(submitted, std::move(applied->output));
      }
      if (!renderer_.render())
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/hotkeys" || submitted == "/keybindings")
    {
      draft_state_.record_submission(submitted);
      selectors_.open(ActiveSelectList::Hotkeys, hotkeys_select_list_view(frontend_.key_bindings()));
      presentation_state_.publish_status("keybindings opened");
      renderer_.show_live_tail(LiveTailReason::Requested);
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/sidebar")
    {
      draft_state_.record_submission(submitted);
      presentation_state_.publish_sidebar_drawer(true, 0);
      renderer_.show_live_tail(LiveTailReason::LocalCommand);
      presentation_state_.publish_status("session overview opened");
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/settings")
    {
      draft_state_.record_submission(submitted);
      settings_dialog_.open();
      renderer_.show_live_tail(LiveTailReason::Requested);
      if (!renderer_.request_render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (exact_command(submitted, "/overview"))
    {
      draft_state_.record_submission(submitted);
      if (!action_controller_.toggle_startup_overview())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto attach_target = attach_command_argument(submitted))
    {
      draft_state_.record_submission(submitted);
      if (attach_target->empty())
      {
        presentation_state_.publish_local_error(submitted, "invalid_argument: usage: /attach <image-path>");
        ava::core::Application::instance().terminal_context().beep();
        if (!renderer_.render())
        {
          return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
        }
        return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
      }
      auto imported = frontend_.on_attach_image(*attach_target);
      if (!imported)
      {
        presentation_state_.publish_local_error(submitted, imported.error().format());
        ava::core::Application::instance().terminal_context().beep();
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
      draft_state_.record_submission(submitted);
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
        presentation_state_.publish_local_output(submitted, {}, {indexed_tool->tool});
      }
      else
      {
        auto const status = tool_query->empty() ? std::string("no tool details to show") : std::string("no matching tool details to show");
        presentation_state_.publish_local_error(submitted, status);
        ava::core::Application::instance().terminal_context().beep();
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto diff_query = diff_command_argument(submitted))
    {
      draft_state_.record_submission(submitted);
      auto diff_text = latest_indexed_tool_diff_copy_text(snapshot, *diff_query);
      if (diff_text)
      {
        auto const title = diff_query->empty() ? std::string_view("Latest tool diff:") : std::string_view("Matching tool diff:");
        presentation_state_.publish_local_output(submitted, {diff_transcript_text(title, *diff_text)});
      }
      else
      {
        presentation_state_.publish_local_error(submitted, diff_query->empty() ? "no tool diff to show" : "no matching tool diff to show");
        ava::core::Application::instance().terminal_context().beep();
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (auto copy_target = copy_command_argument(submitted))
    {
      draft_state_.record_submission(submitted);
      auto const target = parse_copy_target(*copy_target);
      // Exact target name "user" opens the public user-turn picker. No alias is
      // accepted; remaining text becomes the selector's initial filter query.
      if (target.name == "user")
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
        presentation_state_.publish_status(runtime_transcript::latest_assistant_copy_status(result));
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
        presentation_state_.publish_status("copy user-turn selector unavailable");
      }
      else
      {
        valid_target = false;
        presentation_state_.publish_status("invalid_argument: unsupported copy target\n  target: " + target.name +
                                           "\n  supported: user [query], tool [query], diff [query], permission [query]");
      }

      if (valid_target && !handled_latest_assistant)
      {
        copied = copy_text && copy_text_to_terminal_clipboard(*copy_text);
        if (copied)
        {
          presentation_state_.publish_status(std::move(copied_status));
        }
        else
        {
          presentation_state_.publish_status(copy_text ? "clipboard copy failed" : std::move(missing_status));
        }
      }
      if (copied)
      {
        presentation_state_.publish_local_status(snapshot.status);
      }
      else
      {
        presentation_state_.publish_local_error(submitted, snapshot.status);
        ava::core::Application::instance().terminal_context().beep();
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/details" || submitted == "/details compact" || submitted == "/details rich" || submitted == "/details expanded")
    {
      draft_state_.record_submission(submitted);
      if (submitted == "/details compact")
        presentation_state_.publish_tool_presentation(ToolPresentation::Compact);
      else if (submitted == "/details rich")
        presentation_state_.publish_tool_presentation(ToolPresentation::Rich);
      else if (submitted == "/details expanded")
        presentation_state_.publish_tool_presentation(ToolPresentation::Expanded);
      else
        presentation_state_.publish_tool_presentation(snapshot.tool_presentation == ToolPresentation::Expanded ? ToolPresentation::Rich
                                                                                                               : ToolPresentation::Expanded);
      presentation_state_.publish_local_status("tool details " + std::string(to_string(snapshot.tool_presentation)));
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (submitted == "/thinking" || submitted == "/thinking details")
    {
      draft_state_.record_submission(submitted);
      if (submitted == "/thinking details")
      {
        auto const item_index = navigation_.toggle_latest_thinking_details();
        if (item_index && *item_index < snapshot.transcript.size())
        {
          presentation_state_.publish_local_status(snapshot.transcript[*item_index].thinking_expanded ? "latest thinking details expanded"
                                                                                                      : "latest thinking details collapsed");
        }
        else
        {
          presentation_state_.publish_status("no completed long thinking block to expand");
          ava::core::Application::instance().terminal_context().beep();
        }
      }
      else
      {
        action_controller_.toggle_thinking_visibility();
        presentation_state_.publish_local_status(snapshot.status);
      }
      if (!renderer_.render())
      {
        return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
      }
      return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
    }
    if (!active_run_controller_.start(std::move(submitted)))
      return {.disposition = RuntimeSubmitDisposition::BreakLoop, .terminal_write_failed = true};
    return {.disposition = RuntimeSubmitDisposition::ContinueLoop};
  }
  return {};
}

}  // namespace ava::tui
