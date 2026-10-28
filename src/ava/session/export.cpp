#include "sys.h"
#include "ava/session/export.h"
#include "ava/session/logical_projection.h"
#include "ava/session/record.h"
#include "ava/session/validation.h"
#include "ava/core/json.h"
#include "ava/core/text_escape.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace ava::session {
namespace {

std::string json_string(std::string_view value)
{
  return "\"" + ava::core::json::escape(value) + "\"";
}

std::size_t longest_backtick_run(std::string_view text) noexcept
{
  std::size_t longest = 0;
  std::size_t current = 0;
  for (char const ch : text)
  {
    if (ch == '`')
    {
      ++current;
      longest = std::max(longest, current);
    }
    else
    {
      current = 0;
    }
  }
  return longest;
}

std::string fence_for(std::string_view text)
{
  return std::string(std::max<std::size_t>(3, longest_backtick_run(text) + 1), '`');
}

std::string sanitize_fenced_content(std::string_view content)
{
  std::string sanitized;
  sanitized.reserve(content.size());
  constexpr char kHex[] = "0123456789ABCDEF";
  for (char const ch : content)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if ((byte < 0x20 && ch != '\n' && ch != '\t') || byte == 0x7F)
    {
      sanitized += "\\u00";
      sanitized.push_back(kHex[(byte >> 4U) & 0x0FU]);
      sanitized.push_back(kHex[byte & 0x0FU]);
    }
    else
    {
      sanitized.push_back(ch);
    }
  }
  return sanitized;
}

void append_heading(std::string& out, std::string_view heading)
{
  out += "## ";
  out += heading;
  out += "\n\n";
}

void append_fenced_block(std::string& out, std::string_view label, std::string_view content, std::string_view language = "text")
{
  out += label;
  out += ":\n\n";
  auto const sanitized_content = sanitize_fenced_content(content);
  auto const fence = fence_for(sanitized_content);
  out += fence;
  if (!language.empty())
    out += language;
  out += '\n';
  out += sanitized_content;
  if (!sanitized_content.empty() && sanitized_content.back() != '\n')
    out += '\n';
  out += fence;
  out += "\n\n";
}

void append_optional_fenced_block(std::string& out, std::string_view label, std::optional<std::string> const& content, std::string_view language = "text")
{
  if (content && !content->empty())
    append_fenced_block(out, label, *content, language);
}

std::string metadata_json(SessionEntry const& entry)
{
  return "{\"id\":" + json_string(entry.id) + ",\"parent_id\":" + json_string(entry.parent_id) + ",\"type\":" + json_string(to_string(entry.type)) +
         ",\"timestamp\":" + json_string(entry.timestamp) + "}";
}

void append_metadata(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  if (!options.include_metadata)
    return;
  append_fenced_block(out, "Metadata", metadata_json(entry), "json");
}

std::optional<std::string> string_field(SessionEntry const& entry, std::string_view key)
{
  return ava::core::json::string_field(entry.data_json, key);
}

std::optional<long long> integer_field(SessionEntry const& entry, std::string_view key)
{
  return ava::core::json::integer_field(entry.data_json, key);
}

std::optional<std::string> object_field(SessionEntry const& entry, std::string_view key)
{
  return ava::core::json::object_field(entry.data_json, key);
}

bool bool_field_is_true(SessionEntry const& entry, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(entry.data_json, key);
  return start && entry.data_json.substr(*start, 4) == "true";
}

std::string success_text(SessionEntry const& entry)
{
  auto const start = ava::core::json::field_value_start(entry.data_json, "success");
  if (!start)
    return "unknown";
  if (entry.data_json.substr(*start, 4) == "true")
    return "true";
  if (entry.data_json.substr(*start, 5) == "false")
    return "false";
  return "unknown";
}

std::string image_attachment_export_text(std::string_view data_json)
{
  std::string text;
  for (auto const& attachment : ava::core::json::objects_in_array_field(data_json, "attachments"))
  {
    auto const id = ava::core::json::string_field(attachment, "id").value_or("");
    auto const mime_type = ava::core::json::string_field(attachment, "mime_type").value_or("");
    auto const byte_size = ava::core::json::integer_field(attachment, "byte_size").value_or(0);
    if (id.empty() || mime_type.empty() || byte_size <= 0)
      continue;
    if (!text.empty())
      text += '\n';
    text += "[image attachment: id=" + id + " mime=" + mime_type + " bytes=" + std::to_string(byte_size);
    auto const redacted_start = ava::core::json::field_value_start(attachment, "redacted");
    if (redacted_start && attachment.substr(*redacted_start, 4) == "true")
      text += " redacted=true";
    text += ']';
  }
  return text;
}

void append_user_message(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "User");
  append_metadata(out, entry, options);
  auto const sanitized = sanitized_message_data_json(entry.data_json);
  append_fenced_block(out, "Message", ava::core::json::string_field(sanitized, "text").value_or(""));
  auto const attachments = image_attachment_export_text(sanitized);
  append_optional_fenced_block(out, "Attachments", attachments.empty() ? std::nullopt : std::optional<std::string>(attachments));
}

void append_assistant_message(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Assistant");
  append_metadata(out, entry, options);
  append_fenced_block(out, "Message", string_field(entry, "text").value_or(""));
  append_optional_fenced_block(out, "Usage", object_field(entry, "usage"), "json");
}

void append_tool_call(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Tool Call");
  append_metadata(out, entry, options);
  append_fenced_block(out, "Name", string_field(entry, "name").value_or(""));
  append_fenced_block(out, "Call ID", string_field(entry, "call_id").value_or(""));
  append_fenced_block(out, "Arguments", string_field(entry, "arguments").value_or(""), "json");
}

void append_tool_result(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Tool Result");
  append_metadata(out, entry, options);
  append_fenced_block(out, "Name", string_field(entry, "name").value_or(""));
  append_fenced_block(out, "Call ID", string_field(entry, "call_id").value_or(""));
  append_fenced_block(out, "Success", success_text(entry));
  append_fenced_block(out, "Result", string_field(entry, "result").value_or(""));
}

void append_mode_change(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Mode Change");
  append_metadata(out, entry, options);
  append_fenced_block(out, "Mode", string_field(entry, "mode").value_or(""));
}

void append_model_change(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Model Change");
  append_metadata(out, entry, options);
  append_optional_fenced_block(out, "Previous provider", string_field(entry, "previous_provider"));
  append_optional_fenced_block(out, "Previous model", string_field(entry, "previous_model"));
  append_optional_fenced_block(out, "Provider", string_field(entry, "provider"));
  append_optional_fenced_block(out, "Model", string_field(entry, "model"));
  append_optional_fenced_block(out, "Display name", string_field(entry, "display_name"));
}

void append_reasoning_block(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Reasoning");
  append_metadata(out, entry, options);
  append_optional_fenced_block(out, "Provider", string_field(entry, "provider"));
  append_optional_fenced_block(out, "Model", string_field(entry, "model"));
  append_optional_fenced_block(out, "Format", string_field(entry, "format"));
  bool const redacted = bool_field_is_true(entry, "redacted");
  append_fenced_block(out, "Redacted", redacted ? "true" : "false");
  append_fenced_block(out, "Signature present", string_field(entry, "signature").has_value() ? "true" : "false");
  if (!redacted)
    append_optional_fenced_block(out, "Text", string_field(entry, "text"));
}

void append_reasoning_change(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Reasoning Change");
  append_metadata(out, entry, options);
  append_optional_fenced_block(out, "Provider", string_field(entry, "provider"));
  append_optional_fenced_block(out, "Model", string_field(entry, "model"));
  append_optional_fenced_block(out, "Format", string_field(entry, "format"));
  append_optional_fenced_block(out, "Level", string_field(entry, "level"));
  if (auto const budget_tokens = integer_field(entry, "budget_tokens"))
  {
    append_fenced_block(out, "Budget tokens", std::to_string(*budget_tokens));
  }
  append_optional_fenced_block(out, "Display", string_field(entry, "display"));
}

void append_session_start(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Session Start");
  append_metadata(out, entry, options);
  append_optional_fenced_block(out, "Mode", string_field(entry, "mode"));
  append_optional_fenced_block(out, "Provider", string_field(entry, "provider"));
  append_optional_fenced_block(out, "Model", string_field(entry, "model"));
}

void append_session_metadata(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Session Metadata");
  append_metadata(out, entry, options);
  append_optional_fenced_block(out, "Name", string_field(entry, "name"));
  append_optional_fenced_block(out, "Parent session", string_field(entry, "parent_session_id"));
  append_optional_fenced_block(out, "Source session", string_field(entry, "source_session_id"));
  append_optional_fenced_block(out, "Branch origin", string_field(entry, "branch_origin"));
  append_fenced_block(out, "Data", entry.data_json, "json");
}

void append_compaction_number(std::string& out, SessionEntry const& entry, std::string_view label, std::string_view key)
{
  if (auto const value = integer_field(entry, key))
    append_fenced_block(out, label, std::to_string(*value));
}

void append_compaction(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Compaction");
  append_metadata(out, entry, options);
  auto const summary = string_field(entry, "summary").value_or("Prior context was compacted, but the summary is unavailable.");
  append_fenced_block(out, "Summary", summary);
  append_optional_fenced_block(out, "Carry-forward instructions", string_field(entry, "instructions"));
  if (options.include_metadata)
  {
    append_optional_fenced_block(out, "Trigger", string_field(entry, "trigger"));
    append_optional_fenced_block(out, "Status", string_field(entry, "status"));
    append_optional_fenced_block(out, "Model", string_field(entry, "model"));
    append_fenced_block(out, "Summary unavailable", bool_field_is_true(entry, "summary_unavailable") ? "true" : "false");
    append_compaction_number(out, entry, "Estimated tokens", "estimated_tokens");
    append_compaction_number(out, entry, "Threshold tokens", "threshold_tokens");
    append_compaction_number(out, entry, "Keep recent tokens", "keep_recent_tokens");
    append_compaction_number(out, entry, "Keep recent messages", "keep_recent_messages");
  }
}

void append_branch_summary(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Branch Summary");
  append_metadata(out, entry, options);
  append_fenced_block(out, "Summary", string_field(entry, "summary").value_or(""));
  if (options.include_metadata)
    append_fenced_block(out, "Data", entry.data_json, "json");
}

void append_error(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Error");
  append_metadata(out, entry, options);
  append_optional_fenced_block(out, "Category", string_field(entry, "category"));
  append_fenced_block(out, "Message", string_field(entry, "message").value_or(""));
  append_optional_fenced_block(out, "Details", string_field(entry, "details"));
}

void append_permission_decision(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Permission Decision");
  append_metadata(out, entry, options);
  append_fenced_block(out, "Data", entry.data_json, "json");
}

void append_cancel(std::string& out, SessionEntry const& entry, ExportOptions const& options)
{
  append_heading(out, "Cancel");
  append_metadata(out, entry, options);
  append_fenced_block(out, "Data", entry.data_json, "json");
}

std::string format_projected_session_markdown(std::vector<SessionEntry> const& entries, ExportOptions const& options)
{
  std::string out = "# AVA Session Export\n\n";

  for (auto const& entry : entries)
  {
    if (is_internal_replay_user_message(entry))
      continue;
    switch (entry.type)
    {
      case EntryType::SessionStart:
        append_session_start(out, entry, options);
        break;
      case EntryType::SessionMetadata:
        append_session_metadata(out, entry, options);
        break;
      case EntryType::UserMessage:
        append_user_message(out, entry, options);
        break;
      case EntryType::AssistantMessage:
        append_assistant_message(out, entry, options);
        break;
      case EntryType::ToolCall:
        if (options.include_tool_details)
          append_tool_call(out, entry, options);
        break;
      case EntryType::ToolResult:
        if (options.include_tool_details)
          append_tool_result(out, entry, options);
        break;
      case EntryType::PermissionDecision:
        append_permission_decision(out, entry, options);
        break;
      case EntryType::ModeChange:
        append_mode_change(out, entry, options);
        break;
      case EntryType::ModelChange:
        append_model_change(out, entry, options);
        break;
      case EntryType::ReasoningBlock:
        append_reasoning_block(out, entry, options);
        break;
      case EntryType::ReasoningChange:
        append_reasoning_change(out, entry, options);
        break;
      // Shared logical projection removes physical v4 records before this
      // renderer; retain this defense in depth for direct internal callers.
      case EntryType::AssistantOutputItem:
      case EntryType::AssistantTurnCommit:
        break;
      case EntryType::Compaction:
        if (options.include_compactions)
          append_compaction(out, entry, options);
        break;
      case EntryType::BranchSummary:
        append_branch_summary(out, entry, options);
        break;
      case EntryType::Error:
        append_error(out, entry, options);
        break;
      case EntryType::Cancel:
        append_cancel(out, entry, options);
        break;
    }
  }

  return out;
}

std::string format_projected_session_html(std::vector<SessionEntry> const& entries, ExportOptions const& options)
{
  auto const markdown = format_projected_session_markdown(entries, options);
  std::string out;
  out.reserve(markdown.size() + 1024);
  out += "<!doctype html>\n";
  out += "<html lang=\"en\">\n";
  out += "<head>\n";
  out += "<meta charset=\"utf-8\">\n";
  out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
  out += "<title>AVA Session Export</title>\n";
  out += "<style>\n";
  out +=
      ":root{color-scheme:light dark;font-family:ui-sans-serif,system-ui,sans-serif;}"
      "body{margin:0;padding:2rem;background:Canvas;color:CanvasText;}"
      "main{max-width:72rem;margin:0 auto;}"
      "pre{white-space:pre-wrap;word-break:break-word;border:1px solid color-mix(in srgb,CanvasText 18%,transparent);"
      "border-radius:6px;padding:1rem;background:color-mix(in srgb,CanvasText 4%,Canvas);line-height:1.45;}"
      "h1{font-size:1.5rem;margin:0 0 1rem;}"
      ".meta{color:color-mix(in srgb,CanvasText 65%,transparent);font-size:.875rem;margin-bottom:1rem;}\n";
  out += "</style>\n";
  out += "</head>\n";
  out += "<body>\n";
  out += "<main>\n";
  out += "<h1>AVA Session Export</h1>\n";
  out += "<div class=\"meta\">Generated from AVA JSONL session entries. Markdown content is escaped as text.</div>\n";
  out += "<pre>";
  out += ava::core::html_escape(markdown);
  out += "</pre>\n";
  out += "</main>\n";
  out += "</body>\n";
  out += "</html>\n";
  return out;
}

}  // namespace

ava::core::Result<std::string> format_session_portable_jsonl_checked(std::vector<SessionEntry> const& entries)
{
  auto projected = project_portable_session_history(entries);
  if (!projected)
    return std::unexpected(std::move(projected.error()));
  auto const validation = validate_session_replay(*projected);
  if (!validation.ok())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "portable session archive is not independently replay-valid");
    error.with_context("errors", std::to_string(validation.error_count));
    auto const issue =
        std::ranges::find_if(validation.issues, [](SessionReplayIssue const& issue) { return issue.severity == SessionReplayIssueSeverity::Error; });
    if (issue != validation.issues.end())
    {
      error.with_context("issue_kind", std::string(to_string(issue->kind))).with_context("entry_id", issue->entry_id).with_context("issue", issue->message);
    }
    return std::unexpected(std::move(error));
  }

  std::string out;
  for (auto const& entry : *projected)
  {
    auto line = serialize_session_entry_line(entry);
    if (!line)
      return std::unexpected(std::move(line.error()));
    out += *line;
    out += '\n';
  }
  return out;
}

ava::core::Result<std::string> format_session_markdown_checked(std::vector<SessionEntry> const& entries, ExportOptions const& options)
{
  auto projected = project_ordered_public_session_history(entries);
  if (!projected)
    return std::unexpected(std::move(projected.error()));
  return format_projected_session_markdown(*projected, options);
}

ava::core::Result<std::string> format_session_html_checked(std::vector<SessionEntry> const& entries, ExportOptions const& options)
{
  auto projected = project_ordered_public_session_history(entries);
  if (!projected)
    return std::unexpected(std::move(projected.error()));
  return format_projected_session_html(*projected, options);
}

std::string format_session_markdown(std::vector<SessionEntry> const& entries, ExportOptions const& options)
{
  auto formatted = format_session_markdown_checked(entries, options);
  return formatted ? std::move(*formatted) : std::string{};
}

std::string format_session_html(std::vector<SessionEntry> const& entries, ExportOptions const& options)
{
  auto formatted = format_session_html_checked(entries, options);
  return formatted ? std::move(*formatted) : std::string{};
}

}  // namespace ava::session
