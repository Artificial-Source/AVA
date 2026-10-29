#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_session_support_internal.h"
#include "ava/app/command_sessions.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/session/session_tree.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ava::app {
using session_command_support::contains_ascii_case_insensitive;
using session_command_support::labels_text;
using session_command_support::reopen_session;
using session_command_support::shorten_middle;
using session_command_support::trim_ascii;

namespace {

bool metadata_labels_match(std::vector<std::string> const& labels, std::string_view query)
{
  return std::ranges::any_of(labels, [&](std::string const& label) { return contains_ascii_case_insensitive(label, query); });
}

bool session_matches_query(ava::session::SessionTreeNode const& node, std::string_view query)
{
  return contains_ascii_case_insensitive(node.summary.session_id, query) || contains_ascii_case_insensitive(node.summary.last_updated, query) ||
         contains_ascii_case_insensitive(node.summary.path.generic_string(), query) ||
         contains_ascii_case_insensitive(node.metadata.effective_title(), query) || contains_ascii_case_insensitive(node.metadata.parent_session_id, query) ||
         contains_ascii_case_insensitive(node.metadata.source_session_id, query) ||
         contains_ascii_case_insensitive(node.metadata.branch_from_entry_id, query) || contains_ascii_case_insensitive(node.metadata.branch_origin, query) ||
         (node.metadata.archived && contains_ascii_case_insensitive("archived", query)) || contains_ascii_case_insensitive(node.metadata.actor, query) ||
         metadata_labels_match(node.metadata.labels, query);
}

std::unordered_map<std::string, std::size_t> tree_index_by_id(std::vector<ava::session::SessionTreeNode> const& nodes)
{
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index)
  {
    index.emplace(nodes[node_index].summary.session_id, node_index);
  }
  return index;
}

std::string format_session_tree_line(ava::session::SessionTreeNode const& node, std::size_t depth, bool current_path)
{
  std::string output(depth * 2, ' ');
  output += node.current ? "* " : "- ";
  if (!node.metadata.effective_title().empty())
  {
    output += sanitize_inline_text(node.metadata.effective_title());
    output += "  id=" + sanitize_inline_text(node.summary.session_id);
  }
  else
  {
    output += sanitize_inline_text(node.summary.session_id);
  }
  output += "  entries=" + std::to_string(node.summary.entry_count);
  if (!node.summary.last_updated.empty())
    output += "  updated=" + sanitize_inline_text(node.summary.last_updated);
  if (!node.metadata.branch_origin.empty())
    output += "  origin=" + sanitize_inline_text(node.metadata.branch_origin);
  if (node.metadata.archived)
    output += "  archived";
  if (!node.metadata.parent_session_id.empty())
    output += "  parent=" + sanitize_inline_text(shorten_middle(node.metadata.parent_session_id, 24));
  if (!node.metadata.branch_from_entry_id.empty())
    output += "  from=" + sanitize_inline_text(shorten_middle(node.metadata.branch_from_entry_id, 24));
  if (!node.metadata.labels.empty())
    output += "  labels=" + sanitize_inline_text(labels_text(node.metadata.labels));
  if (current_path && !node.current)
    output += "  current_path";
  return output;
}

ava::core::Result<CommandResult> run_sessions_rename_command(runtime::session_ts& unlocked_session, std::string_view arguments)
{
  CommandResult result;
  result.handled = true;

  auto rest = std::string_view(arguments);
  auto const subcommand = std::string_view("rename");
  if (rest.starts_with(subcommand))
    rest.remove_prefix(subcommand.size());
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())) != 0) rest.remove_prefix(1);
  auto const id_end = rest.find_first_of(" \t\r\n");
  auto const requested_session_id = id_end == std::string_view::npos ? rest : rest.substr(0, id_end);
  if (requested_session_id.empty())
  {
    add_output(result, missing_argument("/sessions rename <id> <name|--clear>"));
    return result;
  }
  rest = id_end == std::string_view::npos ? std::string_view{} : rest.substr(id_end);
  auto const trimmed_name = trim_ascii(rest);
  if (trimmed_name.empty())
  {
    add_output(result, missing_argument("/sessions rename <id> <name|--clear>"));
    return result;
  }

  auto const clear_name = trimmed_name == "--clear";
  ava::session::SessionMetadataUpdate update;
  update.name = clear_name ? std::optional<std::string>(std::string{}) : std::optional<std::string>(trimmed_name);
  update.actor = "tui";

  std::string target_id;
  ava::core::Result<ava::session::SessionMetadataView> metadata =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session rename target was not resolved"));

  CRITICAL_AREA_BEGIN_W(session);
  if (requested_session_id == session_w->store.options_r()->session_id)
  {
    target_id = session_w->store.options_r()->session_id;
    metadata = session_w->append_metadata_1(std::move(update));
    CRITICAL_AREA_END_W(session);
  }
  else
  {
    CRITICAL_AREA_END_W(session);
    auto unlocked_target_result = reopen_session(unlocked_session, requested_session_id);
    if (!unlocked_target_result)
      return std::unexpected(std::move(unlocked_target_result.error()));
    SCOPED_CRITICAL_AREA_W(target_w, *unlocked_target_result);
    target_id = target_w->store.options_r()->session_id;
    metadata = target_w->append_metadata_1(std::move(update));
  }
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));

  result.session_tree_changed = true;
  if (clear_name)
    add_output(result, "session " + target_id + " name cleared");
  else
    add_output(result, "session " + target_id + " name set: \"" + sanitize_inline_text(metadata->name) + "\"");
  return result;
}

ava::core::Result<CommandResult> run_sessions_labels_command(runtime::session_ts& unlocked_session, std::string_view arguments)
{
  CommandResult result;
  result.handled = true;

  auto rest = std::string_view(arguments);
  if (rest.starts_with("labels"))
    rest.remove_prefix(std::string_view("labels").size());
  else if (rest.starts_with("label"))
    rest.remove_prefix(std::string_view("label").size());

  auto const parts = split_command_arguments(rest);
  if (parts.empty())
  {
    add_output(result, missing_argument("/sessions labels <id> <label...|--clear>"));
    return result;
  }

  auto const requested_session_id = parts.front();
  auto unlocked_target_result = reopen_session(unlocked_session, requested_session_id);
  if (!unlocked_target_result)
    return std::unexpected(std::move(unlocked_target_result.error()));

  SCOPED_CRITICAL_AREA_W(target_w, *unlocked_target_result);

  if (parts.size() == 1)
  {
    auto metadata = target_w->load_metadata();
    if (!metadata)
      return std::unexpected(std::move(metadata.error()));
    auto text = metadata->labels.empty() ? std::string("session " + target_w->store.options_r()->session_id + " labels: <none>")
                                         : std::string("session " + target_w->store.options_r()->session_id + " labels: " + labels_text(metadata->labels));
    text += "\nusage: /sessions labels <id> <label...|--clear>";
    add_output(result, std::move(text));
    return result;
  }

  std::vector<std::string> label_parts(parts.begin() + 1, parts.end());
  if (std::ranges::find(label_parts, "--clear") != label_parts.end() && label_parts.size() != 1)
  {
    add_output(result, missing_argument("/sessions labels <id> <label...|--clear>"));
    return result;
  }

  auto next_labels = label_parts.size() == 1 && label_parts[0] == "--clear" ? std::vector<std::string>{} : label_parts;
  ava::session::SessionMetadataUpdate update;
  update.labels = next_labels;
  update.actor = "tui";
  std::string const target_id = target_w->store.options_r()->session_id;
  auto metadata = target_w->append_metadata_1(std::move(update));
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));

  result.session_tree_changed = true;
  if (metadata->labels.empty())
    add_output(result, "session " + target_id + " labels cleared");
  else
    add_output(result, "session " + target_id + " labels set: " + sanitize_inline_text(labels_text(metadata->labels)));
  return result;
}

ava::core::Result<CommandResult> run_sessions_archive_command(runtime::session_ts& unlocked_session, std::string_view arguments, bool archived)
{
  CommandResult result;
  result.handled = true;

  auto rest = std::string_view(arguments);
  auto const subcommand = archived ? std::string_view("archive") : std::string_view("unarchive");
  if (rest.starts_with(subcommand))
    rest.remove_prefix(subcommand.size());
  auto const parts = split_command_arguments(rest);
  if (parts.empty())
  {
    add_output(result, missing_argument(archived ? "/sessions archive <id> --confirm" : "/sessions unarchive <id>"));
    return result;
  }
  auto const requested_session_id = parts.front();
  if (requested_session_id == runtime::session_ts::rat(unlocked_session)->store.options_r()->session_id)
  {
    add_output(result, "Cannot archive the active session. Switch sessions first.");
    return result;
  }
  if (archived && std::ranges::find(parts, "--confirm") == parts.end())
  {
    add_output(result,
               "Archive hides a session from the default selector and /sessions view but keeps its JSONL file intact.\n"
               "Run /sessions archive " +
                   sanitize_inline_text(requested_session_id) + " --confirm to archive it.");
    return result;
  }

  auto unlocked_target_result = reopen_session(unlocked_session, requested_session_id);
  if (!unlocked_target_result)
    return std::unexpected(std::move(unlocked_target_result.error()));

  SCOPED_CRITICAL_AREA_W(target_w, *unlocked_target_result);

  auto current_metadata = target_w->load_metadata();
  if (!current_metadata)
    return std::unexpected(std::move(current_metadata.error()));
  if (current_metadata->archived == archived)
  {
    add_output(result, "session " + target_w->store.options_r()->session_id + (archived ? " already archived" : " is not archived"));
    return result;
  }

  ava::session::SessionMetadataUpdate update;
  update.archived = archived;
  update.actor = "tui";
  auto metadata = target_w->append_metadata_1(std::move(update));
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));

  result.session_tree_changed = true;
  add_output(result, "session " + target_w->store.options_r()->session_id + (metadata->archived ? " archived" : " unarchived"));
  return result;
}

void append_session_tree_lines(std::string& output, std::vector<ava::session::SessionTreeNode> const& nodes,
                               std::unordered_map<std::string, std::size_t> const& index_by_id, std::vector<std::string> const& ids,
                               std::vector<std::string> const& current_path, std::string_view query, std::size_t depth, bool include_archived, bool& wrote_any)
{
  for (auto const& id : ids)
  {
    auto const found = index_by_id.find(id);
    if (found == index_by_id.end())
      continue;
    auto const& node = nodes[found->second];
    auto const query_match = session_matches_query(node, query);
    auto const current_path_node = std::ranges::find(current_path, node.summary.session_id) != current_path.end();
    auto const visible = include_archived || !node.metadata.archived;
    if (visible && (query.empty() || query_match))
    {
      output += format_session_tree_line(node, depth, current_path_node);
      output += '\n';
      wrote_any = true;
    }
    append_session_tree_lines(output, nodes, index_by_id, node.children, current_path, query, depth + (visible ? 1 : 0), include_archived, wrote_any);
  }
}

}  // namespace

ava::core::Result<CommandResult> run_sessions_command(runtime::session_ts& unlocked_session, std::string_view query)
{
  CommandResult result;
  result.handled = true;
  auto const trimmed_query = trim_ascii(query);
  auto const query_parts = split_command_arguments(trimmed_query);
  if (!query_parts.empty() && query_parts.front() == "rename")
  {
    return run_sessions_rename_command(unlocked_session, trimmed_query);
  }
  if (!query_parts.empty() && (query_parts.front() == "labels" || query_parts.front() == "label"))
  {
    return run_sessions_labels_command(unlocked_session, trimmed_query);
  }
  if (!query_parts.empty() && query_parts.front() == "archive")
  {
    return run_sessions_archive_command(unlocked_session, trimmed_query, true);
  }
  if (!query_parts.empty() && query_parts.front() == "unarchive")
  {
    return run_sessions_archive_command(unlocked_session, trimmed_query, false);
  }
  auto list_query = trimmed_query;
  bool include_archived = false;
  if (!query_parts.empty() && query_parts.front() == "--archived")
  {
    include_archived = true;
    list_query = trimmed_query.substr(std::string_view("--archived").size());
    list_query = trim_ascii(list_query);
  }

  CRITICAL_AREA_BEGIN_W(session);
  auto const workspace_dir_copy = session_w->workspace_dir();
  auto const sessions_dir_copy = session_w->paths().sessions_dir;
  auto const session_id_copy = session_w->store.options_r()->session_id;
  CRITICAL_AREA_END_W(session);

  auto tree = ava::session::build_session_tree(workspace_dir_copy, sessions_dir_copy, session_id_copy);
  if (!tree)
  {
    add_output(result, tree.error().format());
    return result;
  }
  if (tree->sessions.empty())
  {
    add_output(result, "No sessions for this workspace.");
    return result;
  }
  std::string output;
  output += include_archived ? "Sessions (including archived):\n" : "Sessions:\n";
  auto const index_by_id = tree_index_by_id(tree->sessions);
  bool wrote_any = false;
  append_session_tree_lines(output, tree->sessions, index_by_id, tree->roots, tree->current_path, list_query, 0, include_archived, wrote_any);
  if (!wrote_any && !list_query.empty())
  {
    add_output(result, "No sessions matching: " + sanitize_inline_text(list_query));
    return result;
  }
  if (!wrote_any && !include_archived)
  {
    add_output(result, "No active sessions for this workspace. Use /sessions --archived to include archived sessions.");
    return result;
  }
  add_output(result, std::move(output));
  return result;
}

}  // namespace ava::app
