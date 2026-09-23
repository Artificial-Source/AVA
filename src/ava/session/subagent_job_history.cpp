#include "sys.h"
#include "ava/session/record.h"
#include "ava/session/subagent_job_history.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace ava::session {
namespace {

bool has_control_byte(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7F;
  });
}

bool has_forbidden_summary_control(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return (byte < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || byte == 0x7F;
  });
}

bool valid_history_id(std::string_view value)
{
  if (value.empty() || value.size() > kMaxSubagentJobHistoryIdBytes || !validate_session_id(value))
    return false;
  return std::ranges::all_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == ':';
  });
}

bool valid_timestamp(std::string_view value)
{
  return !value.empty() && value.size() <= kMaxSubagentJobHistoryTimestampBytes && !has_control_byte(value) && ava::core::json::is_valid_utf8(value);
}

bool valid_optional_bounded_text(std::optional<std::string> const& value, std::size_t max_bytes, bool allow_summary_whitespace)
{
  if (!value)
    return true;
  if (value->size() > max_bytes || !ava::core::json::is_valid_utf8(*value))
    return false;
  return allow_summary_whitespace ? !has_forbidden_summary_control(*value) : !has_control_byte(*value);
}

std::optional<SubagentJobHistoryPhase> parse_phase(std::string_view value)
{
  if (value == "start")
    return SubagentJobHistoryPhase::Start;
  if (value == "terminal")
    return SubagentJobHistoryPhase::Terminal;
  return std::nullopt;
}

std::optional<SubagentJobHistoryMode> parse_mode(std::string_view value)
{
  if (value == "foreground")
    return SubagentJobHistoryMode::Foreground;
  if (value == "background")
    return SubagentJobHistoryMode::Background;
  return std::nullopt;
}

std::optional<SubagentJobHistoryExecution> parse_execution(std::string_view value)
{
  if (value == "starting")
    return SubagentJobHistoryExecution::Starting;
  if (value == "running")
    return SubagentJobHistoryExecution::Running;
  if (value == "completed")
    return SubagentJobHistoryExecution::Completed;
  if (value == "failed")
    return SubagentJobHistoryExecution::Failed;
  if (value == "canceled")
    return SubagentJobHistoryExecution::Canceled;
  if (value == "interrupted")
    return SubagentJobHistoryExecution::Interrupted;
  return std::nullopt;
}

bool required_id(std::string_view object, std::string_view key, std::string& out)
{
  auto const value = ava::core::json::string_field(object, key);
  if (!value || !valid_history_id(*value))
    return false;
  out = std::move(*value);
  return true;
}

bool required_timestamp(std::string_view object, std::string_view key, std::string& out)
{
  auto const value = ava::core::json::string_field(object, key);
  if (!value || !valid_timestamp(*value))
    return false;
  out = std::move(*value);
  return true;
}

bool optional_string(std::string_view object, std::string_view key, std::optional<std::string>& out, std::size_t max_bytes, bool allow_summary_whitespace)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
  {
    out = std::nullopt;
    return true;
  }
  auto const value = ava::core::json::string_field(object, key);
  if (!value)
    return false;
  out = std::move(*value);
  return valid_optional_bounded_text(out, max_bytes, allow_summary_whitespace);
}

bool optional_bool(std::string_view object, std::string_view key, bool& out)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
  {
    out = false;
    return true;
  }
  if (object.substr(*start, 4) == "true")
  {
    out = true;
    return true;
  }
  if (object.substr(*start, 5) == "false")
  {
    out = false;
    return true;
  }
  return false;
}

bool optional_accounting(std::string_view object, std::string_view key, std::size_t& out)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
  {
    out = 0;
    return true;
  }
  auto const value = ava::core::json::integer_field(object, key);
  if (!value || *value < 0 || static_cast<std::uint64_t>(*value) > kMaxSubagentJobHistoryAccountingValue)
    return false;
  out = static_cast<std::size_t>(*value);
  return true;
}

bool terminal_execution(SubagentJobHistoryExecution execution) noexcept
{
  return execution == SubagentJobHistoryExecution::Completed || execution == SubagentJobHistoryExecution::Failed ||
         execution == SubagentJobHistoryExecution::Canceled || execution == SubagentJobHistoryExecution::Interrupted;
}

ava::core::Result<SubagentJobHistoryRecord> parse_object(std::string_view object)
{
  if (!ava::core::json::is_valid_object(object))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job history object is not valid JSON"));

  auto const schema = ava::core::json::integer_field(object, "schema_version");
  if (!schema || *schema != kSubagentJobHistorySchemaVersion)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job history schema_version is unsupported"));

  SubagentJobHistoryRecord record;
  auto const phase = ava::core::json::string_field(object, "phase");
  auto const mode = ava::core::json::string_field(object, "mode");
  auto const execution = ava::core::json::string_field(object, "execution");
  auto parsed_phase = phase ? parse_phase(*phase) : std::nullopt;
  auto parsed_mode = mode ? parse_mode(*mode) : std::nullopt;
  auto parsed_execution = execution ? parse_execution(*execution) : std::nullopt;
  if (!parsed_phase || !parsed_mode || !parsed_execution)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job history enum is invalid"));
  record.phase = *parsed_phase;
  record.mode = *parsed_mode;
  record.execution = *parsed_execution;

  if (!required_id(object, "job_id", record.job_id) || !required_id(object, "task_id", record.task_id) ||
      !required_id(object, "parent_session_id", record.parent_session_id) || !required_id(object, "child_session_id", record.child_session_id) ||
      !required_id(object, "delivery_id", record.delivery_id) || !required_timestamp(object, "started_at", record.started_at) ||
      !required_timestamp(object, "updated_at", record.updated_at))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job history identity or timestamp is invalid"));
  }

  if (!optional_string(object, "terminal_at", record.terminal_at, kMaxSubagentJobHistoryTimestampBytes, false) ||
      (record.terminal_at && !valid_timestamp(*record.terminal_at)))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job history terminal_at is invalid"));
  }
  if (!optional_string(object, "summary", record.summary, kMaxSubagentJobHistorySummaryBytes, true) ||
      !optional_string(object, "error", record.error, kMaxSubagentJobHistoryErrorBytes, true) ||
      !optional_string(object, "error_category", record.error_category, kMaxSubagentJobHistoryIdBytes, false) ||
      !optional_string(object, "stop_reason", record.stop_reason, kMaxSubagentJobHistoryStopReasonBytes, true) ||
      !optional_bool(object, "summary_truncated", record.summary_truncated) || !optional_bool(object, "error_truncated", record.error_truncated) ||
      !optional_bool(object, "stop_reason_truncated", record.stop_reason_truncated) ||
      !optional_accounting(object, "provider_iterations", record.provider_iterations) || !optional_accounting(object, "tool_calls", record.tool_calls) ||
      !optional_accounting(object, "tool_iterations", record.tool_iterations))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job history field type or bound is invalid"));
  }

  if (record.phase == SubagentJobHistoryPhase::Start)
  {
    if (record.execution != SubagentJobHistoryExecution::Starting || record.terminal_at)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job start history has a terminal shape"));
  }
  else if (!terminal_execution(record.execution) || !record.terminal_at)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job terminal history is missing a terminal outcome"));
  }
  return record;
}

void append_string_field(std::string& json, std::string_view key, std::string_view value)
{
  json += ",\"";
  json += key;
  json += "\":\"";
  json += ava::core::json::escape(value);
  json += '"';
}

void append_optional_string_field(std::string& json, std::string_view key, std::optional<std::string> const& value)
{
  if (!value)
    return;
  append_string_field(json, key, *value);
}

void append_bool_field(std::string& json, std::string_view key, bool value)
{
  json += ",\"";
  json += key;
  json += "\":";
  json += value ? "true" : "false";
}

void append_accounting_field(std::string& json, std::string_view key, std::size_t value)
{
  json += ",\"";
  json += key;
  json += "\":";
  json += std::to_string(value);
}

ava::core::VoidResult validate_record(SubagentJobHistoryRecord const& record)
{
  if (!valid_history_id(record.job_id) || !valid_history_id(record.task_id) || !valid_history_id(record.parent_session_id) ||
      !valid_history_id(record.child_session_id) || !valid_history_id(record.delivery_id) || !valid_timestamp(record.started_at) ||
      !valid_timestamp(record.updated_at) || (record.terminal_at && !valid_timestamp(*record.terminal_at)) ||
      !valid_optional_bounded_text(record.summary, kMaxSubagentJobHistorySummaryBytes, true) ||
      !valid_optional_bounded_text(record.error, kMaxSubagentJobHistoryErrorBytes, true) ||
      !valid_optional_bounded_text(record.error_category, kMaxSubagentJobHistoryIdBytes, false) ||
      !valid_optional_bounded_text(record.stop_reason, kMaxSubagentJobHistoryStopReasonBytes, true) ||
      record.provider_iterations > kMaxSubagentJobHistoryAccountingValue || record.tool_calls > kMaxSubagentJobHistoryAccountingValue ||
      record.tool_iterations > kMaxSubagentJobHistoryAccountingValue)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job history record exceeds bounds or has an invalid identity"));
  }
  if (record.phase == SubagentJobHistoryPhase::Start)
  {
    if (record.execution != SubagentJobHistoryExecution::Starting || record.terminal_at)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job start history has a terminal shape"));
  }
  else if (!terminal_execution(record.execution) || !record.terminal_at)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job terminal history is missing a terminal outcome"));
  }
  return {};
}

}  // namespace

std::string_view to_string(SubagentJobHistoryPhase value) noexcept
{
  switch (value)
  {
    case SubagentJobHistoryPhase::Start:
      return "start";
    case SubagentJobHistoryPhase::Terminal:
      return "terminal";
  }
  return "unknown";
}

std::string_view to_string(SubagentJobHistoryMode value) noexcept
{
  switch (value)
  {
    case SubagentJobHistoryMode::Foreground:
      return "foreground";
    case SubagentJobHistoryMode::Background:
      return "background";
  }
  return "unknown";
}

std::string_view to_string(SubagentJobHistoryExecution value) noexcept
{
  switch (value)
  {
    case SubagentJobHistoryExecution::Starting:
      return "starting";
    case SubagentJobHistoryExecution::Running:
      return "running";
    case SubagentJobHistoryExecution::Completed:
      return "completed";
    case SubagentJobHistoryExecution::Failed:
      return "failed";
    case SubagentJobHistoryExecution::Canceled:
      return "canceled";
    case SubagentJobHistoryExecution::Interrupted:
      return "interrupted";
  }
  return "unknown";
}

bool session_metadata_has_subagent_job_history(SessionEntry const& entry)
{
  return entry.type == EntryType::SessionMetadata && ava::core::json::field_value_start(entry.data_json, "subagent_job").has_value();
}

bool valid_subagent_job_history_object(std::string_view object)
{
  return parse_object(object).has_value();
}

ava::core::Result<SessionEntry> make_subagent_job_history_entry(SubagentJobHistoryRecord record)
{
  if (auto valid = validate_record(record); !valid)
    return std::unexpected(std::move(valid.error()));

  std::string nested = "{\"schema_version\":1";
  append_string_field(nested, "phase", to_string(record.phase));
  append_string_field(nested, "job_id", record.job_id);
  append_string_field(nested, "task_id", record.task_id);
  append_string_field(nested, "parent_session_id", record.parent_session_id);
  append_string_field(nested, "child_session_id", record.child_session_id);
  append_string_field(nested, "delivery_id", record.delivery_id);
  append_string_field(nested, "mode", to_string(record.mode));
  append_string_field(nested, "execution", to_string(record.execution));
  append_string_field(nested, "started_at", record.started_at);
  append_string_field(nested, "updated_at", record.updated_at);
  append_optional_string_field(nested, "terminal_at", record.terminal_at);
  append_optional_string_field(nested, "summary", record.summary);
  if (record.summary_truncated)
    append_bool_field(nested, "summary_truncated", true);
  append_optional_string_field(nested, "error", record.error);
  if (record.error_truncated)
    append_bool_field(nested, "error_truncated", true);
  append_optional_string_field(nested, "error_category", record.error_category);
  append_optional_string_field(nested, "stop_reason", record.stop_reason);
  if (record.stop_reason_truncated)
    append_bool_field(nested, "stop_reason_truncated", true);
  if (record.phase == SubagentJobHistoryPhase::Terminal)
  {
    append_accounting_field(nested, "provider_iterations", record.provider_iterations);
    append_accounting_field(nested, "tool_calls", record.tool_calls);
    append_accounting_field(nested, "tool_iterations", record.tool_iterations);
  }
  nested += '}';

  std::string data = "{\"schema_version\":1,\"subagent_job\":";
  data += nested;
  data += '}';

  return SessionEntry{
      .id = ava::core::make_id("entry"), .parent_id = "", .type = EntryType::SessionMetadata, .timestamp = now_timestamp(), .data_json = std::move(data)};
}

ava::core::Result<std::optional<SubagentJobHistoryRecord>> parse_subagent_job_history_entry(SessionEntry const& entry)
{
  if (entry.type != EntryType::SessionMetadata)
    return std::optional<SubagentJobHistoryRecord>{};
  auto const start = ava::core::json::field_value_start(entry.data_json, "subagent_job");
  if (!start)
    return std::optional<SubagentJobHistoryRecord>{};
  auto const object = ava::core::json::object_field(entry.data_json, "subagent_job");
  if (!object)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent_job history field must be an object"));
  auto parsed = parse_object(*object);
  if (!parsed)
    return std::unexpected(std::move(parsed.error()));
  return std::optional<SubagentJobHistoryRecord>(std::move(*parsed));
}

std::vector<SubagentJobHistoryView> project_subagent_job_history(std::string_view parent_session_id, std::vector<SessionEntry> const& entries,
                                                                 std::size_t max_jobs)
{
  std::vector<SubagentJobHistoryView> result;
  if (parent_session_id.empty() || max_jobs == 0)
    return result;

  std::vector<std::string> order;
  std::unordered_map<std::string, SubagentJobHistoryView> by_id;
  for (auto const& entry : entries)
  {
    auto parsed = parse_subagent_job_history_entry(entry);
    if (!parsed || !*parsed)
      continue;
    auto record = std::move(**parsed);
    if (record.parent_session_id != parent_session_id)
      continue;
    auto found = by_id.find(record.job_id);
    if (found == by_id.end())
    {
      order.push_back(record.job_id);
      SubagentJobHistoryView view;
      view.record = std::move(record);
      view.unmatched_start = view.record.phase == SubagentJobHistoryPhase::Start;
      if (view.unmatched_start)
        view.record.execution = SubagentJobHistoryExecution::Interrupted;
      by_id.emplace(order.back(), std::move(view));
      continue;
    }
    bool const unmatched = record.phase == SubagentJobHistoryPhase::Start;
    found->second.record = std::move(record);
    found->second.unmatched_start = unmatched;
    if (unmatched)
      found->second.record.execution = SubagentJobHistoryExecution::Interrupted;
  }

  auto const first = order.size() > max_jobs ? order.size() - max_jobs : 0;
  result.reserve(order.size() - first);
  for (std::size_t index = first; index < order.size(); ++index)
  {
    auto found = by_id.find(order[index]);
    if (found != by_id.end())
      result.push_back(std::move(found->second));
  }
  return result;
}

}  // namespace ava::session
