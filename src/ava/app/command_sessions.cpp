#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_session_support_internal.h"
#include "ava/app/command_sessions.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/session/session_branch.h"
#include "ava/session/session_metadata.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app {
using session_command_support::labels_text;
using session_command_support::reopen_session;
using session_command_support::trim_ascii;

namespace {

ava::core::Result<runtime::session_ts> create_fresh_session(runtime::session_ts const& unlocked_current)
{
  runtime::OpenContext context;
  runtime::SessionLifecycleRequest request;
  {
    SCOPED_CRITICAL_AREA_CR(current_r, unlocked_current);
    context = current_r->replacement_open_context({});
    request.sessionless = current_r->sessionless();
  }
  return runtime::Session::open(context, request);
}

ava::core::Result<CommandResult> run_branch_command(runtime::session_ts& unlocked_session, std::string_view name,
                                                     ava::session::SessionBranchMode mode,
                                                     std::string_view branch_from_entry_id = {})
{
  CommandResult result;
  result.handled = true;

  auto const trimmed_name = trim_ascii(name);
  std::string source_session_id;
  runtime::OpenContext owned_options;
  ava::core::Result<ava::session::SessionBranchResult> branched =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session branch was not attempted"));
  {
    // Branch creation receives a pointer to the source lease, so this critical area must span the
    // synchronous branch snapshot. Opening and installing the resulting session happen outside it.
    SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
    source_session_id = session_w->store.options_r()->session_id;
    if (session_w->sessionless())
    {
      add_output(result, "Cannot branch a sessionless session.");
      return result;
    }
    auto recovered = session_w->store.recover_torn_tail(session_w->lease(), session_w->session_read_limits());
    if (!recovered)
      return std::unexpected(std::move(recovered.error()));
    auto staged_recovery =
        session_w->store.recover_incomplete_assistant_output_suffix(session_w->lease(), session_w->session_read_limits());
    if (!staged_recovery)
      return std::unexpected(std::move(staged_recovery.error()));
    branched = ava::session::create_session_branch(
        ava::session::SessionBranchOptions{.workspace_dir = session_w->workspace_dir(),
                                           .root_dir = session_w->paths().sessions_dir,
                                           .source_session_id = source_session_id,
                                           .branch_from_entry_id = std::string(branch_from_entry_id),
                                           .name = trimmed_name.empty() ? std::nullopt : std::optional<std::string>(trimmed_name),
                                           .labels = std::nullopt,
                                           .read_limits = session_w->session_read_limits(),
                                           .source_lease = &session_w->lease(),
                                           .mode = mode,
                                           .actor = "tui"});
    owned_options = session_w->replacement_open_context({});
  }
  if (!branched)
  {
    return std::unexpected(std::move(branched.error()));
  }

  auto const created_session_id = branched->store.options_r()->session_id;
  auto const resolved_branch_from_entry_id = branched->branch_from_entry_id;
  auto unlocked_opened_result = runtime::Session::open_owned(owned_options, branched->store, branched->lease, true);
  if (!unlocked_opened_result)
  {
    auto error = std::move(unlocked_opened_result.error());
    ava::session::rollback_created_session_with_context(branched->store, branched->lease, error);
    return std::unexpected(std::move(error));
  }
  std::string switched_session_id;
  {
    SCOPED_CRITICAL_AREA_W(opened_w, *unlocked_opened_result);
    opened_w->created = true;
  }
  if (auto replaced = runtime::Session::replace_with(unlocked_session, *unlocked_opened_result); !replaced)
    return std::unexpected(std::move(replaced.error()));
  switched_session_id = runtime::session_ts::rat(unlocked_session)->store.options_r()->session_id;

  result.session_tree_changed = true;
  auto const mode_text = mode == ava::session::SessionBranchMode::Clone ? std::string("cloned") : std::string("forked");
  std::string output = mode_text + " session " + created_session_id + " from " + source_session_id;
  if (!resolved_branch_from_entry_id.empty())
    output += " at " + resolved_branch_from_entry_id;
  if (!trimmed_name.empty())
    output += " name=\"" + sanitize_inline_text(trimmed_name) + "\"";
  output += "\nswitched to " + switched_session_id;
  add_output(result, std::move(output));
  return result;
}

}  // namespace

ava::core::Result<CommandResult> run_fork_command(runtime::session_ts& unlocked_session, std::string_view name, std::string_view branch_from_entry_id)
{
  return run_branch_command(unlocked_session, name, ava::session::SessionBranchMode::Fork, branch_from_entry_id);
}

ava::core::Result<CommandResult> run_clone_command(runtime::session_ts& unlocked_session, std::string_view name)
{
  return run_branch_command(unlocked_session, name, ava::session::SessionBranchMode::Clone);
}

ava::core::Result<CommandResult> run_new_session_command(runtime::session_ts& unlocked_session, std::string_view name)
{
  CommandResult result;
  result.handled = true;

  std::string previous_session_id;
  auto previous_session_title = std::string("Untitled session");
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    previous_session_id = session_r->store.options_r()->session_id;
    if (auto metadata = session_r->load_metadata(); metadata && !metadata->effective_title().empty())
      previous_session_title = sanitize_inline_text(metadata->effective_title());
  }

  auto const trimmed_name = trim_ascii(name);
  auto const created_session_title = trimmed_name.empty() ? std::string("Untitled session") : sanitize_inline_text(trimmed_name);
  auto unlocked_opened_result = create_fresh_session(unlocked_session);
  if (!unlocked_opened_result)
    return std::unexpected(std::move(unlocked_opened_result.error()));

  std::string created_session_id;
  {
    SCOPED_CRITICAL_AREA_W(opened_w, *unlocked_opened_result);
    created_session_id = opened_w->store.options_r()->session_id;
    if (!trimmed_name.empty())
    {
      auto metadata =
          opened_w->append_metadata_1(ava::session::SessionMetadataUpdate{.name = std::optional<std::string>(trimmed_name), .actor = "tui"});
      if (!metadata)
        return std::unexpected(std::move(metadata.error()));
    }
  }
  if (auto replaced = runtime::Session::replace_with(unlocked_session, *unlocked_opened_result); !replaced)
    return std::unexpected(std::move(replaced.error()));

  result.session_tree_changed = true;
  std::string output = "started session \"" + created_session_title + "\" · id " + created_session_id;
  output += "\nprevious session \"" + previous_session_title + "\" · id " + previous_session_id;
  output += "\nswitched to \"" + created_session_title + "\"";
  add_output(result, std::move(output));
  return result;
}

ava::core::Result<CommandResult> run_resume_command(runtime::session_ts& unlocked_session, std::string_view session_id)
{
  CommandResult result;
  result.handled = true;

  auto const trimmed_session_id = trim_ascii(session_id);
  if (trimmed_session_id.empty())
  {
    add_output(result, missing_argument("/resume <id>"));
    return result;
  }

  auto unlocked_opened_result = reopen_session(unlocked_session, trimmed_session_id);
  if (!unlocked_opened_result)
    return std::unexpected(std::move(unlocked_opened_result.error()));

  if (auto replaced = runtime::Session::replace_with(unlocked_session, *unlocked_opened_result); !replaced)
    return std::unexpected(std::move(replaced.error()));
  add_output(result, "resumed session " + runtime::session_ts::rat(unlocked_session)->store.options_r()->session_id);
  return result;
}

ava::core::Result<CommandResult> run_name_command(runtime::session_ts& unlocked_session, std::string_view name)
{
  CommandResult result;
  result.handled = true;

  auto const trimmed_name = trim_ascii(name);
  if (trimmed_name.empty())
  {
    add_output(result, missing_argument("/name <name|--clear>"));
    return result;
  }

  auto const clear_name = trimmed_name == "--clear";
  ava::session::SessionMetadataUpdate update;
  update.name = clear_name ? std::optional<std::string>(std::string{}) : std::optional<std::string>(trimmed_name);
  update.actor = "tui";
  auto metadata = runtime::session_ts::wat(unlocked_session)->append_metadata_1(std::move(update));
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));

  result.session_tree_changed = true;
  if (clear_name)
    add_output(result, "session name cleared");
  else
    add_output(result, "session name set: \"" + sanitize_inline_text(metadata->name) + "\"");
  return result;
}

ava::core::Result<CommandResult> run_labels_command(runtime::session_ts& unlocked_session, std::string_view labels)
{
  CommandResult result;
  result.handled = true;

  auto const parts = split_command_arguments(labels);
  if (parts.empty())
  {
    auto metadata = runtime::session_ts::rat(unlocked_session)->load_metadata();
    if (!metadata)
      return std::unexpected(std::move(metadata.error()));
    auto text = metadata->labels.empty() ? std::string("session labels: <none>") : std::string("session labels: ") + labels_text(metadata->labels);
    text += "\nusage: /labels <label> [label...] | /labels --clear";
    add_output(result, std::move(text));
    return result;
  }

  if (std::ranges::find(parts, "--clear") != parts.end() && parts.size() != 1)
  {
    add_output(result, missing_argument("/labels <label> [label...] | /labels --clear"));
    return result;
  }

  auto next_labels = parts.size() == 1 && parts[0] == "--clear" ? std::vector<std::string>{} : parts;
  ava::session::SessionMetadataUpdate update;
  update.labels = next_labels;
  update.actor = "tui";
  auto metadata = runtime::session_ts::wat(unlocked_session)->append_metadata_1(std::move(update));
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));

  result.session_tree_changed = true;
  if (metadata->labels.empty())
    add_output(result, "session labels cleared");
  else
    add_output(result, "session labels set: " + sanitize_inline_text(labels_text(metadata->labels)));
  return result;
}

ava::core::Result<CommandResult> run_mode_command(runtime::session_ts& unlocked_session)
{
  CommandResult result;
  result.handled = true;
  auto const new_mode = ava::agent::toggle_mode(runtime::session_ts::rat(unlocked_session)->mode());
  auto prompt_state = select_runtime_prompt_state(unlocked_session, new_mode);
  if (!prompt_state)
    return std::unexpected(std::move(prompt_state.error()));
  ava::agent::Mode applied_mode;
  {
    SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
    if (auto appended = session_w->append_mode_change_1(new_mode); !appended)
    {
      return std::unexpected(std::move(appended.error()));
    }
  }
  if (auto refreshed = runtime::Session::apply_prompt_state_and_refresh(unlocked_session, std::move(*prompt_state)); !refreshed)
    return std::unexpected(std::move(refreshed.error()));
  applied_mode = runtime::session_ts::rat(unlocked_session)->mode();
  add_output(result, "mode switched to " + ava::agent::to_string(applied_mode));
  return result;
}

}  // namespace ava::app
