#include "sys.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/command_format.h"
#include "ava/app/command_permissions.h"
#include "ava/app/command_session_support_internal.h"
#include "ava/app/runtime/Session.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

using session_command_support::trim_ascii;

struct ParsedAddRule
{
  ava::permissions::PermissionRuleDraft draft;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PermissionAuditRow
{
  std::string timestamp;
  std::string entry_id;
  std::string request_id;
  std::string operation;
  std::string mode;
  std::string tool_name;
  std::string action;
  std::string reason;
  std::string risk;
  std::string target_path;
  std::string command;
  std::string resolution;
  std::string resolution_source;
  std::string resolution_reason;
  std::string actor;
  std::string rule_id;
  std::string recipe_display;
  std::string global_recipe_key;
  std::string workspace_recipe_key;
  std::string effective_allowed_scopes;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

std::string permissions_usage()
{
  return "usage: /permissions <list|audit|diagnose|explain|add|remove> ...\n"
         "  /permissions list [query]\n"
         "  /permissions audit [query]\n"
         "  /permissions audit summary [query]\n"
         "  /permissions audit show <entry_id|request_id>\n"
         "  /permissions audit export [query]\n"
         "  /permissions diagnose [query]\n"
         "  /permissions explain <rule_id>\n"
         "  /permissions add action=<allow|deny> operation=<operation> reason=\"<why>\" "
         "[scope=workspace|global] [mode=any|build|plan] [path=<path>] [command=\"<cmd>\"] [recipe_key=<key>] [tool=<tool>]\n"
         "    Advanced: exact Critical command Allows require critical_acknowledged=true and command=<exact command>; prompt UI never creates them.\n"
         "  /permissions remove <rule_id>\n"
         "    Remove requires the full rule id from explain/completion; list ordinals are display-only.\n"
         "operations: read, search, edit, bash, network.fetch, network.search, lsp.server.launch, lsp.query, "
         "skill, task, plugin.execute, plugin.tool.call, plugin.command.run, plugin.ui.present, plugin.event.observe, mcp.server.launch, "
         "mcp.server.connect, mcp.tool.call, mcp.resource.read";
}

std::string lower_ascii(std::string_view text)
{
  std::string lowered(text);
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lowered;
}

bool contains_ascii_case_insensitive(std::string_view text, std::string_view query)
{
  if (query.empty())
    return true;
  return lower_ascii(text).find(lower_ascii(query)) != std::string::npos;
}

std::string display_permission_path(std::filesystem::path const& path, std::filesystem::path const& workspace)
{
  if (path.empty())
    return "";
  auto text = display_path(path, workspace);
  if (text == ".." || text.starts_with("../"))
    return path.generic_string();
  return text;
}

std::string permission_rule_storage_path(ava::permissions::PermissionRuleStore const& store, ava::permissions::PersistentPermissionRule const& rule)
{
  return ava::permissions::enforceable_permission_rules_file(store, rule.scope).generic_string();
}

bool identity_equals_ascii_case_insensitive(std::string_view value, std::string_view expected)
{
  return lower_ascii(value) == lower_ascii(expected);
}

// Dense primary-line qualifier budget: UTF-8-safe byte cap keeps list/receipt
// lines compact without depending on TUI cluster width helpers.
constexpr std::size_t kPermissionSummaryQualifierMaxBytes = 120;
constexpr std::size_t kPermissionSummaryToolQualifierMaxBytes = 48;
constexpr std::size_t kPermissionSummaryTaskTypeMaxBytes = 32;

std::size_t utf8_prefix_bytes(std::string_view text, std::size_t max_bytes)
{
  if (text.size() <= max_bytes)
    return text.size();
  std::size_t index = 0;
  std::size_t end = 0;
  while (index < text.size())
  {
    auto const byte = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    if (byte < 0x80U)
      length = 1;
    else if ((byte & 0xE0U) == 0xC0U)
      length = 2;
    else if ((byte & 0xF0U) == 0xE0U)
      length = 3;
    else if ((byte & 0xF8U) == 0xF0U)
      length = 4;
    else
      length = 1;
    if (index + length > text.size())
      length = 1;
    if (index + length > max_bytes)
      break;
    index += length;
    end = index;
  }
  return end;
}

std::string bounded_safe_display_qualifier(std::string_view text, std::size_t max_bytes)
{
  auto cleaned = sanitize_inline_text(std::string(text));
  cleaned.resize(utf8_prefix_bytes(cleaned, max_bytes));
  return cleaned;
}

bool is_bounded_identifier_like_qualifier(std::string_view value, std::size_t max_bytes)
{
  if (value.empty() || value.size() > max_bytes)
    return false;
  for (char const ch : value)
  {
    auto const unsigned_ch = static_cast<unsigned char>(ch);
    if (std::isalnum(unsigned_ch) == 0 && ch != '_' && ch != '-')
      return false;
  }
  return true;
}

void append_summary_qualifier(std::string& summary, std::string_view qualifier)
{
  if (qualifier.empty())
    return;
  summary += " · ";
  summary += qualifier;
}

std::string permission_rule_action_label(ava::permissions::PermissionAction action)
{
  switch (action)
  {
    case ava::permissions::PermissionAction::Allow:
      return "Allow";
    case ava::permissions::PermissionAction::Deny:
      return "Block";
    case ava::permissions::PermissionAction::Ask:
      return "Ask";
  }
  return "Block";
}

std::string permission_rule_scope_label(ava::permissions::PermissionRuleScope scope)
{
  switch (scope)
  {
    case ava::permissions::PermissionRuleScope::Global:
      return "Global";
    case ava::permissions::PermissionRuleScope::Workspace:
      return "Workspace";
  }
  return "Workspace";
}

std::string permission_rule_mode_label(ava::permissions::PermissionRuleMode mode)
{
  switch (mode)
  {
    case ava::permissions::PermissionRuleMode::Any:
      return {};
    case ava::permissions::PermissionRuleMode::Build:
      return "Build";
    case ava::permissions::PermissionRuleMode::Plan:
      return "Plan";
  }
  return {};
}

std::string permission_rule_operation_subject(ava::permissions::Operation operation)
{
  switch (operation)
  {
    case ava::permissions::Operation::ReadFile:
      return "file reads";
    case ava::permissions::Operation::SearchFiles:
      return "file searches";
    case ava::permissions::Operation::EditFile:
      return "file edits";
    case ava::permissions::Operation::RunCommand:
      return "shell commands";
    case ava::permissions::Operation::NetworkFetch:
      return "network fetches";
    case ava::permissions::Operation::NetworkSearch:
      return "network searches";
    case ava::permissions::Operation::LspServerLaunch:
      return "LSP server launches";
    case ava::permissions::Operation::LspQuery:
      return "LSP queries";
    case ava::permissions::Operation::SkillLoad:
      return "skills";
    case ava::permissions::Operation::TaskRun:
      return "subagents";
    case ava::permissions::Operation::PluginExecute:
      return "plugin execution";
    case ava::permissions::Operation::PluginToolCall:
      return "plugin tool calls";
    case ava::permissions::Operation::PluginCommandRun:
      return "plugin commands";
    case ava::permissions::Operation::PluginUiPresent:
      return "plugin UI presentation";
    case ava::permissions::Operation::PluginEventObserve:
      return "plugin event observation";
    case ava::permissions::Operation::McpServerLaunch:
      return "MCP server launches";
    case ava::permissions::Operation::McpServerConnect:
      return "MCP server connections";
    case ava::permissions::Operation::McpToolCall:
      return "MCP tool calls";
    case ava::permissions::Operation::McpResourceRead:
      return "MCP resource reads";
  }
  return "operations";
}

bool task_run_is_explore_identity(ava::permissions::PersistentPermissionRule const& rule)
{
  // Explore class requires exact command identity "explore" plus a non-conflicting
  // tool slot (empty, default task, or also explore). tool_name=explore alone,
  // free-form command + explore tool, or explore command + conflicting tool must
  // not claim Explore; they keep fixed "subagents" framing instead.
  if (!identity_equals_ascii_case_insensitive(rule.command, "explore"))
    return false;
  if (rule.tool_name.empty())
    return true;
  return identity_equals_ascii_case_insensitive(rule.tool_name, "task") || identity_equals_ascii_case_insensitive(rule.tool_name, "explore");
}

bool tool_name_is_operation_default(ava::permissions::PersistentPermissionRule const& rule)
{
  if (rule.tool_name.empty())
    return true;

  auto const operation_token = ava::permissions::to_string(rule.operation);
  if (identity_equals_ascii_case_insensitive(rule.tool_name, operation_token))
    return true;

  switch (rule.operation)
  {
    case ava::permissions::Operation::ReadFile:
      return identity_equals_ascii_case_insensitive(rule.tool_name, "read");
    case ava::permissions::Operation::EditFile:
      return identity_equals_ascii_case_insensitive(rule.tool_name, "edit");
    case ava::permissions::Operation::SearchFiles:
      return identity_equals_ascii_case_insensitive(rule.tool_name, "search") || identity_equals_ascii_case_insensitive(rule.tool_name, "grep") ||
             identity_equals_ascii_case_insensitive(rule.tool_name, "glob") || identity_equals_ascii_case_insensitive(rule.tool_name, "list");
    case ava::permissions::Operation::RunCommand:
      return identity_equals_ascii_case_insensitive(rule.tool_name, "bash") || identity_equals_ascii_case_insensitive(rule.tool_name, "shell");
    case ava::permissions::Operation::TaskRun:
      return identity_equals_ascii_case_insensitive(rule.tool_name, "task");
    case ava::permissions::Operation::SkillLoad:
      return identity_equals_ascii_case_insensitive(rule.tool_name, "skill");
    default:
      return false;
  }
}

// Fixed human operation class only. Qualifiers (tool, recipe display, path, task
// type) are appended separately so forged names cannot replace authority class.
std::string permission_rule_subject(ava::permissions::PersistentPermissionRule const& rule)
{
  if (rule.operation == ava::permissions::Operation::TaskRun)
  {
    if (task_run_is_explore_identity(rule))
      return "Explore subagents";
    return "subagents";
  }

  if (rule.operation == ava::permissions::Operation::RunCommand)
  {
    if (!rule.recipe_display.empty())
      return "recipe";
    if (!rule.command.empty() || !rule.command_recipe_key.empty())
      return "Exact command";
    return "shell commands";
  }

  return permission_rule_operation_subject(rule.operation);
}

std::string permission_rule_task_type_qualifier(ava::permissions::PersistentPermissionRule const& rule)
{
  if (rule.operation != ava::permissions::Operation::TaskRun || task_run_is_explore_identity(rule))
    return {};

  // Task command/tool strings are forgeable. Keep only a short identifier-like
  // type token; never print free-form task command bodies.
  if (is_bounded_identifier_like_qualifier(rule.command, kPermissionSummaryTaskTypeMaxBytes))
    return std::string(rule.command);
  if (!tool_name_is_operation_default(rule) && is_bounded_identifier_like_qualifier(rule.tool_name, kPermissionSummaryTaskTypeMaxBytes))
    return std::string(rule.tool_name);
  return {};
}

std::string permission_rule_tool_qualifier(ava::permissions::PersistentPermissionRule const& rule)
{
  if (rule.operation == ava::permissions::Operation::TaskRun)
    return {};
  if (rule.operation == ava::permissions::Operation::RunCommand && !rule.recipe_display.empty())
    return {};
  if (tool_name_is_operation_default(rule))
    return {};
  return bounded_safe_display_qualifier(rule.tool_name, kPermissionSummaryToolQualifierMaxBytes);
}

std::string format_permission_rule_summary_impl(ava::permissions::PersistentPermissionRule const& rule, std::filesystem::path const& workspace_dir)
{
  std::string summary = permission_rule_action_label(rule.action);
  summary += ' ';
  summary += permission_rule_subject(rule);

  if (rule.operation == ava::permissions::Operation::RunCommand && !rule.recipe_display.empty())
    append_summary_qualifier(summary, bounded_safe_display_qualifier(rule.recipe_display, kPermissionSummaryQualifierMaxBytes));
  else if (rule.operation == ava::permissions::Operation::TaskRun)
    append_summary_qualifier(summary, permission_rule_task_type_qualifier(rule));
  else
    append_summary_qualifier(summary, permission_rule_tool_qualifier(rule));

  // Any non-empty target_path is authority-critical, including RunCommand cwd and TaskRun paths.
  if (!rule.target_path.empty())
  {
    auto const path_text = sanitize_inline_text(display_permission_path(rule.target_path, workspace_dir));
    append_summary_qualifier(summary, path_text);
  }

  append_summary_qualifier(summary, permission_rule_scope_label(rule.scope));

  if (auto const mode_label = permission_rule_mode_label(rule.mode); !mode_label.empty())
    append_summary_qualifier(summary, mode_label);
  return summary;
}

std::string format_permission_rule_receipt(ava::permissions::PersistentPermissionRule const& rule, runtime::session_ts const& unlocked_session,
                                           std::string_view verb)
{
  auto const workspace_dir = runtime::session_ts::crat(unlocked_session)->workspace_dir();
  std::ostringstream output;
  output << verb << " permission rule\n";
  output << format_permission_rule_summary_impl(rule, workspace_dir) << "\n";
  output << "Rule ID: " << sanitize_inline_text(rule.rule_id);
  return output.str();
}

bool looks_like_display_ordinal(std::string_view text)
{
  if (text.empty())
    return false;
  for (char const ch : text)
  {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0)
      return false;
  }
  return true;
}

bool rule_matches_query(ava::permissions::PersistentPermissionRule const& rule, runtime::session_ts const& unlocked_session, std::string_view query)
{
  if (query.empty())
    return true;
  auto const workspace_dir = runtime::session_ts::crat(unlocked_session)->workspace_dir();
  auto const summary = format_permission_rule_summary_impl(rule, workspace_dir);
  return contains_ascii_case_insensitive(summary, query) || contains_ascii_case_insensitive(rule.rule_id, query) ||
         contains_ascii_case_insensitive(ava::permissions::to_string(rule.scope), query) ||
         contains_ascii_case_insensitive(ava::permissions::to_string(rule.action), query) ||
         contains_ascii_case_insensitive(ava::permissions::to_string(rule.operation), query) ||
         contains_ascii_case_insensitive(ava::permissions::to_string(rule.mode), query) || contains_ascii_case_insensitive(rule.tool_name, query) ||
         contains_ascii_case_insensitive(display_permission_path(rule.target_path, workspace_dir), query) ||
         // Command text may match filters privately; list/receipts never print the body.
         contains_ascii_case_insensitive(rule.command, query) || contains_ascii_case_insensitive(rule.command_recipe_key, query) ||
         contains_ascii_case_insensitive(rule.recipe_display, query) || contains_ascii_case_insensitive(rule.reason, query) ||
         contains_ascii_case_insensitive(rule.actor, query) || contains_ascii_case_insensitive(rule.created_at, query);
}

std::string audit_field(std::string_view data_json, std::string_view key)
{
  return ava::core::json::string_field(data_json, key).value_or("");
}

PermissionAuditRow permission_audit_row(ava::session::SessionEntry const& entry, runtime::session_ts const& unlocked_session)
{
  auto const workspace_dir = runtime::session_ts::crat(unlocked_session)->workspace_dir();
  auto target_path = audit_field(entry.data_json, "target_path");
  if (!target_path.empty())
    target_path = display_permission_path(target_path, workspace_dir);

  auto const command_metadata = ava::core::json::object_field(entry.data_json, "command_metadata").value_or("");
  auto scopes = ava::core::json::strings_in_array_field(command_metadata, "effective_allowed_scopes");
  auto scope_text = std::string{};
  for (std::size_t index = 0; index < scopes.size(); ++index)
  {
    if (index > 0)
      scope_text += ",";
    scope_text += scopes[index];
  }

  return PermissionAuditRow{.timestamp = entry.timestamp,
                            .entry_id = entry.id,
                            .request_id = audit_field(entry.data_json, "permission_request_id"),
                            .operation = audit_field(entry.data_json, "operation"),
                            .mode = audit_field(entry.data_json, "mode"),
                            .tool_name = audit_field(entry.data_json, "tool_name"),
                            .action = audit_field(entry.data_json, "action"),
                            .reason = audit_field(entry.data_json, "reason"),
                            .risk = audit_field(entry.data_json, "risk"),
                            .target_path = std::move(target_path),
                            .command = audit_field(entry.data_json, "command"),
                            .resolution = audit_field(entry.data_json, "resolution"),
                            .resolution_source = audit_field(entry.data_json, "resolution_source"),
                            .resolution_reason = audit_field(entry.data_json, "resolution_reason"),
                            .actor = audit_field(entry.data_json, "actor"),
                            .rule_id = audit_field(entry.data_json, "rule_id"),
                            .recipe_display = audit_field(command_metadata, "recipe_display"),
                            .global_recipe_key = audit_field(command_metadata, "global_recipe_key"),
                            .workspace_recipe_key = audit_field(command_metadata, "workspace_recipe_key"),
                            .effective_allowed_scopes = std::move(scope_text)};
}

void append_audit_field(std::string& line, std::string_view label, std::string_view value)
{
  if (value.empty())
    return;
  line += " ";
  line += label;
  line += "=";
  line += sanitize_inline_text(std::string(value));
}

void append_audit_quoted_field(std::string& line, std::string_view label, std::string_view value)
{
  if (value.empty())
    return;
  line += " ";
  line += label;
  line += "=\"";
  line += sanitize_inline_text(std::string(value));
  line += "\"";
}

std::string format_permission_audit_line(PermissionAuditRow const& row)
{
  std::string line = "- " + sanitize_inline_text(row.timestamp);
  append_audit_field(line, "entry", row.entry_id);
  append_audit_field(line, "request", row.request_id);
  append_audit_field(line, "action", row.action);
  append_audit_field(line, "resolution", row.resolution);
  append_audit_field(line, "source", row.resolution_source);
  append_audit_field(line, "operation", row.operation);
  append_audit_field(line, "mode", row.mode);
  append_audit_field(line, "risk", row.risk);
  append_audit_field(line, "tool", row.tool_name);
  append_audit_field(line, "path", row.target_path);
  append_audit_quoted_field(line, "command", row.command);
  append_audit_quoted_field(line, "reason", row.reason);
  append_audit_quoted_field(line, "resolution_reason", row.resolution_reason);
  append_audit_field(line, "rule", row.rule_id);
  append_audit_quoted_field(line, "recipe", row.recipe_display);
  append_audit_field(line, "global_recipe_key", row.global_recipe_key);
  append_audit_field(line, "workspace_recipe_key", row.workspace_recipe_key);
  append_audit_field(line, "scopes", row.effective_allowed_scopes);
  append_audit_field(line, "actor", row.actor);
  return line;
}

std::string markdown_table_cell(std::string_view text)
{
  auto sanitized = sanitize_inline_text(std::string(text));
  std::string escaped;
  escaped.reserve(sanitized.size());
  for (char const ch : sanitized)
  {
    if (ch == '|')
      escaped.push_back('\\');
    escaped.push_back(ch);
  }
  return escaped;
}

bool audit_line_matches_query(std::string const& line, std::string_view data_json, std::string_view query)
{
  if (query.empty())
    return true;
  return contains_ascii_case_insensitive(line, query) || contains_ascii_case_insensitive(data_json, query);
}

std::string format_permission_audit(std::vector<ava::session::SessionEntry> const& entries, runtime::session_ts const& unlocked_session,
                                    std::string_view query)
{
  static constexpr std::size_t kMaxDisplayedAuditRows = 50;

  auto const session_id = runtime::session_ts::crat(unlocked_session)->store.options_r()->session_id;
  std::ostringstream output;
  output << "Permission audit:\n";
  output << "  session: " << sanitize_inline_text(session_id) << "\n";
  output << "  behavior: append-only session permission decisions; most recent first\n";
  if (!query.empty())
    output << "  filter: " << sanitize_inline_text(std::string(query)) << "\n";

  std::size_t audit_count = 0;
  std::size_t matched_count = 0;
  std::vector<std::string> rows;

  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
  {
    if (entry->type != ava::session::EntryType::PermissionDecision)
      continue;
    ++audit_count;

    auto const row = permission_audit_row(*entry, unlocked_session);
    auto const line = format_permission_audit_line(row);
    if (!audit_line_matches_query(line, entry->data_json, query))
      continue;
    ++matched_count;
    if (rows.size() < kMaxDisplayedAuditRows)
      rows.push_back(line);
  }

  if (audit_count == 0)
  {
    output << "\nNo permission audit entries for this session.";
    return output.str();
  }
  if (matched_count == 0)
  {
    output << "\nNo permission audit entries match \"" << sanitize_inline_text(std::string(query)) << "\".";
    return output.str();
  }

  output << "  showing: " << rows.size() << " of " << matched_count << " matching entries";
  if (matched_count != audit_count)
    output << " (" << audit_count << " total audit entries)";
  if (matched_count > rows.size())
    output << "; newest " << kMaxDisplayedAuditRows << " displayed";
  output << "\n\n";

  for (auto const& row : rows)
  {
    output << row << "\n";
  }
  return output.str();
}

std::string format_permission_audit_export(std::vector<ava::session::SessionEntry> const& entries, runtime::session_ts const& unlocked_session,
                                           std::string_view query)
{
  static constexpr std::size_t kMaxDisplayedAuditRows = 50;

  auto const session_id = runtime::session_ts::crat(unlocked_session)->store.options_r()->session_id;
  std::ostringstream output;
  output << "Permission audit export:\n";
  output << "  session: " << sanitize_inline_text(session_id) << "\n";
  output << "  format: markdown table\n";
  output << "  behavior: append-only session permission decisions; most recent first\n";
  if (!query.empty())
    output << "  filter: " << sanitize_inline_text(std::string(query)) << "\n";

  std::size_t audit_count = 0;
  std::size_t matched_count = 0;
  std::vector<PermissionAuditRow> rows;

  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
  {
    if (entry->type != ava::session::EntryType::PermissionDecision)
      continue;
    ++audit_count;

    auto row = permission_audit_row(*entry, unlocked_session);
    auto const line = format_permission_audit_line(row);
    if (!audit_line_matches_query(line, entry->data_json, query))
      continue;
    ++matched_count;
    if (rows.size() < kMaxDisplayedAuditRows)
      rows.push_back(std::move(row));
  }

  if (audit_count == 0)
  {
    output << "\nNo permission audit entries for this session.";
    return output.str();
  }
  if (matched_count == 0)
  {
    output << "\nNo permission audit entries match \"" << sanitize_inline_text(std::string(query)) << "\".";
    return output.str();
  }

  output << "  rows: " << rows.size() << " of " << matched_count << " matching entries";
  if (matched_count != audit_count)
    output << " (" << audit_count << " total audit entries)";
  if (matched_count > rows.size())
    output << "; newest " << kMaxDisplayedAuditRows << " exported";
  output << "\n\n";

  output << "```markdown\n";
  output << "| timestamp | entry | request | action | resolution | source | operation | mode | risk | tool | target | command | "
            "reason | resolution reason | rule | recipe | global recipe key | workspace recipe key | scopes | actor |\n";
  output << "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
  for (auto const& row : rows)
  {
    output << "| " << markdown_table_cell(row.timestamp) << " | " << markdown_table_cell(row.entry_id) << " | " << markdown_table_cell(row.request_id) << " | "
           << markdown_table_cell(row.action) << " | " << markdown_table_cell(row.resolution) << " | " << markdown_table_cell(row.resolution_source) << " | "
           << markdown_table_cell(row.operation) << " | " << markdown_table_cell(row.mode) << " | " << markdown_table_cell(row.risk) << " | "
           << markdown_table_cell(row.tool_name) << " | " << markdown_table_cell(row.target_path) << " | " << markdown_table_cell(row.command) << " | "
           << markdown_table_cell(row.reason) << " | " << markdown_table_cell(row.resolution_reason) << " | " << markdown_table_cell(row.rule_id) << " | "
           << markdown_table_cell(row.recipe_display) << " | " << markdown_table_cell(row.global_recipe_key) << " | "
           << markdown_table_cell(row.workspace_recipe_key) << " | " << markdown_table_cell(row.effective_allowed_scopes) << " | "
           << markdown_table_cell(row.actor) << " |\n";
  }
  output << "```";
  return output.str();
}

void increment_summary_count(std::unordered_map<std::string, std::size_t>& counts, std::string_view value)
{
  if (value.empty())
    return;
  ++counts[std::string(value)];
}

std::string format_summary_counts(std::string_view label, std::unordered_map<std::string, std::size_t> const& counts)
{
  std::vector<std::pair<std::string, std::size_t>> sorted;
  sorted.reserve(counts.size());
  for (auto const& [key, count] : counts)
  {
    sorted.emplace_back(key, count);
  }
  std::ranges::sort(sorted, [](auto const& lhs, auto const& rhs) {
    if (lhs.second != rhs.second)
      return lhs.second > rhs.second;
    return lhs.first < rhs.first;
  });

  std::ostringstream output;
  output << "  by " << label << ": ";
  if (sorted.empty())
  {
    output << "none";
    return output.str();
  }

  for (auto index = std::size_t{0}; index < sorted.size(); ++index)
  {
    if (index > 0)
      output << ", ";
    output << sanitize_inline_text(sorted[index].first) << "=" << sorted[index].second;
  }
  return output.str();
}

std::string format_permission_audit_summary(std::vector<ava::session::SessionEntry> const& entries, runtime::session_ts const& unlocked_session,
                                            std::string_view query)
{
  auto const session_id = runtime::session_ts::crat(unlocked_session)->store.options_r()->session_id;
  std::ostringstream output;
  output << "Permission audit summary:\n";
  output << "  session: " << sanitize_inline_text(session_id) << "\n";
  output << "  behavior: read-only grouped summary over append-only session permission decisions\n";
  if (!query.empty())
    output << "  filter: " << sanitize_inline_text(std::string(query)) << "\n";

  std::size_t audit_count = 0;
  std::size_t matched_count = 0;
  std::size_t denial_count = 0;
  std::string newest_matching_request;
  std::string newest_matching_entry;
  std::unordered_map<std::string, std::size_t> by_action;
  std::unordered_map<std::string, std::size_t> by_resolution;
  std::unordered_map<std::string, std::size_t> by_source;
  std::unordered_map<std::string, std::size_t> by_risk;
  std::unordered_map<std::string, std::size_t> by_operation;
  std::unordered_map<std::string, std::size_t> by_tool;

  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
  {
    if (entry->type != ava::session::EntryType::PermissionDecision)
      continue;
    ++audit_count;

    auto const row = permission_audit_row(*entry, unlocked_session);
    auto const line = format_permission_audit_line(row);
    if (!audit_line_matches_query(line, entry->data_json, query))
      continue;

    ++matched_count;
    if (lower_ascii(row.action) == "deny" || lower_ascii(row.resolution) == "deny")
      ++denial_count;
    if (newest_matching_entry.empty())
      newest_matching_entry = row.entry_id;
    if (newest_matching_request.empty())
      newest_matching_request = row.request_id;
    increment_summary_count(by_action, row.action);
    increment_summary_count(by_resolution, row.resolution);
    increment_summary_count(by_source, row.resolution_source);
    increment_summary_count(by_risk, row.risk);
    increment_summary_count(by_operation, row.operation);
    increment_summary_count(by_tool, row.tool_name);
  }

  if (audit_count == 0)
  {
    output << "\nNo permission audit entries for this session.";
    return output.str();
  }
  if (matched_count == 0)
  {
    output << "\nNo permission audit entries match \"" << sanitize_inline_text(std::string(query)) << "\".";
    return output.str();
  }

  output << "  entries: " << matched_count << " matching";
  if (matched_count != audit_count)
    output << " of " << audit_count << " total";
  output << "\n";
  output << "  denials: " << denial_count << "\n";
  output << format_summary_counts("action", by_action) << "\n";
  output << format_summary_counts("resolution", by_resolution) << "\n";
  output << format_summary_counts("source", by_source) << "\n";
  output << format_summary_counts("risk", by_risk) << "\n";
  output << format_summary_counts("operation", by_operation) << "\n";
  output << format_summary_counts("tool", by_tool) << "\n";

  output << "\nRelated commands:\n";
  output << "  /permissions audit";
  if (!query.empty())
    output << " " << sanitize_inline_text(std::string(query));
  output << "\n";
  output << "  /permissions audit export";
  if (!query.empty())
    output << " " << sanitize_inline_text(std::string(query));
  output << "\n";
  output << "  /permissions diagnose";
  if (!query.empty())
    output << " " << sanitize_inline_text(std::string(query));
  output << "\n";
  if (!newest_matching_request.empty())
    output << "  /permissions audit show " << sanitize_inline_text(newest_matching_request) << "\n";
  else if (!newest_matching_entry.empty())
    output << "  /permissions audit show " << sanitize_inline_text(newest_matching_entry) << "\n";
  return output.str();
}

bool audit_row_matches_selector(PermissionAuditRow const& row, std::string_view selector)
{
  if (selector.empty())
    return false;
  return row.entry_id == selector || row.request_id == selector || row.entry_id.starts_with(selector) || row.request_id.starts_with(selector);
}

void append_audit_detail_field(std::ostringstream& output, std::string_view label, std::string_view value)
{
  if (value.empty())
    return;
  output << "  " << label << ": " << sanitize_inline_text(std::string(value)) << "\n";
}

std::string format_permission_audit_detail(std::vector<ava::session::SessionEntry> const& entries, runtime::session_ts const& unlocked_session,
                                           std::string_view selector)
{
  auto const session_id = runtime::session_ts::crat(unlocked_session)->store.options_r()->session_id;
  std::ostringstream output;
  output << "Permission audit detail:\n";
  output << "  session: " << sanitize_inline_text(session_id) << "\n";
  output << "  selector: " << sanitize_inline_text(std::string(selector)) << "\n";
  output << "  behavior: matching entry ids and permission request ids; oldest matching decision first\n";

  std::vector<PermissionAuditRow> rows;
  std::size_t audit_count = 0;
  for (auto const& entry : entries)
  {
    if (entry.type != ava::session::EntryType::PermissionDecision)
      continue;
    ++audit_count;
    auto row = permission_audit_row(entry, unlocked_session);
    if (audit_row_matches_selector(row, selector))
      rows.push_back(std::move(row));
  }

  if (rows.empty())
  {
    output << "\nNo permission audit entries match \"" << sanitize_inline_text(std::string(selector)) << "\".";
    if (audit_count > 0)
      output << "\nUse /permissions audit [query] to find request or entry ids.";
    return output.str();
  }

  output << "  matched entries: " << rows.size() << " of " << audit_count << " audit entries\n\n";
  for (auto const& row : rows)
  {
    output << "- " << sanitize_inline_text(row.timestamp) << "\n";
    append_audit_detail_field(output, "entry", row.entry_id);
    append_audit_detail_field(output, "request", row.request_id);
    append_audit_detail_field(output, "operation", row.operation);
    append_audit_detail_field(output, "mode", row.mode);
    append_audit_detail_field(output, "tool", row.tool_name);
    append_audit_detail_field(output, "action", row.action);
    append_audit_detail_field(output, "resolution", row.resolution);
    append_audit_detail_field(output, "source", row.resolution_source);
    append_audit_detail_field(output, "risk", row.risk);
    append_audit_detail_field(output, "path", row.target_path);
    append_audit_detail_field(output, "command", row.command);
    append_audit_detail_field(output, "reason", row.reason);
    append_audit_detail_field(output, "resolution reason", row.resolution_reason);
    append_audit_detail_field(output, "rule", row.rule_id);
    append_audit_detail_field(output, "recipe", row.recipe_display);
    append_audit_detail_field(output, "global recipe key", row.global_recipe_key);
    append_audit_detail_field(output, "workspace recipe key", row.workspace_recipe_key);
    append_audit_detail_field(output, "effective allowed scopes", row.effective_allowed_scopes);
    append_audit_detail_field(output, "actor", row.actor);
  }

  output << "\nRelated commands:\n";
  output << "  /permissions audit " << sanitize_inline_text(std::string(selector)) << "\n";
  output << "  /permissions diagnose " << sanitize_inline_text(std::string(selector)) << "\n";
  output << "  /permissions audit export " << sanitize_inline_text(std::string(selector)) << "\n";
  for (auto const& row : rows)
  {
    if (!row.rule_id.empty())
      output << "  /permissions explain " << sanitize_inline_text(row.rule_id) << "\n";
  }
  return output.str();
}

bool permission_audit_row_is_denial(PermissionAuditRow const& row)
{
  return lower_ascii(row.action) == "deny" || lower_ascii(row.resolution) == "deny";
}

std::string permission_audit_row_target(PermissionAuditRow const& row)
{
  if (!row.command.empty())
    return "command=\"" + sanitize_inline_text(row.command) + "\"";
  if (!row.target_path.empty())
    return "path=" + sanitize_inline_text(row.target_path);
  if (!row.tool_name.empty())
    return "tool=" + sanitize_inline_text(row.tool_name);
  return "";
}

std::string permission_audit_row_decision(PermissionAuditRow const& row)
{
  if (!row.resolution.empty())
    return row.resolution;
  return row.action;
}

std::string format_permission_denial_diagnostics(std::vector<ava::session::SessionEntry> const& entries,
                                                 runtime::session_ts const& unlocked_session,
                                                 std::vector<ava::permissions::PersistentPermissionRule> const& rules, std::string_view query)
{
  static constexpr std::size_t kMaxDisplayedDenials = 10;

  std::ostringstream output;
  output << "Recent permission denials:\n";
  if (!query.empty())
    output << "  filter: " << sanitize_inline_text(std::string(query)) << "\n";

  std::size_t denial_count = 0;
  std::size_t matched_count = 0;
  std::vector<PermissionAuditRow> rows;
  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
  {
    if (entry->type != ava::session::EntryType::PermissionDecision)
      continue;

    auto row = permission_audit_row(*entry, unlocked_session);
    if (!permission_audit_row_is_denial(row))
      continue;
    ++denial_count;

    auto const line = format_permission_audit_line(row);
    if (!audit_line_matches_query(line, entry->data_json, query))
      continue;
    ++matched_count;
    if (rows.size() < kMaxDisplayedDenials)
      rows.push_back(std::move(row));
  }

  if (denial_count == 0)
  {
    output << "\nNo denied permission decisions for this session.";
    return output.str();
  }
  if (matched_count == 0)
  {
    output << "\nNo denied permission decisions match \"" << sanitize_inline_text(std::string(query)) << "\".";
    return output.str();
  }

  output << "  showing: " << rows.size() << " of " << matched_count << " matching denials";
  if (matched_count != denial_count)
    output << " (" << denial_count << " total denials)";
  if (matched_count > rows.size())
    output << "; newest " << kMaxDisplayedDenials << " displayed";
  output << "\n\n";

  for (auto const& row : rows)
  {
    auto const decision = permission_audit_row_decision(row);
    auto const target = permission_audit_row_target(row);
    auto const* rule = [&rules, &row]() -> ava::permissions::PersistentPermissionRule const* {
      if (row.rule_id.empty())
        return nullptr;
      auto const found = std::ranges::find_if(rules, [&](auto const& candidate) { return candidate.rule_id == row.rule_id; });
      if (found == rules.end())
        return nullptr;
      return &*found;
    }();

    output << "- " << sanitize_inline_text(row.timestamp);
    if (!row.request_id.empty())
      output << " request=" << sanitize_inline_text(row.request_id);
    if (!decision.empty())
      output << " decision=" << sanitize_inline_text(decision);
    if (!row.resolution_source.empty())
      output << " source=" << sanitize_inline_text(row.resolution_source);
    if (!row.operation.empty())
      output << " operation=" << sanitize_inline_text(row.operation);
    if (!row.mode.empty())
      output << " mode=" << sanitize_inline_text(row.mode);
    if (!row.risk.empty())
      output << " risk=" << sanitize_inline_text(row.risk);
    if (!row.tool_name.empty())
      output << " tool=" << sanitize_inline_text(row.tool_name);
    if (!target.empty())
      output << " " << target;
    output << "\n";
    if (!row.reason.empty())
      output << "  reason: " << sanitize_inline_text(row.reason) << "\n";
    if (!row.resolution_reason.empty())
      output << "  resolution reason: " << sanitize_inline_text(row.resolution_reason) << "\n";
    if (!row.rule_id.empty())
    {
      output << "  rule: " << sanitize_inline_text(row.rule_id);
      if (rule)
      {
        output << " (" << ava::permissions::to_string(rule->action) << " " << ava::permissions::to_string(rule->operation)
               << " scope=" << ava::permissions::to_string(rule->scope) << " mode=" << ava::permissions::to_string(rule->mode) << ")";
      }
      output << "\n";
      output << "  next: /permissions explain " << sanitize_inline_text(row.rule_id) << "\n";
    }
    else if (!row.request_id.empty())
    {
      output << "  next: /permissions audit " << sanitize_inline_text(row.request_id) << "\n";
    }
  }

  output << "\nUse /permissions audit";
  if (!query.empty())
    output << " " << sanitize_inline_text(std::string(query));
  output << " for raw audit rows.";
  return output.str();
}

std::string format_permission_rules_list(ava::permissions::PermissionRuleStore const& store,
                                         std::vector<ava::permissions::PersistentPermissionRule> const& rules,
                                         runtime::session_ts const& unlocked_session,
                                         std::string_view query)
{
  auto const workspace_dir = runtime::session_ts::crat(unlocked_session)->workspace_dir();
  std::ostringstream output;
  output << "Permission rules:\n";
  output << "  global file: " << sanitize_inline_text(store.global_rules_file.generic_string()) << "\n";
  output << "  workspace file: "
         << sanitize_inline_text(ava::permissions::enforceable_permission_rules_file(store, ava::permissions::PermissionRuleScope::Workspace).generic_string())
         << "\n";
  output << "  behavior: built-in hard denies run first; matching deny rules override allow rules\n";
  output << "  display: human summaries with 1-based ordinals; ordinals are not remove authority\n";

  std::vector<ava::permissions::PersistentPermissionRule const*> matched;
  matched.reserve(rules.size());
  for (auto const& rule : rules)
  {
    if (rule_matches_query(rule, unlocked_session, query))
      matched.push_back(&rule);
  }

  if (!query.empty())
    output << "  filter: " << sanitize_inline_text(std::string(query)) << "\n";
  output << "\n";

  if (matched.empty())
  {
    if (query.empty())
      output << "No persistent permission rules.\n";
    else
      output << "No persistent permission rules match \"" << sanitize_inline_text(std::string(query)) << "\".\n";
  }
  else
  {
    for (std::size_t index = 0; index < matched.size(); ++index)
    {
      output << "  " << (index + 1) << ". " << format_permission_rule_summary_impl(*matched[index], workspace_dir) << "\n";
    }
  }

  output << "\nUse /permissions explain or /permissions remove, then choose or complete a rule.";
  return output.str();
}

std::string format_permission_rule_explain(ava::permissions::PermissionRuleStore const& store, ava::permissions::PersistentPermissionRule const& rule,
                                           runtime::session_ts const& unlocked_session)
{
  auto const workspace_dir = runtime::session_ts::crat(unlocked_session)->workspace_dir();
  std::ostringstream output;
  output << format_permission_rule_summary_impl(rule, workspace_dir) << "\n";
  output << "  rule id: " << sanitize_inline_text(rule.rule_id) << "\n";
  output << "  action: " << ava::permissions::to_string(rule.action) << "\n";
  output << "  operation: " << ava::permissions::to_string(rule.operation) << "\n";
  output << "  scope: " << ava::permissions::to_string(rule.scope) << "\n";
  output << "  mode: " << ava::permissions::to_string(rule.mode) << "\n";
  if (!rule.workspace_dir.empty())
    output << "  workspace: " << sanitize_inline_text(rule.workspace_dir.generic_string()) << "\n";
  if (!rule.target_path.empty())
    output << "  path: " << sanitize_inline_text(display_permission_path(rule.target_path, workspace_dir)) << "\n";
  if (!rule.command.empty())
    output << "  command: " << sanitize_inline_text(rule.command) << "\n";
  if (!rule.command_recipe_key.empty())
    output << "  recipe key: " << sanitize_inline_text(rule.command_recipe_key) << "\n";
  if (!rule.recipe_display.empty())
    output << "  recipe display: " << sanitize_inline_text(rule.recipe_display) << "\n";
  if (rule.critical_acknowledged)
    output << "  critical acknowledged: true (advanced exact-command allow)\n";
  output << "  schema version: " << rule.schema_version << "\n";
  if (!rule.tool_name.empty())
    output << "  tool: " << sanitize_inline_text(rule.tool_name) << "\n";
  output << "  reason: " << sanitize_inline_text(rule.reason) << "\n";
  output << "  actor: " << sanitize_inline_text(rule.actor) << "\n";
  output << "  created: " << sanitize_inline_text(rule.created_at) << "\n";
  output << "  storage: " << sanitize_inline_text(permission_rule_storage_path(store, rule)) << "\n";
  output << "  matching: exact operation plus any non-empty path, command, recipe key, tool, scope, and mode fields\n";
  output << "  precedence: built-in hard denies run first; matching deny rules override allow rules";
  return output.str();
}

ava::core::Result<std::vector<std::string>> parse_permission_command_tokens(std::string_view text)
{
  std::vector<std::string> tokens;
  std::size_t index = 0;
  while (index < text.size())
  {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) ++index;
    if (index >= text.size())
      break;

    std::string token;
    char quote = '\0';
    while (index < text.size())
    {
      char const ch = text[index++];
      if (quote != '\0')
      {
        if (ch == quote)
        {
          quote = '\0';
          continue;
        }
        if (ch == '\\' && index < text.size())
        {
          token.push_back(text[index++]);
          continue;
        }
        token.push_back(ch);
        continue;
      }

      if (ch == '"' || ch == '\'')
      {
        quote = ch;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        break;
      token.push_back(ch);
    }

    if (quote != '\0')
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission command has unterminated quoted value");
      return std::unexpected(std::move(error));
    }
    if (!token.empty())
      tokens.push_back(std::move(token));
  }
  return tokens;
}

std::string normalize_field_key(std::string_view key)
{
  auto normalized = lower_ascii(key);
  if (normalized == "target_path")
    return "path";
  if (normalized == "tool_name")
    return "tool";
  if (normalized == "command_recipe_key")
    return "recipe_key";
  return normalized;
}

ava::core::Result<ParsedAddRule> parse_permission_add_rule(std::vector<std::string> const& tokens)
{
  std::unordered_map<std::string, std::string> fields;
  for (std::size_t index = 1; index < tokens.size(); ++index)
  {
    auto const separator = tokens[index].find('=');
    if (separator == std::string::npos || separator == 0)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission rule add arguments must use key=value form");
      error.with_context("argument", tokens[index]);
      return std::unexpected(std::move(error));
    }

    auto key = normalize_field_key(std::string_view(tokens[index]).substr(0, separator));
    auto value = tokens[index].substr(separator + 1);
    if (key != "action" && key != "operation" && key != "scope" && key != "mode" && key != "path" && key != "command" && key != "recipe_key" &&
        key != "recipe_display" && key != "critical_acknowledged" && key != "tool" && key != "reason")
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission rule add argument is unsupported");
      error.with_context("argument", key);
      return std::unexpected(std::move(error));
    }
    if (!fields.emplace(key, std::move(value)).second)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission rule add argument is duplicated");
      error.with_context("argument", key);
      return std::unexpected(std::move(error));
    }
  }

  auto require_field = [&](std::string_view key) -> ava::core::Result<std::string> {
    auto const found = fields.find(std::string(key));
    if (found == fields.end() || found->second.empty())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission rule add argument is required");
      error.with_context("argument", std::string(key));
      return std::unexpected(std::move(error));
    }
    return found->second;
  };

  auto action_text = require_field("action");
  if (!action_text)
    return std::unexpected(std::move(action_text.error()));
  auto action = ava::permissions::parse_permission_action(*action_text);
  if (!action || *action == ava::permissions::PermissionAction::Ask)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission rule action must be allow or deny");
    error.with_context("action", *action_text);
    return std::unexpected(std::move(error));
  }

  auto operation_text = require_field("operation");
  if (!operation_text)
    return std::unexpected(std::move(operation_text.error()));
  auto operation = ava::permissions::parse_operation(*operation_text);
  if (!operation)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission rule operation is unsupported");
    error.with_context("operation", *operation_text);
    return std::unexpected(std::move(error));
  }

  auto reason = require_field("reason");
  if (!reason)
    return std::unexpected(std::move(reason.error()));

  auto scope = ava::permissions::PermissionRuleScope::Workspace;
  if (auto const found = fields.find("scope"); found != fields.end() && !found->second.empty())
  {
    auto parsed = ava::permissions::parse_permission_rule_scope(found->second);
    if (!parsed)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission rule scope must be workspace or global");
      error.with_context("scope", found->second);
      return std::unexpected(std::move(error));
    }
    scope = *parsed;
  }

  auto mode = ava::permissions::PermissionRuleMode::Any;
  if (auto const found = fields.find("mode"); found != fields.end() && !found->second.empty())
  {
    auto parsed = ava::permissions::parse_permission_rule_mode(found->second);
    if (!parsed)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission rule mode must be any, build, or plan");
      error.with_context("mode", found->second);
      return std::unexpected(std::move(error));
    }
    mode = *parsed;
  }

  auto field_value = [&](std::string_view key) -> std::string {
    auto const found = fields.find(std::string(key));
    if (found == fields.end())
      return "";
    return found->second;
  };
  if (auto const acknowledged = fields.find("critical_acknowledged");
      acknowledged != fields.end() && acknowledged->second != "true" && acknowledged->second != "false")
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "critical_acknowledged must be true or false");
    error.with_context("critical_acknowledged", acknowledged->second);
    return std::unexpected(std::move(error));
  }

  return ParsedAddRule{.draft = ava::permissions::PermissionRuleDraft{.scope = scope,
                                                                      .action = *action,
                                                                      .operation = *operation,
                                                                      .mode = mode,
                                                                      .tool_name = field_value("tool"),
                                                                      .target_path = field_value("path"),
                                                                      .command = field_value("command"),
                                                                      .command_recipe_key = field_value("recipe_key"),
                                                                      .recipe_display = field_value("recipe_display"),
                                                                      .critical_acknowledged = field_value("critical_acknowledged") == "true",
                                                                      .reason = *reason,
                                                                      .actor = "tui"}};
}

ava::core::Result<std::vector<ava::session::SessionEntry>> load_permission_history(runtime::session_ts const& unlocked_session)
{
  auto read_authority = runtime::session_ts::crat(unlocked_session)->read_authority_1();
  if (!read_authority)
    return std::unexpected(std::move(read_authority.error()));
  return read_authority->load();
}

ava::permissions::PersistentPermissionRule const* find_rule(std::vector<ava::permissions::PersistentPermissionRule> const& rules, std::string_view rule_id)
{
  auto const found = std::ranges::find_if(rules, [&](auto const& rule) { return rule.rule_id == rule_id; });
  if (found == rules.end())
    return nullptr;
  return &*found;
}

}  // namespace

std::string format_permission_rule_summary(ava::permissions::PersistentPermissionRule const& rule, std::filesystem::path const& workspace_dir)
{
  return format_permission_rule_summary_impl(rule, workspace_dir);
}

// Run the permission-rule command in request against unlocked_session and return display-ready output.
//
// Session state is only read. Add and remove commands can update the persistent permission-rule files.
ava::core::Result<CommandResult> run_permissions_command(runtime::session_ts& unlocked_session, CommandRequest const& request)
{
  CommandResult result;
  result.handled = true;

  auto const argument = trim_ascii(command_argument(request.command, "/permissions"));
  auto tokens = parse_permission_command_tokens(argument);
  if (!tokens)
  {
    add_output(result, tokens.error().format());
    return result;
  }
  if (tokens->empty())
    tokens->push_back("list");

  auto const subcommand = lower_ascii((*tokens)[0]);
  auto const store = runtime::session_ts::rat(unlocked_session)->permission_rule_store();

  if (subcommand == "list")
  {
    auto rules = ava::permissions::load_persistent_permission_rules(store);
    if (!rules)
    {
      add_output(result, rules.error().format());
      return result;
    }
    auto query = std::string{};
    if (argument.size() > (*tokens)[0].size())
      query = trim_ascii(argument.substr((*tokens)[0].size()));
    add_output(result, format_permission_rules_list(store, *rules, unlocked_session, query));
    return result;
  }

  if (subcommand == "audit" || subcommand == "history")
  {
    auto entries = load_permission_history(unlocked_session);
    if (!entries)
    {
      add_output(result, "Permission audit failed closed:\n" + entries.error().format());
      return result;
    }
    auto audit_action = tokens->size() > 1 ? lower_ascii((*tokens)[1]) : std::string{};
    if (audit_action == "show" || audit_action == "inspect" || audit_action == "detail")
    {
      if (tokens->size() < 3)
      {
        add_output(result, missing_argument("/permissions audit show <entry_id|request_id>"));
        return result;
      }
      auto selector_tokens = std::vector<std::string>{};
      for (auto index = std::size_t{2}; index < tokens->size(); ++index)
      {
        selector_tokens.push_back((*tokens)[index]);
      }
      add_output(result, format_permission_audit_detail(*entries, unlocked_session, joined_strings(selector_tokens, " ")));
      return result;
    }
    auto export_markdown = audit_action == "export";
    auto summarize = audit_action == "summary" || audit_action == "summarize" || audit_action == "stats";
    auto query_tokens = std::vector<std::string>{};
    auto const first_query_token = (export_markdown || summarize) ? std::size_t{2} : std::size_t{1};
    for (auto index = first_query_token; index < tokens->size(); ++index)
    {
      query_tokens.push_back((*tokens)[index]);
    }
    auto const query = joined_strings(query_tokens, " ");
    if (summarize)
      add_output(result, format_permission_audit_summary(*entries, unlocked_session, query));
    else
      add_output(result,
                 export_markdown ? format_permission_audit_export(*entries, unlocked_session, query)
                                 : format_permission_audit(*entries, unlocked_session, query));
    return result;
  }

  if (subcommand == "diagnose" || subcommand == "diagnostics")
  {
    auto rules = ava::permissions::load_persistent_permission_rules(store);
    if (!rules)
    {
      add_output(result, "Permission rule diagnostics failed closed:\n" + rules.error().format());
      return result;
    }
    auto entries = load_permission_history(unlocked_session);
    if (!entries)
    {
      add_output(result, "Permission audit diagnostics failed closed:\n" + entries.error().format());
      return result;
    }
    auto query_tokens = std::vector<std::string>{};
    for (auto index = std::size_t{1}; index < tokens->size(); ++index)
    {
      query_tokens.push_back((*tokens)[index]);
    }
    auto const query = joined_strings(query_tokens, " ");
    std::ostringstream output;
    output << "Permission rule diagnostics:\n";
    output << "  global file: " << sanitize_inline_text(store.global_rules_file.generic_string()) << "\n";
    output << "  workspace file: "
           << sanitize_inline_text(
                  ava::permissions::enforceable_permission_rules_file(store, ava::permissions::PermissionRuleScope::Workspace).generic_string())
           << "\n";
    output << "  loaded rules: " << rules->size() << "\n";
    output << "  hard-deny policy: evaluated before persistent rules\n";
    output << "  precedence: matching deny rules override allow rules\n";
    output << "  command allows: schema-v2 exact stable recipe keys; v1 allows are non-authoritative; Critical exact allows require acknowledgement\n";
    output << "  workspace storage: outside the model-writable workspace";
    output << "\n\n" << format_permission_denial_diagnostics(*entries, unlocked_session, *rules, query);
    add_output(result, output.str());
    return result;
  }

  if (subcommand == "explain" || subcommand == "show")
  {
    if (tokens->size() != 2)
    {
      add_output(result, missing_argument("/permissions explain <rule_id>"));
      return result;
    }
    if (looks_like_display_ordinal((*tokens)[1]))
    {
      add_output(result, "permission rule explain requires the full rule id; list ordinals are display-only");
      return result;
    }
    auto rules = ava::permissions::load_persistent_permission_rules(store);
    if (!rules)
    {
      add_output(result, rules.error().format());
      return result;
    }
    auto const* rule = find_rule(*rules, (*tokens)[1]);
    if (!rule)
    {
      add_output(result, "permission rule not found: " + sanitize_inline_text((*tokens)[1]));
      return result;
    }
    add_output(result, format_permission_rule_explain(store, *rule, unlocked_session));
    return result;
  }

  if (subcommand == "add")
  {
    if (tokens->size() == 1)
    {
      add_output(result, permissions_usage());
      return result;
    }
    auto parsed = parse_permission_add_rule(*tokens);
    if (!parsed)
    {
      add_output(result, parsed.error().format() + "\n" + permissions_usage());
      return result;
    }
    auto added = ava::permissions::add_persistent_permission_rule(store, std::move(parsed->draft));
    if (!added)
    {
      add_output(result, added.error().format());
      return result;
    }
    add_output(result, format_permission_rule_receipt(*added, unlocked_session, "added"));
    return result;
  }

  if (subcommand == "remove" || subcommand == "delete")
  {
    if (tokens->size() != 2)
    {
      add_output(result, missing_argument("/permissions remove <rule_id>"));
      return result;
    }
    if (looks_like_display_ordinal((*tokens)[1]))
    {
      add_output(result, "permission rule remove requires the full rule id; list ordinals are display-only");
      return result;
    }
    auto removed = ava::permissions::remove_persistent_permission_rule(store, (*tokens)[1]);
    if (!removed)
    {
      add_output(result, removed.error().format());
      return result;
    }
    add_output(result, format_permission_rule_receipt(*removed, unlocked_session, "removed"));
    return result;
  }

  add_output(result, permissions_usage());
  return result;
}

}  // namespace ava::app
