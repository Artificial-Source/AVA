#include "sys.h"
#include "ava/session/assistant_output.h"
#include "ava/session/record.h"
#include "ava/session/session_metadata.h"
#include "ava/session/subagent_job_history.h"
#include "ava/session/validation.h"
#include "ava/session/validation_fields.h"
#include "ava/core/json.h"
#include "ava/core/openai_wire.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ava::session {
namespace {

struct ToolCallState
{
  std::string entry_id;
  std::string tool_name;
  // Non-empty only for a function_call carried by a valid committed v4
  // assistant-output group. It makes tool-result pairing exact rather than
  // relying only on a provider logical call ID.
  std::string assistant_output_entry_id;
  bool result_seen = false;
};

struct PendingPermissionPrompt
{
  std::string entry_id;
  std::size_t entry_index = 0;
};

struct ActiveModelState
{
  std::string provider_id;
  std::string model_id;
};

constexpr std::size_t kMaxSessionNameBytes = 256;
constexpr std::size_t kMaxSessionLabels = 32;
constexpr std::size_t kMaxSessionLabelBytes = 64;
constexpr std::size_t kMaxBranchSummaryBytes = 8192;
constexpr std::size_t kMaxToolResultSummaryBytes = 8192;
constexpr std::size_t kMaxToolResultShortStringBytes = 4096;
constexpr std::size_t kMaxToolResultTextBytes = 512 * 1024;
constexpr std::size_t kMaxToolResultPaths = 4096;
constexpr std::size_t kMaxToolResultPathBytes = 4096;
constexpr std::size_t kMaxToolResultPermissionIds = 4096;
constexpr std::size_t kMaxToolResultPermissionIdBytes = 256;
constexpr std::size_t kMaxMessageAttachments = 16;
constexpr std::size_t kMaxImageAttachmentIdBytes = 128;
constexpr std::size_t kMaxImageStoragePathBytes = 4096;
constexpr std::size_t kMaxImageBytes = 20 * 1024 * 1024;

bool valid_optional_string_array(std::string_view object, std::string_view key, std::size_t max_count, std::size_t max_item_bytes);

bool is_supported_image_attachment_mime_type(std::string_view mime_type)
{
  return mime_type == "image/png" || mime_type == "image/jpeg" || mime_type == "image/webp" || mime_type == "image/gif";
}
bool schema_version_is_current(std::string_view object);

std::string permission_key(SessionEntry const& entry)
{
  auto const permission_request_id = ava::core::json::string_field(entry.data_json, "permission_request_id");
  if (permission_request_id && !permission_request_id->empty())
  {
    return "id:" + *permission_request_id;
  }

  std::string key = ava::core::json::string_field(entry.data_json, "operation").value_or("");
  key += '\x1F';
  key += ava::core::json::string_field(entry.data_json, "tool_name").value_or("");
  key += '\x1F';
  key += ava::core::json::string_field(entry.data_json, "target_path").value_or("");
  key += '\x1F';
  key += ava::core::json::string_field(entry.data_json, "command").value_or("");
  key += '\x1F';
  key += ava::core::json::string_field(entry.data_json, "reason").value_or("");
  return key;
}

void add_issue(SessionReplayValidation& validation, SessionReplayIssue issue)
{
  if (issue.severity == SessionReplayIssueSeverity::Warning)
  {
    ++validation.warning_count;
  }
  else
  {
    ++validation.error_count;
  }
  validation.issues.push_back(std::move(issue));
}

void add_error(SessionReplayValidation& validation, SessionReplayIssueKind kind, std::size_t index, SessionEntry const& entry, std::string call_id,
               std::string message)
{
  add_issue(validation, SessionReplayIssue{.severity = SessionReplayIssueSeverity::Error,
                                           .kind = kind,
                                           .entry_index = index,
                                           .entry_id = entry.id,
                                           .call_id = std::move(call_id),
                                           .message = std::move(message)});
}

void add_assistant_output_diagnostics(SessionReplayValidation& validation, AssistantOutputProjection const& projection)
{
  for (auto const& diagnostic : projection.diagnostics)
  {
    SessionReplayIssueKind kind = SessionReplayIssueKind::MalformedAssistantTurn;
    switch (diagnostic.kind)
    {
      case AssistantOutputDiagnosticKind::InvalidAssistantOutputItem:
        kind = SessionReplayIssueKind::InvalidAssistantOutputItem;
        break;
      case AssistantOutputDiagnosticKind::InvalidAssistantTurnCommit:
        kind = SessionReplayIssueKind::InvalidAssistantTurnCommit;
        break;
      case AssistantOutputDiagnosticKind::IncompleteAssistantTurn:
        kind = SessionReplayIssueKind::IncompleteAssistantTurn;
        break;
      case AssistantOutputDiagnosticKind::MalformedAssistantTurn:
        kind = SessionReplayIssueKind::MalformedAssistantTurn;
        break;
    }
    add_issue(validation, SessionReplayIssue{.severity = diagnostic.severity == AssistantOutputDiagnosticSeverity::Warning ? SessionReplayIssueSeverity::Warning
                                                                                                                           : SessionReplayIssueSeverity::Error,
                                             .kind = kind,
                                             .entry_index = diagnostic.entry_index,
                                             .entry_id = diagnostic.entry_id,
                                             .call_id = "",
                                             .message = diagnostic.message});
  }
}

void register_committed_assistant_output_calls(SessionReplayValidation& validation, CommittedAssistantTurn const& turn,
                                               std::unordered_map<std::string, ToolCallState>& tool_calls)
{
  for (auto const& output_item : turn.items)
  {
    auto const* function = std::get_if<AssistantOutputFunctionCall>(&output_item.item.payload);
    if (!function)
      continue;
    auto const existing = tool_calls.find(function->call_id);
    if (existing != tool_calls.end())
    {
      add_error(
          validation, SessionReplayIssueKind::DuplicateToolCallId, output_item.entry_index,
          SessionEntry{.id = output_item.entry_id, .parent_id = "", .type = EntryType::AssistantOutputItem, .timestamp = "", .data_json = "{}", .version = 4},
          function->call_id, "tool_call id is reused in the same session");
      continue;
    }
    tool_calls.emplace(
        function->call_id,
        ToolCallState{.entry_id = output_item.entry_id, .tool_name = function->name, .assistant_output_entry_id = output_item.entry_id, .result_seen = false});
  }
}

bool tool_result_matches_committed_output_item(SessionReplayValidation& validation, AssistantOutputProjection const& projection,
                                               std::vector<SessionEntry> const& entries, std::size_t index, SessionEntry const& entry, std::string_view call_id,
                                               std::string_view tool_name, ToolCallState const* registered_call)
{
  auto const binding_present = ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id");
  auto const output_entry_id = ava::core::json::string_field(entry.data_json, "assistant_output_entry_id");
  bool const requires_output_binding = registered_call && !registered_call->assistant_output_entry_id.empty();
  if (!requires_output_binding && !binding_present)
    return true;

  auto const* committed_turn = output_entry_id ? projection.find_turn_by_output_entry_id(*output_entry_id) : nullptr;
  auto const* output_item = output_entry_id ? projection.find_item_by_output_entry_id(*output_entry_id) : nullptr;
  auto const* function = output_item ? std::get_if<AssistantOutputFunctionCall>(&output_item->item.payload) : nullptr;
  auto const result_window = committed_turn ? committed_assistant_output_tool_result_window(entries, *committed_turn) : AssistantOutputToolResultWindow{};
  if (!output_entry_id || output_entry_id->empty() || !committed_turn || !output_item || !function || !result_window.contains(index) ||
      output_item->entry_index >= index || function->call_id != call_id || function->name != tool_name ||
      (registered_call && registered_call->assistant_output_entry_id != *output_entry_id))
  {
    add_error(validation, SessionReplayIssueKind::ToolResultOutputItemMismatch, index, entry, std::string(call_id),
              "tool_result must bind to a committed function output item with matching call_id and name in its immediate post-commit result window");
    return false;
  }
  return true;
}

bool valid_optional_string_field(std::string_view object, std::string_view key, std::size_t max_bytes, bool allow_empty = true)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return true;
  auto const value = ava::core::json::string_field(object, key);
  return value && (allow_empty || !value->empty()) && value->size() <= max_bytes;
}

bool valid_required_structured_content(std::string_view object, std::string_view content_type)
{
  auto const start = ava::core::json::field_value_start(object, "content");
  if (!start)
    return false;
  if (content_type == "application/json")
  {
    auto const content = ava::core::json::object_field(object, "content");
    return content && ava::core::json::is_valid_object(*content);
  }
  return ava::core::json::string_field(object, "content").has_value();
}

bool valid_optional_structured_tool_metadata(std::string_view object)
{
  if (!valid_optional_string_field(object, "summary", kMaxToolResultSummaryBytes) ||
      !valid_optional_string_field(object, "content_type", kMaxToolResultShortStringBytes, false) ||
      !valid_optional_string_field(object, "diff", kMaxToolResultTextBytes) || !valid_optional_string_field(object, "spill_path", kMaxToolResultPathBytes) ||
      !valid_optional_string_array(object, "changed_paths", kMaxToolResultPaths, kMaxToolResultPathBytes) ||
      !valid_optional_string_array(object, "permission_request_ids", kMaxToolResultPermissionIds, kMaxToolResultPermissionIdBytes))
  {
    return false;
  }
  for (std::string_view key : {"diff_truncated", "truncated", "byte_limited", "line_limited", "spill_truncated"})
  {
    if (!present_boolean(object, key))
      return false;
  }
  for (std::string_view key : {"output_bytes", "total_bytes", "output_lines", "total_lines", "start_line", "end_line", "next_offset_line", "omitted_bytes",
                               "omitted_lines", "visible_matches", "total_matches"})
  {
    if (!present_integer_matching(object, key, false))
      return false;
  }
  return true;
}

bool valid_structured_tool_error(std::string_view object, std::string_view status)
{
  auto const error_start = ava::core::json::field_value_start(object, "error");
  if (!error_start)
    return status == "success";
  auto const error = ava::core::json::object_field(object, "error");
  if (!error || !ava::core::json::is_valid_object(*error))
    return false;
  if (!valid_optional_string_field(*error, "category", kMaxToolResultShortStringBytes) ||
      !valid_optional_string_field(*error, "code", kMaxToolResultShortStringBytes) ||
      !valid_optional_string_field(*error, "message", kMaxToolResultSummaryBytes) || !valid_optional_string_field(*error, "details", kMaxToolResultTextBytes))
  {
    return false;
  }
  if (status != "success")
  {
    auto const message = ava::core::json::string_field(*error, "message");
    return message && !message->empty();
  }
  return true;
}

void validate_structured_tool_result(SessionReplayValidation& validation, std::size_t index, SessionEntry const& entry, std::string_view call_id,
                                     std::string_view tool_name)
{
  auto const structured = ava::core::json::object_field(entry.data_json, "structured_result");
  if (!structured)
  {
    add_error(validation, SessionReplayIssueKind::MissingStructuredToolResult, index, entry, std::string(call_id),
              "tool_result entry is missing structured_result");
    return;
  }
  if (!ava::core::json::is_valid_object(*structured))
  {
    add_error(validation, SessionReplayIssueKind::InvalidStructuredToolResult, index, entry, std::string(call_id),
              "tool_result structured_result is not valid JSON");
    return;
  }
  if (!schema_version_is_current(*structured))
  {
    add_error(validation, SessionReplayIssueKind::InvalidStructuredToolResult, index, entry, std::string(call_id),
              "tool_result structured_result has an unsupported schema_version");
    return;
  }

  auto const structured_call_id = ava::core::json::string_field(*structured, "call_id").value_or("");
  auto const structured_tool = ava::core::json::string_field(*structured, "tool").value_or("");
  auto const status = ava::core::json::string_field(*structured, "status").value_or("");
  auto const content_type = ava::core::json::string_field(*structured, "content_type").value_or("");
  if (structured_call_id.empty() || structured_tool.empty() || status.empty() || content_type.empty() || !valid_status(status) ||
      !required_boolean(*structured, "ok") || !valid_required_structured_content(*structured, content_type) ||
      !valid_optional_structured_tool_metadata(*structured) || !valid_structured_tool_error(*structured, status))
  {
    add_error(validation, SessionReplayIssueKind::InvalidStructuredToolResult, index, entry, std::string(call_id),
              "tool_result structured_result is missing required semantic fields");
    return;
  }
  if (structured_call_id != call_id || structured_tool != tool_name)
  {
    add_error(validation, SessionReplayIssueKind::StructuredToolResultMismatch, index, entry, std::string(call_id),
              "tool_result structured_result does not match top-level call_id/name");
    return;
  }

  auto const top_status = ava::core::json::string_field(entry.data_json, "status").value_or("");
  if (!top_status.empty() && top_status != status)
  {
    add_error(validation, SessionReplayIssueKind::StructuredToolResultMismatch, index, entry, std::string(call_id),
              "tool_result structured_result status does not match top-level status");
    return;
  }
  bool const ok = bool_field_is_true(*structured, "ok");
  if ((status == "success") != ok)
  {
    add_error(validation, SessionReplayIssueKind::StructuredToolResultMismatch, index, entry, std::string(call_id),
              "tool_result structured_result ok flag does not match status");
    return;
  }
  if (bool_field_is_true(entry.data_json, "success") && !ok)
  {
    add_error(validation, SessionReplayIssueKind::StructuredToolResultMismatch, index, entry, std::string(call_id),
              "successful tool_result has non-success structured_result status");
    return;
  }
  if (bool_field_is_false(entry.data_json, "success") && ok)
  {
    add_error(validation, SessionReplayIssueKind::StructuredToolResultMismatch, index, entry, std::string(call_id),
              "failed tool_result has success structured_result status");
  }
}

void validate_permission_decision(SessionReplayValidation& validation, std::unordered_map<std::string, std::vector<PendingPermissionPrompt>>& pending,
                                  std::size_t index, SessionEntry const& entry)
{
  auto const operation = ava::core::json::string_field(entry.data_json, "operation").value_or("");
  auto const mode = ava::core::json::string_field(entry.data_json, "mode").value_or("");
  auto const tool_name = ava::core::json::string_field(entry.data_json, "tool_name").value_or("");
  auto const action = ava::core::json::string_field(entry.data_json, "action").value_or("");
  auto const reason = ava::core::json::string_field(entry.data_json, "reason").value_or("");
  auto const risk = ava::core::json::string_field(entry.data_json, "risk");
  auto const resolution = ava::core::json::string_field(entry.data_json, "resolution").value_or("");
  auto const resolution_source = ava::core::json::string_field(entry.data_json, "resolution_source").value_or("");

  if (!valid_operation(operation) || !valid_mode(mode) || tool_name.empty() || !valid_action(action) || reason.empty() ||
      !present_non_empty_string(entry.data_json, "permission_request_id"))
  {
    add_error(validation, SessionReplayIssueKind::InvalidPermissionDecision, index, entry, "", "permission_decision entry is missing required semantic fields");
    return;
  }
  if (risk && !valid_risk(*risk))
  {
    add_error(validation, SessionReplayIssueKind::InvalidPermissionDecision, index, entry, "", "permission_decision entry has an invalid risk");
    return;
  }
  if (!resolution.empty() && !valid_resolution(resolution))
  {
    add_error(validation, SessionReplayIssueKind::InvalidPermissionDecision, index, entry, "", "permission_decision entry has an invalid resolution");
    return;
  }
  if (!resolution_source.empty() && !valid_resolution_source(resolution_source))
  {
    add_error(validation, SessionReplayIssueKind::InvalidPermissionDecision, index, entry, "", "permission_decision entry has an invalid resolution_source");
    return;
  }

  if (action == "allow" || action == "deny")
  {
    if (resolution != action || resolution_source != "policy")
    {
      add_error(validation, SessionReplayIssueKind::InvalidPermissionDecision, index, entry, "",
                "policy allow/deny permission_decision must resolve to its action from policy");
    }
    return;
  }

  if (resolution.empty())
  {
    if (resolution_source != "policy")
    {
      add_error(validation, SessionReplayIssueKind::InvalidPermissionDecision, index, entry, "",
                "ask permission_decision without resolution must come from policy");
      return;
    }
    pending[permission_key(entry)].push_back(PendingPermissionPrompt{.entry_id = entry.id, .entry_index = index});
    return;
  }

  if (resolution_source == "policy" || resolution_source.empty())
  {
    add_error(validation, SessionReplayIssueKind::InvalidPermissionDecision, index, entry, "",
              "resolved ask permission_decision must include a resolver outcome source");
    return;
  }

  auto pending_for_key = pending.find(permission_key(entry));
  if (pending_for_key == pending.end() || pending_for_key->second.empty())
  {
    add_error(validation, SessionReplayIssueKind::PermissionResolutionWithoutAsk, index, entry, "",
              "resolved ask permission_decision has no earlier matching ask prompt");
    return;
  }
  pending_for_key->second.pop_back();
  if (pending_for_key->second.empty())
    pending.erase(pending_for_key);
}

void validate_compaction_entry(SessionReplayValidation& validation, std::size_t index, SessionEntry const& entry)
{
  if (!ava::core::json::is_valid_object(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidCompactionEntry, index, entry, "", "compaction entry data is not valid JSON");
    return;
  }

  auto const summary = ava::core::json::string_field(entry.data_json, "summary");
  if (!summary || summary->empty())
  {
    add_error(validation, SessionReplayIssueKind::InvalidCompactionEntry, index, entry, "", "compaction entry is missing a non-empty summary");
    return;
  }

  auto const unavailable_present = ava::core::json::field_value_start(entry.data_json, "summary_unavailable");
  if (unavailable_present && !bool_field_is_true(entry.data_json, "summary_unavailable") && !bool_field_is_false(entry.data_json, "summary_unavailable"))
  {
    add_error(validation, SessionReplayIssueKind::InvalidCompactionEntry, index, entry, "", "compaction entry summary_unavailable must be a boolean");
    return;
  }

  auto const status_present = ava::core::json::field_value_start(entry.data_json, "status");
  auto const status = ava::core::json::string_field(entry.data_json, "status");
  if (status_present && (!status || *status != "recorded"))
  {
    add_error(validation, SessionReplayIssueKind::InvalidCompactionEntry, index, entry, "", "compaction entry status must be recorded when present");
    return;
  }

  auto const projection_present = ava::core::json::field_value_start(entry.data_json, "history_projection");
  auto const history_projection = ava::core::json::string_field(entry.data_json, "history_projection");
  if (projection_present && (!history_projection || *history_projection != "portable-v1"))
  {
    add_error(validation, SessionReplayIssueKind::InvalidCompactionEntry, index, entry, "",
              "compaction entry history_projection must be portable-v1 when present");
    return;
  }

  auto const reason = ava::core::json::string_field(entry.data_json, "reason");
  auto const threshold_type = ava::core::json::string_field(entry.data_json, "threshold_type");
  auto const retry_outcome = ava::core::json::string_field(entry.data_json, "overflow_retry_outcome");
  if (!present_non_empty_string(entry.data_json, "trigger") || !present_non_empty_string(entry.data_json, "provider") ||
      !present_non_empty_string(entry.data_json, "model") || !present_non_empty_string(entry.data_json, "reason") ||
      (reason && *reason != "manual" && *reason != "automatic" && *reason != "overflow") || !present_non_empty_string(entry.data_json, "threshold_type") ||
      (threshold_type && *threshold_type != "tokens" && *threshold_type != "percent") || !present_non_empty_string(entry.data_json, "overflow_retry_outcome") ||
      (retry_outcome && *retry_outcome != "scheduled" && *retry_outcome != "succeeded" && *retry_outcome != "failed") ||
      !present_integer_matching(entry.data_json, "threshold_tokens", false) ||
      !present_integer_matching(entry.data_json, "configured_threshold_tokens", false) ||
      !present_integer_matching(entry.data_json, "configured_threshold_percent", false) ||
      !present_integer_matching(entry.data_json, "estimated_tokens", false) ||
      !present_integer_matching(entry.data_json, "active_pre_compaction_tokens", false) ||
      !present_integer_matching(entry.data_json, "retained_tokens", false) ||
      !present_integer_matching(entry.data_json, "post_compaction_estimated_tokens", false) ||
      !present_integer_matching(entry.data_json, "keep_recent_tokens", false) || !present_integer_matching(entry.data_json, "keep_recent_turns", false) ||
      !present_integer_matching(entry.data_json, "keep_recent_messages", false) || !present_boolean(entry.data_json, "recent_context_omitted") ||
      !present_integer_matching(entry.data_json, "max_summary_bytes", true))
  {
    add_error(validation, SessionReplayIssueKind::InvalidCompactionEntry, index, entry, "", "compaction entry has malformed semantic metadata");
  }
}

void skip_json_ws(std::string_view text, std::size_t& index)
{
  while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0)
    ++index;
}

bool string_has_control_byte(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7F;
  });
}

bool hex_string(std::string_view value)
{
  return std::ranges::all_of(value, [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); });
}

std::optional<std::size_t> json_string_end(std::string_view text, std::size_t start)
{
  if (start >= text.size() || text[start] != '"')
    return std::nullopt;
  bool escaped = false;
  for (std::size_t index = start + 1; index < text.size(); ++index)
  {
    char const ch = text[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\')
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
      return index;
  }
  return std::nullopt;
}

std::optional<std::size_t> json_balanced_end(std::string_view text, std::size_t start, char open, char close)
{
  if (start >= text.size() || text[start] != open)
    return std::nullopt;
  bool in_string = false;
  bool escaped = false;
  std::size_t depth = 0;
  for (std::size_t index = start; index < text.size(); ++index)
  {
    char const ch = text[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\' && in_string)
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
    {
      in_string = !in_string;
      continue;
    }
    if (in_string)
      continue;
    if (ch == open)
    {
      ++depth;
    }
    else if (ch == close)
    {
      if (depth == 0)
        return std::nullopt;
      --depth;
      if (depth == 0)
        return index;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> json_value_end(std::string_view text, std::size_t start)
{
  skip_json_ws(text, start);
  if (start >= text.size())
    return std::nullopt;
  if (text[start] == '"')
    return json_string_end(text, start);
  if (text[start] == '{')
    return json_balanced_end(text, start, '{', '}');
  if (text[start] == '[')
    return json_balanced_end(text, start, '[', ']');
  auto end = start;
  while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != ']')
    ++end;
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
    --end;
  return end == start ? std::nullopt : std::optional<std::size_t>(end - 1);
}

bool allowed_image_attachment_key(std::string_view key)
{
  constexpr std::array<std::string_view, 7> kAllowedKeys = {"id", "type", "mime_type", "byte_size", "sha256", "storage_path", "redacted"};
  return std::find(kAllowedKeys.begin(), kAllowedKeys.end(), key) != kAllowedKeys.end();
}

bool object_has_duplicate_top_level_keys(std::string_view object)
{
  std::unordered_set<std::string> seen_keys;
  std::size_t cursor = 0;
  skip_json_ws(object, cursor);
  if (cursor >= object.size() || object[cursor] != '{')
    return true;
  ++cursor;
  skip_json_ws(object, cursor);
  if (cursor < object.size() && object[cursor] == '}')
    return false;

  while (cursor < object.size())
  {
    skip_json_ws(object, cursor);
    auto const key_end = json_string_end(object, cursor);
    if (!key_end)
      return true;
    auto const key = object.substr(cursor + 1, *key_end - cursor - 1);
    if (key.find('\\') != std::string_view::npos)
      return true;
    if (!seen_keys.insert(std::string(key)).second)
      return true;
    cursor = *key_end + 1;
    skip_json_ws(object, cursor);
    if (cursor >= object.size() || object[cursor] != ':')
      return true;
    ++cursor;
    auto const value_end = json_value_end(object, cursor);
    if (!value_end)
      return true;
    cursor = *value_end + 1;
    skip_json_ws(object, cursor);
    if (cursor < object.size() && object[cursor] == ',')
    {
      ++cursor;
      continue;
    }
    if (cursor < object.size() && object[cursor] == '}')
      return false;
    return true;
  }
  return true;
}

bool object_has_only_allowed_image_attachment_keys(std::string_view object)
{
  std::unordered_set<std::string> seen_keys;
  std::size_t cursor = 0;
  skip_json_ws(object, cursor);
  if (cursor >= object.size() || object[cursor] != '{')
    return false;
  ++cursor;
  skip_json_ws(object, cursor);
  if (cursor < object.size() && object[cursor] == '}')
    return true;

  while (cursor < object.size())
  {
    skip_json_ws(object, cursor);
    auto const key_end = json_string_end(object, cursor);
    if (!key_end)
      return false;
    auto const key = object.substr(cursor + 1, *key_end - cursor - 1);
    if (key.find('\\') != std::string_view::npos || !allowed_image_attachment_key(key))
      return false;
    if (!seen_keys.insert(std::string(key)).second)
      return false;
    cursor = *key_end + 1;
    skip_json_ws(object, cursor);
    if (cursor >= object.size() || object[cursor] != ':')
      return false;
    ++cursor;
    auto const value_end = json_value_end(object, cursor);
    if (!value_end)
      return false;
    cursor = *value_end + 1;
    skip_json_ws(object, cursor);
    if (cursor < object.size() && object[cursor] == ',')
    {
      ++cursor;
      continue;
    }
    if (cursor < object.size() && object[cursor] == '}')
      return true;
    return false;
  }
  return false;
}

bool valid_attachment_storage_path(std::string_view path)
{
  if (path.empty() || path.size() > kMaxImageStoragePathBytes || string_has_control_byte(path))
    return false;
  if (!path.starts_with("attachments/"))
    return false;
  if (path.starts_with('/') || path.starts_with('~') || path.find('\\') != std::string_view::npos)
    return false;
  if (path.find(":") != std::string_view::npos)
    return false;
  std::size_t segment_start = 0;
  while (segment_start <= path.size())
  {
    auto const slash = path.find('/', segment_start);
    auto const segment = path.substr(segment_start, slash == std::string_view::npos ? std::string_view::npos : slash - segment_start);
    if (segment.empty() || segment == "." || segment == "..")
      return false;
    if (slash == std::string_view::npos)
      break;
    segment_start = slash + 1;
  }
  return true;
}

std::optional<long long> strict_positive_integer_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || *start >= object.size() || !std::isdigit(static_cast<unsigned char>(object[*start])))
    return std::nullopt;
  unsigned long long value = 0;
  auto cursor = *start;
  while (cursor < object.size() && std::isdigit(static_cast<unsigned char>(object[cursor])))
  {
    auto const digit = static_cast<unsigned long long>(object[cursor] - '0');
    if (value > (static_cast<unsigned long long>(kMaxImageBytes) - digit) / 10ULL)
      return std::nullopt;
    value = value * 10ULL + digit;
    ++cursor;
  }
  skip_json_ws(object, cursor);
  if (cursor >= object.size() || (object[cursor] != ',' && object[cursor] != '}'))
    return std::nullopt;
  if (value == 0 || value > static_cast<unsigned long long>(kMaxImageBytes))
    return std::nullopt;
  return static_cast<long long>(value);
}

std::optional<std::vector<std::string>> image_attachment_objects(std::string_view object, std::string_view key)
{
  std::vector<std::string> result;
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || *start >= object.size() || object[*start] != '[')
    return std::nullopt;
  auto const end = json_balanced_end(object, *start, '[', ']');
  if (!end)
    return std::nullopt;
  std::string_view const array = object.substr(*start, *end - *start + 1);
  std::size_t cursor = 1;
  skip_json_ws(array, cursor);
  if (cursor < array.size() && array[cursor] == ']')
    return result;
  while (cursor + 1 < array.size())
  {
    skip_json_ws(array, cursor);
    if (cursor >= array.size() || array[cursor] != '{')
      return std::nullopt;
    auto const object_end = json_balanced_end(array, cursor, '{', '}');
    if (!object_end)
      return std::nullopt;
    result.emplace_back(array.substr(cursor, *object_end - cursor + 1));
    cursor = *object_end + 1;
    skip_json_ws(array, cursor);
    if (cursor < array.size() && array[cursor] == ',')
    {
      ++cursor;
      continue;
    }
    if (cursor < array.size() && array[cursor] == ']')
      return result;
    return std::nullopt;
  }
  return std::nullopt;
}

bool valid_image_attachment_object(std::string_view attachment)
{
  if (!ava::core::json::is_valid_object(attachment))
    return false;
  if (!object_has_only_allowed_image_attachment_keys(attachment))
    return false;

  auto const type = ava::core::json::string_field(attachment, "type");
  auto const id = ava::core::json::string_field(attachment, "id");
  auto const mime_type = ava::core::json::string_field(attachment, "mime_type");
  auto const storage_path = ava::core::json::string_field(attachment, "storage_path");
  auto const sha256 = ava::core::json::string_field(attachment, "sha256");
  auto const byte_size = strict_positive_integer_field(attachment, "byte_size");
  if (!type || *type != "image" || !id || id->empty() || id->size() > kMaxImageAttachmentIdBytes || string_has_control_byte(*id) || !mime_type ||
      !is_supported_image_attachment_mime_type(*mime_type) || !storage_path || !valid_attachment_storage_path(*storage_path) || !sha256 ||
      sha256->size() != 64 || !hex_string(*sha256) || !byte_size)
    return false;
  auto const redacted_start = ava::core::json::field_value_start(attachment, "redacted");
  return !redacted_start || bool_field_is_true(attachment, "redacted") || bool_field_is_false(attachment, "redacted");
}

void validate_message_entry(SessionReplayValidation& validation, std::size_t index, SessionEntry const& entry)
{
  if (!ava::core::json::is_valid_object(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "message entry data is not valid JSON");
    return;
  }
  if (object_has_duplicate_top_level_keys(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "message entry data has duplicate keys");
    return;
  }
  if (ava::core::json::field_value_start(entry.data_json, "provenance"))
  {
    if (entry.type != EntryType::UserMessage)
    {
      add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "synthetic delivery provenance is only supported on user messages");
      return;
    }
    auto provenance = parse_synthetic_delivery_provenance(entry);
    if (!provenance || !*provenance)
    {
      add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "",
                "synthetic delivery provenance is malformed, unsupported, or unbounded");
      return;
    }
  }
  auto const attachments_start = ava::core::json::field_value_start(entry.data_json, "attachments");
  if (!attachments_start)
    return;
  if (entry.type != EntryType::UserMessage)
  {
    add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "message attachments are only supported on user messages");
    return;
  }
  if (*attachments_start >= entry.data_json.size() || entry.data_json[*attachments_start] != '[')
  {
    add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "message attachments must be an array");
    return;
  }
  auto const attachments = image_attachment_objects(entry.data_json, "attachments");
  if (!attachments)
  {
    add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "message attachments must contain only objects");
    return;
  }
  if (attachments->size() > kMaxMessageAttachments)
  {
    add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "message has too many attachments");
    return;
  }
  std::unordered_set<std::string> attachment_ids;
  for (auto const& attachment : *attachments)
  {
    if (!valid_image_attachment_object(attachment))
    {
      add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "message image attachment metadata is invalid");
      return;
    }
    auto const id = ava::core::json::string_field(attachment, "id").value_or("");
    if (!attachment_ids.insert(id).second)
    {
      add_error(validation, SessionReplayIssueKind::InvalidMessageEntry, index, entry, "", "message image attachment ids must be unique");
      return;
    }
  }
}

bool string_has_control_byte_except_summary_whitespace(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return (byte < 0x20 && ch != '\n' && ch != '\t') || byte == 0x7F;
  });
}

bool valid_optional_short_string(std::string_view object, std::string_view key, std::size_t max_bytes, bool allow_empty)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return true;
  auto const value = ava::core::json::string_field(object, key);
  return value && (allow_empty || !value->empty()) && value->size() <= max_bytes && !string_has_control_byte(*value);
}

bool valid_optional_session_id(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return true;
  auto const value = ava::core::json::string_field(object, key);
  if (!value || value->empty())
    return false;
  return validate_session_id(*value).has_value();
}

bool valid_optional_entry_id(std::string_view object, std::string_view key, std::string_view entry_id)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return true;
  auto const value = ava::core::json::string_field(object, key);
  if (!value || value->empty())
    return false;
  return validate_parent_id(*value, entry_id).has_value();
}

bool schema_version_is_current(std::string_view object)
{
  return present_integer_matching(object, "schema_version", true) && ava::core::json::integer_field(object, "schema_version").value_or(0) == 1;
}

bool valid_metadata_origin(std::string_view origin)
{
  return origin.empty() || origin == "root" || origin == "fork" || origin == "clone" || origin == "manual" || origin == "import";
}

bool valid_optional_metadata_origin(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return true;
  auto const value = ava::core::json::string_field(object, key);
  return value && valid_metadata_origin(*value);
}

bool required_bounded_string(std::string_view object, std::string_view key, std::size_t max_bytes)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return false;
  auto const value = ava::core::json::string_field(object, key);
  return value && !value->empty() && value->size() <= max_bytes && !string_has_control_byte(*value);
}

bool required_session_id(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return false;
  auto const value = ava::core::json::string_field(object, key);
  return value && !value->empty() && validate_session_id(*value).has_value();
}

bool required_entry_id(std::string_view object, std::string_view key, std::string_view entry_id)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return false;
  auto const value = ava::core::json::string_field(object, key);
  return value && !value->empty() && validate_parent_id(*value, entry_id).has_value();
}

bool valid_optional_string_array(std::string_view object, std::string_view key, std::size_t max_count, std::size_t max_item_bytes)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return true;
  if (*start >= object.size() || object[*start] != '[')
    return false;

  std::size_t cursor = *start + 1;
  std::size_t string_count = 0;
  skip_json_ws(object, cursor);
  if (cursor < object.size() && object[cursor] != ']')
  {
    while (cursor < object.size())
    {
      if (object[cursor] != '"')
        return false;
      bool escaped = false;
      bool closed = false;
      for (++cursor; cursor < object.size(); ++cursor)
      {
        char const ch = object[cursor];
        if (escaped)
        {
          escaped = false;
          continue;
        }
        if (ch == '\\')
        {
          escaped = true;
          continue;
        }
        if (ch == '"')
        {
          closed = true;
          ++cursor;
          break;
        }
      }
      if (!closed)
        return false;
      ++string_count;
      if (string_count > max_count)
        return false;

      skip_json_ws(object, cursor);
      if (cursor >= object.size())
        return false;
      if (object[cursor] == ',')
      {
        ++cursor;
        skip_json_ws(object, cursor);
        continue;
      }
      if (object[cursor] == ']')
        break;
      return false;
    }
  }
  if (cursor >= object.size() || object[cursor] != ']')
    return false;
  ++cursor;
  skip_json_ws(object, cursor);
  if (cursor < object.size() && object[cursor] != ',' && object[cursor] != '}')
    return false;

  auto values = ava::core::json::strings_in_array_field(object, key);
  if (values.size() != string_count)
    return false;
  std::unordered_set<std::string> seen;
  for (auto const& value : values)
  {
    if (value.empty() || value.size() > max_item_bytes || string_has_control_byte(value))
      return false;
    if (!seen.insert(value).second)
      return false;
  }
  return true;
}

bool valid_optional_bool(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return true;
  if (object.substr(*start, 4) == "true")
    return true;
  return object.substr(*start, 5) == "false";
}

void validate_session_metadata_entry(SessionReplayValidation& validation, std::size_t index, SessionEntry const& entry)
{
  if (!ava::core::json::is_valid_object(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidSessionMetadataEntry, index, entry, "", "session_metadata entry data is not valid JSON");
    return;
  }

  auto const branch_origin = ava::core::json::string_field(entry.data_json, "branch_origin");
  bool const has_branch_origin = branch_origin && !branch_origin->empty();
  auto const subagent_job_start = ava::core::json::field_value_start(entry.data_json, "subagent_job");
  bool const has_subagent_job = subagent_job_start.has_value();
  auto const subagent_job = has_subagent_job ? ava::core::json::object_field(entry.data_json, "subagent_job") : std::nullopt;
  bool const valid_subagent_job = !has_subagent_job || (subagent_job && valid_subagent_job_history_object(*subagent_job));
  bool const has_meaningful_field =
      ava::core::json::field_value_start(entry.data_json, "name") || ava::core::json::field_value_start(entry.data_json, "generated_title") ||
      ava::core::json::field_value_start(entry.data_json, "labels") || ava::core::json::field_value_start(entry.data_json, "archived") ||
      ava::core::json::field_value_start(entry.data_json, "parent_session_id") || ava::core::json::field_value_start(entry.data_json, "source_session_id") ||
      ava::core::json::field_value_start(entry.data_json, "branch_from_entry_id") || has_branch_origin || has_subagent_job;
  if (!schema_version_is_current(entry.data_json) || !has_meaningful_field ||
      !valid_optional_short_string(entry.data_json, "name", kMaxSessionNameBytes, true) ||
      !valid_optional_short_string(entry.data_json, "generated_title", kMaxGeneratedSessionTitleBytes, false) ||
      !valid_optional_string_array(entry.data_json, "labels", kMaxSessionLabels, kMaxSessionLabelBytes) || !valid_optional_bool(entry.data_json, "archived") ||
      !valid_optional_session_id(entry.data_json, "parent_session_id") || !valid_optional_session_id(entry.data_json, "source_session_id") ||
      !valid_optional_entry_id(entry.data_json, "branch_from_entry_id", entry.id) ||
      !valid_optional_short_string(entry.data_json, "actor", kMaxSessionLabelBytes, false) ||
      !valid_optional_metadata_origin(entry.data_json, "branch_origin") || !valid_subagent_job)
  {
    add_error(validation, SessionReplayIssueKind::InvalidSessionMetadataEntry, index, entry, "",
              has_subagent_job && !valid_subagent_job ? "session_metadata entry has malformed subagent_job history"
                                                      : "session_metadata entry has malformed tree metadata");
  }
}

void validate_branch_summary_entry(SessionReplayValidation& validation, std::size_t index, SessionEntry const& entry,
                                   std::unordered_map<std::string, std::size_t> const& entry_indices)
{
  if (!ava::core::json::is_valid_object(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidBranchSummaryEntry, index, entry, "", "branch_summary entry data is not valid JSON");
    return;
  }

  auto const summary = ava::core::json::string_field(entry.data_json, "summary");
  auto const root_entry_id = ava::core::json::string_field(entry.data_json, "branch_root_entry_id");
  auto const tip_entry_id = ava::core::json::string_field(entry.data_json, "branch_tip_entry_id");
  auto const root_index = root_entry_id ? entry_indices.find(*root_entry_id) : entry_indices.end();
  auto const tip_index = tip_entry_id ? entry_indices.find(*tip_entry_id) : entry_indices.end();
  bool const valid_range = root_entry_id && tip_entry_id && root_index != entry_indices.end() && tip_index != entry_indices.end() &&
                           root_index->second < index && tip_index->second < index && root_index->second <= tip_index->second;

  if (!schema_version_is_current(entry.data_json) || !summary || summary->empty() || summary->size() > kMaxBranchSummaryBytes ||
      string_has_control_byte_except_summary_whitespace(*summary) || !valid_range || !required_session_id(entry.data_json, "source_session_id") ||
      !required_entry_id(entry.data_json, "branch_root_entry_id", entry.id) || !required_entry_id(entry.data_json, "branch_tip_entry_id", entry.id) ||
      !required_bounded_string(entry.data_json, "provider", 256) || !required_bounded_string(entry.data_json, "model", 256) ||
      !required_bounded_string(entry.data_json, "reason", 1024) || !valid_optional_short_string(entry.data_json, "actor", kMaxSessionLabelBytes, false))
  {
    add_error(validation, SessionReplayIssueKind::InvalidBranchSummaryEntry, index, entry, "", "branch_summary entry has malformed semantic metadata");
  }
}

void validate_compaction_boundaries(SessionReplayValidation& validation, std::size_t index, SessionEntry const& entry,
                                    std::unordered_map<std::string, ToolCallState> const& tool_calls,
                                    std::unordered_map<std::string, std::vector<PendingPermissionPrompt>> const& pending_permissions,
                                    SessionReplayValidationOptions const& options)
{
  if (options.require_tool_result_pairing)
  {
    for (auto const& [call_id, state] : tool_calls)
    {
      if (state.result_seen)
        continue;
      add_error(validation, SessionReplayIssueKind::CompactionWithUnresolvedToolCall, index, entry, call_id,
                "compaction occurred while a tool_call had no matching tool_result");
    }
  }

  if (options.require_permission_decision_integrity)
  {
    for (auto const& [unused_key, prompts] : pending_permissions)
    {
      (void)unused_key;
      for (auto const& prompt : prompts)
      {
        add_issue(validation, SessionReplayIssue{.severity = SessionReplayIssueSeverity::Error,
                                                 .kind = SessionReplayIssueKind::CompactionWithUnresolvedPermissionPrompt,
                                                 .entry_index = index,
                                                 .entry_id = entry.id,
                                                 .call_id = "",
                                                 .message = "compaction occurred while a permission prompt was unresolved"});
        (void)prompt;
      }
    }
  }
}

void validate_session_start_entry(SessionReplayValidation& validation, ActiveModelState& active_model, std::size_t index, SessionEntry const& entry)
{
  if (!ava::core::json::is_valid_object(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidModelEntry, index, entry, "", "session_start entry data is not valid JSON");
    return;
  }

  auto const mode = ava::core::json::string_field(entry.data_json, "mode").value_or("");
  auto const provider = ava::core::json::string_field(entry.data_json, "provider").value_or("");
  auto const model = ava::core::json::string_field(entry.data_json, "model").value_or("");
  if (!valid_mode(mode) || provider.empty() || model.empty() || !present_integer_matching(entry.data_json, "context_sources", false) ||
      !present_integer_matching(entry.data_json, "context_window_tokens", true) || !present_integer_matching(entry.data_json, "max_output_tokens", true) ||
      !present_boolean(entry.data_json, "prompt_override") || !present_boolean(entry.data_json, "supports_tools") ||
      !present_boolean(entry.data_json, "supports_streaming") || !present_boolean(entry.data_json, "supports_reasoning") ||
      !present_boolean(entry.data_json, "reports_usage"))
  {
    add_error(validation, SessionReplayIssueKind::InvalidModelEntry, index, entry, "", "session_start entry is missing required model/session metadata");
    return;
  }

  active_model.provider_id = provider;
  active_model.model_id = model;
}

void validate_model_change_entry(SessionReplayValidation& validation, ActiveModelState& active_model, std::size_t index, SessionEntry const& entry)
{
  if (!ava::core::json::is_valid_object(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidModelEntry, index, entry, "", "model_change entry data is not valid JSON");
    return;
  }

  auto const previous_provider = ava::core::json::string_field(entry.data_json, "previous_provider").value_or("");
  auto const previous_model = ava::core::json::string_field(entry.data_json, "previous_model").value_or("");
  auto const provider = ava::core::json::string_field(entry.data_json, "provider").value_or("");
  auto const model = ava::core::json::string_field(entry.data_json, "model").value_or("");
  if (previous_provider.empty() || previous_model.empty() || provider.empty() || model.empty() || (previous_provider == provider && previous_model == model) ||
      (!active_model.provider_id.empty() && (previous_provider != active_model.provider_id || previous_model != active_model.model_id)) ||
      !present_integer_matching(entry.data_json, "context_window_tokens", true) || !present_integer_matching(entry.data_json, "max_output_tokens", true) ||
      !present_boolean(entry.data_json, "supports_tools") || !present_boolean(entry.data_json, "supports_streaming") ||
      !present_boolean(entry.data_json, "supports_reasoning") || !present_boolean(entry.data_json, "reports_usage"))
  {
    add_error(validation, SessionReplayIssueKind::InvalidModelEntry, index, entry, "",
              "model_change entry is missing required provider/model transition metadata");
    return;
  }

  active_model.provider_id = provider;
  active_model.model_id = model;
}

void validate_reasoning_change_entry(SessionReplayValidation& validation, ActiveModelState const& active_model, std::size_t index, SessionEntry const& entry)
{
  if (!ava::core::json::is_valid_object(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "", "reasoning_change entry data is not valid JSON");
    return;
  }

  auto const provider = ava::core::json::string_field(entry.data_json, "provider").value_or("");
  auto const model = ava::core::json::string_field(entry.data_json, "model").value_or("");
  if (provider.empty() || model.empty() || !required_boolean(entry.data_json, "enabled") || !present_non_empty_string(entry.data_json, "format") ||
      !present_integer_matching(entry.data_json, "budget_tokens", true) || !present_non_empty_string(entry.data_json, "display"))
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "", "reasoning_change entry is missing required semantic fields");
    return;
  }

  if (!active_model.provider_id.empty() && (provider != active_model.provider_id || model != active_model.model_id))
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "",
              "reasoning_change provider/model does not match active session model");
    return;
  }

  auto const enabled = bool_field_is_true(entry.data_json, "enabled");
  auto const level = ava::core::json::string_field(entry.data_json, "level").value_or("");
  if (enabled && level.empty())
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "", "enabled reasoning_change entry is missing level");
  }
}

void validate_reasoning_block_entry(SessionReplayValidation& validation, ActiveModelState const& active_model, std::size_t index, SessionEntry const& entry)
{
  if (!ava::core::json::is_valid_object(entry.data_json))
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "", "reasoning_block entry data is not valid JSON");
    return;
  }

  auto const provider = ava::core::json::string_field(entry.data_json, "provider").value_or("");
  auto const model = ava::core::json::string_field(entry.data_json, "model").value_or("");
  auto const text = ava::core::json::string_field(entry.data_json, "text").value_or("");
  auto const signature = ava::core::json::string_field(entry.data_json, "signature").value_or("");
  auto const redacted_data = ava::core::json::string_field(entry.data_json, "redacted_data").value_or("");
  auto const native_item_start = ava::core::json::field_value_start(entry.data_json, "native_item_json");
  auto const native_item_json = ava::core::json::string_field(entry.data_json, "native_item_json");
  if (provider.empty() || model.empty() || !present_non_empty_string(entry.data_json, "format") || !present_boolean(entry.data_json, "redacted") ||
      (text.empty() && signature.empty() && redacted_data.empty() && (!native_item_json || native_item_json->empty())))
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "", "reasoning_block entry is missing provider/model/content metadata");
  }
  if ((!active_model.provider_id.empty() || !active_model.model_id.empty()) && (provider != active_model.provider_id || model != active_model.model_id))
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "",
              "reasoning_block provider/model does not match active session model");
  }
  // Version 3 OpenAI writers can only persist native replay metadata after the
  // strict provider validator accepts it. Older sessions keep readable text as
  // a compatibility fallback even when their optional opaque metadata is stale.
  bool const is_openai_responses_reasoning =
      provider == "openai" && ava::core::json::string_field(entry.data_json, "format").value_or("") == "openai_responses";
  if (native_item_start && is_openai_responses_reasoning && entry.version >= 3 &&
      (!native_item_json || !ava::core::is_valid_openai_native_reasoning_item_json(*native_item_json)))
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "",
              "reasoning_block native_item_json must be a bounded OpenAI reasoning object with id and summary array");
  }
  if (native_item_start && !is_openai_responses_reasoning &&
      (!native_item_json || !ava::core::json::is_valid_object(*native_item_json) ||
       ava::core::json::string_field(*native_item_json, "type").value_or("") != "reasoning"))
  {
    add_error(validation, SessionReplayIssueKind::InvalidReasoningEntry, index, entry, "", "reasoning_block native_item_json must be a JSON reasoning object");
  }
}

}  // namespace

std::string_view to_string(SessionReplayIssueSeverity severity) noexcept
{
  switch (severity)
  {
    case SessionReplayIssueSeverity::Warning:
      return "warning";
    case SessionReplayIssueSeverity::Error:
      return "error";
  }
  return "error";
}

std::string_view to_string(SessionReplayIssueKind kind) noexcept
{
  switch (kind)
  {
    case SessionReplayIssueKind::UnsupportedEntryVersion:
      return "unsupported_entry_version";
    case SessionReplayIssueKind::DuplicateEntryId:
      return "duplicate_entry_id";
    case SessionReplayIssueKind::UnknownParentId:
      return "unknown_parent_id";
    case SessionReplayIssueKind::EmptyToolCallId:
      return "empty_tool_call_id";
    case SessionReplayIssueKind::DuplicateToolCallId:
      return "duplicate_tool_call_id";
    case SessionReplayIssueKind::ToolResultWithoutCall:
      return "tool_result_without_call";
    case SessionReplayIssueKind::ToolResultToolMismatch:
      return "tool_result_tool_mismatch";
    case SessionReplayIssueKind::DuplicateToolResult:
      return "duplicate_tool_result";
    case SessionReplayIssueKind::UnresolvedToolCall:
      return "unresolved_tool_call";
    case SessionReplayIssueKind::MissingStructuredToolResult:
      return "missing_structured_tool_result";
    case SessionReplayIssueKind::InvalidStructuredToolResult:
      return "invalid_structured_tool_result";
    case SessionReplayIssueKind::StructuredToolResultMismatch:
      return "structured_tool_result_mismatch";
    case SessionReplayIssueKind::InvalidPermissionDecision:
      return "invalid_permission_decision";
    case SessionReplayIssueKind::PermissionResolutionWithoutAsk:
      return "permission_resolution_without_ask";
    case SessionReplayIssueKind::UnresolvedPermissionPrompt:
      return "unresolved_permission_prompt";
    case SessionReplayIssueKind::InvalidCompactionEntry:
      return "invalid_compaction_entry";
    case SessionReplayIssueKind::InvalidSessionMetadataEntry:
      return "invalid_session_metadata_entry";
    case SessionReplayIssueKind::InvalidBranchSummaryEntry:
      return "invalid_branch_summary_entry";
    case SessionReplayIssueKind::InvalidMessageEntry:
      return "invalid_message_entry";
    case SessionReplayIssueKind::CompactionWithUnresolvedToolCall:
      return "compaction_with_unresolved_tool_call";
    case SessionReplayIssueKind::CompactionWithUnresolvedPermissionPrompt:
      return "compaction_with_unresolved_permission_prompt";
    case SessionReplayIssueKind::InvalidModelEntry:
      return "invalid_model_entry";
    case SessionReplayIssueKind::InvalidReasoningEntry:
      return "invalid_reasoning_entry";
    case SessionReplayIssueKind::InvalidAssistantOutputItem:
      return "invalid_assistant_output_item";
    case SessionReplayIssueKind::InvalidAssistantTurnCommit:
      return "invalid_assistant_turn_commit";
    case SessionReplayIssueKind::IncompleteAssistantTurn:
      return "incomplete_assistant_turn";
    case SessionReplayIssueKind::MalformedAssistantTurn:
      return "malformed_assistant_turn";
    case SessionReplayIssueKind::ToolResultOutputItemMismatch:
      return "tool_result_output_item_mismatch";
  }
  return "invalid_structured_tool_result";
}

std::string sanitized_message_data_json(std::string_view data_json, bool allow_attachments)
{
  if (!ava::core::json::is_valid_object(data_json) || object_has_duplicate_top_level_keys(data_json))
    return "{}";

  std::string json = "{";
  bool first = true;
  auto append_key = [&](std::string_view key) {
    if (!first)
      json += ',';
    first = false;
    json += '"';
    json += key;
    json += "\":";
  };
  auto append_string = [&](std::string_view key, std::optional<std::string> const& value) {
    if (!value)
      return;
    append_key(key);
    json += '"';
    json += ava::core::json::escape(*value);
    json += '"';
  };

  append_string("text", ava::core::json::string_field(data_json, "text"));
  if (!allow_attachments)
  {
    json += '}';
    return json;
  }

  auto const attachments = image_attachment_objects(data_json, "attachments");
  if (attachments && !attachments->empty() && attachments->size() <= kMaxMessageAttachments)
  {
    std::unordered_set<std::string> attachment_ids;
    bool valid_attachments = true;
    for (auto const& attachment : *attachments)
    {
      if (!valid_image_attachment_object(attachment))
      {
        valid_attachments = false;
        break;
      }
      auto const id = ava::core::json::string_field(attachment, "id").value_or("");
      if (!attachment_ids.insert(id).second)
      {
        valid_attachments = false;
        break;
      }
    }
    if (!valid_attachments)
    {
      json += '}';
      return json;
    }

    append_key("attachments");
    json += '[';
    bool first_attachment = true;
    for (auto const& attachment : *attachments)
    {
      if (!first_attachment)
        json += ',';
      first_attachment = false;
      json += '{';
      bool first_field = true;
      auto append_field_key = [&](std::string_view key) {
        if (!first_field)
          json += ',';
        first_field = false;
        json += '"';
        json += key;
        json += "\":";
      };
      auto append_attachment_string = [&](std::string_view key, std::optional<std::string> const& value) {
        if (!value)
          return;
        append_field_key(key);
        json += '"';
        json += ava::core::json::escape(*value);
        json += '"';
      };
      auto append_attachment_integer = [&](std::string_view key, std::optional<long long> value) {
        if (!value)
          return;
        append_field_key(key);
        json += std::to_string(*value);
      };
      auto append_attachment_bool = [&](std::string_view key, bool value) {
        append_field_key(key);
        json += value ? "true" : "false";
      };
      append_attachment_string("id", ava::core::json::string_field(attachment, "id"));
      append_attachment_string("type", ava::core::json::string_field(attachment, "type"));
      append_attachment_string("mime_type", ava::core::json::string_field(attachment, "mime_type"));
      append_attachment_integer("byte_size", strict_positive_integer_field(attachment, "byte_size"));
      append_attachment_string("sha256", ava::core::json::string_field(attachment, "sha256"));
      append_attachment_string("storage_path", ava::core::json::string_field(attachment, "storage_path"));
      if (ava::core::json::field_value_start(attachment, "redacted"))
      {
        append_attachment_bool("redacted", bool_field_is_true(attachment, "redacted"));
      }
      json += '}';
    }
    json += ']';
  }
  json += '}';
  return json;
}

SessionReplayValidation validate_session_replay(std::vector<SessionEntry> const& entries, SessionReplayValidationOptions options)
{
  SessionReplayValidation validation;
  auto const assistant_output = classify_assistant_output(entries);
  add_assistant_output_diagnostics(validation, assistant_output);
  std::unordered_set<std::string> seen_entry_ids;
  std::unordered_map<std::string, std::size_t> seen_entry_indices;
  std::unordered_map<std::string, ToolCallState> tool_calls;
  std::unordered_map<std::string, std::vector<PendingPermissionPrompt>> pending_permissions;
  ActiveModelState active_model;

  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    auto const& entry = entries[index];
    if (options.require_entry_versions && !supported_entry_version(entry.version))
    {
      add_error(validation, SessionReplayIssueKind::UnsupportedEntryVersion, index, entry, "", "session entry version is outside the supported range");
    }
    if (options.require_known_parent_ids && !entry.parent_id.empty() && seen_entry_ids.find(entry.parent_id) == seen_entry_ids.end())
    {
      add_error(validation, SessionReplayIssueKind::UnknownParentId, index, entry, "", "session entry parent_id does not reference an earlier entry");
    }
    if (!seen_entry_ids.insert(entry.id).second)
    {
      add_error(validation, SessionReplayIssueKind::DuplicateEntryId, index, entry, "", "duplicate session entry id");
    }
    else
    {
      seen_entry_indices.emplace(entry.id, index);
    }

    if (auto const* committed_turn = assistant_output.find_turn_by_commit_index(index))
      register_committed_assistant_output_calls(validation, *committed_turn, tool_calls);

    if (options.require_model_reasoning_integrity)
    {
      if (entry.type == EntryType::SessionStart)
      {
        validate_session_start_entry(validation, active_model, index, entry);
        continue;
      }
      if (entry.type == EntryType::ModelChange)
      {
        validate_model_change_entry(validation, active_model, index, entry);
        continue;
      }
      if (entry.type == EntryType::ReasoningChange)
      {
        validate_reasoning_change_entry(validation, active_model, index, entry);
        continue;
      }
      if (entry.type == EntryType::ReasoningBlock)
      {
        validate_reasoning_block_entry(validation, active_model, index, entry);
        continue;
      }
    }

    if (entry.type == EntryType::PermissionDecision && options.require_permission_decision_integrity)
    {
      validate_permission_decision(validation, pending_permissions, index, entry);
      continue;
    }

    if (entry.type == EntryType::SessionMetadata)
    {
      validate_session_metadata_entry(validation, index, entry);
      continue;
    }

    if (entry.type == EntryType::BranchSummary)
    {
      validate_branch_summary_entry(validation, index, entry, seen_entry_indices);
      continue;
    }

    if (entry.type == EntryType::Compaction && options.require_compaction_integrity)
    {
      validate_compaction_entry(validation, index, entry);
      validate_compaction_boundaries(validation, index, entry, tool_calls, pending_permissions, options);
      continue;
    }

    if (entry.type == EntryType::UserMessage || entry.type == EntryType::AssistantMessage)
    {
      validate_message_entry(validation, index, entry);
      continue;
    }

    if (entry.type == EntryType::ToolCall)
    {
      auto const call_id = ava::core::json::string_field(entry.data_json, "call_id").value_or("");
      auto const tool_name = ava::core::json::string_field(entry.data_json, "name").value_or("");
      if (call_id.empty())
      {
        add_error(validation, SessionReplayIssueKind::EmptyToolCallId, index, entry, "", "tool_call entry is missing call_id");
        continue;
      }
      if (options.require_tool_result_pairing && tool_calls.find(call_id) != tool_calls.end())
      {
        add_error(validation, SessionReplayIssueKind::DuplicateToolCallId, index, entry, call_id, "tool_call id is reused in the same session");
        continue;
      }
      tool_calls.emplace(call_id, ToolCallState{.entry_id = entry.id, .tool_name = tool_name, .assistant_output_entry_id = "", .result_seen = false});
      continue;
    }

    if (entry.type != EntryType::ToolResult)
      continue;

    if (entry.version < 4 && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id").has_value())
    {
      add_error(validation, SessionReplayIssueKind::ToolResultOutputItemMismatch, index, entry, "",
                "tool_result assistant_output_entry_id is a v4-only private replay binding");
      continue;
    }

    auto const call_id = ava::core::json::string_field(entry.data_json, "call_id").value_or("");
    auto const tool_name = ava::core::json::string_field(entry.data_json, "name").value_or("");
    if (call_id.empty())
    {
      add_error(validation, SessionReplayIssueKind::EmptyToolCallId, index, entry, "", "tool_result entry is missing call_id");
      continue;
    }
    if (!options.require_tool_result_pairing)
    {
      auto const committed_call = tool_calls.find(call_id);
      auto const* registered_call =
          committed_call != tool_calls.end() && !committed_call->second.assistant_output_entry_id.empty() ? &committed_call->second : nullptr;
      if ((registered_call || ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id").has_value()) &&
          !tool_result_matches_committed_output_item(validation, assistant_output, entries, index, entry, call_id, tool_name, registered_call))
      {
        continue;
      }
      if (options.require_structured_tool_results)
      {
        validate_structured_tool_result(validation, index, entry, call_id, tool_name);
      }
      continue;
    }

    auto tool_call = tool_calls.find(call_id);
    if (tool_call == tool_calls.end())
    {
      if (ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id").has_value())
      {
        static_cast<void>(tool_result_matches_committed_output_item(validation, assistant_output, entries, index, entry, call_id, tool_name, nullptr));
      }
      else
      {
        add_error(validation, SessionReplayIssueKind::ToolResultWithoutCall, index, entry, call_id, "tool_result has no earlier matching tool_call");
      }
      continue;
    }
    if (!tool_result_matches_committed_output_item(validation, assistant_output, entries, index, entry, call_id, tool_name, &tool_call->second))
      continue;
    if (tool_call->second.result_seen)
    {
      add_error(validation, SessionReplayIssueKind::DuplicateToolResult, index, entry, call_id, "tool_result duplicates an already completed tool_call");
      continue;
    }
    if (!tool_call->second.tool_name.empty() && !tool_name.empty() && tool_call->second.tool_name != tool_name)
    {
      add_error(validation, SessionReplayIssueKind::ToolResultToolMismatch, index, entry, call_id, "tool_result name does not match its tool_call");
      continue;
    }
    tool_call->second.result_seen = true;
    if (options.require_structured_tool_results)
    {
      validate_structured_tool_result(validation, index, entry, call_id, tool_name);
    }
  }

  if (options.require_tool_result_pairing)
  {
    for (auto const& [call_id, state] : tool_calls)
    {
      if (state.result_seen)
        continue;
      add_issue(validation, SessionReplayIssue{.severity = SessionReplayIssueSeverity::Error,
                                               .kind = SessionReplayIssueKind::UnresolvedToolCall,
                                               .entry_index = entries.size(),
                                               .entry_id = state.entry_id,
                                               .call_id = call_id,
                                               .message = "tool_call has no matching tool_result"});
    }
  }

  if (options.require_permission_decision_integrity)
  {
    for (auto const& [unused_key, prompts] : pending_permissions)
    {
      (void)unused_key;
      for (auto const& prompt : prompts)
      {
        add_issue(validation, SessionReplayIssue{.severity = SessionReplayIssueSeverity::Error,
                                                 .kind = SessionReplayIssueKind::UnresolvedPermissionPrompt,
                                                 .entry_index = entries.size(),
                                                 .entry_id = prompt.entry_id,
                                                 .call_id = "",
                                                 .message = "ask permission_decision has no matching resolution"});
      }
    }
  }

  return validation;
}

}  // namespace ava::session
