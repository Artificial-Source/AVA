#include "sys.h"
#include "ava/session/assistant_output.h"
#include "ava/session/Stats.h"
#include "ava/core/json.h"

#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace ava::session {
namespace {

void add_optional_integer(std::optional<long long>& total, std::optional<long long> value)
{
  if (!value || *value < 0)
    return;
  if (!total)
    total = 0;
  constexpr auto maximum = std::numeric_limits<long long>::max();
  *total = *total > maximum - *value ? maximum : *total + *value;
}

std::optional<long long> integer_field_from_usage_or_entry(std::string_view data_json, std::string_view key)
{
  if (auto const usage = ava::core::json::object_field(data_json, "usage"))
  {
    if (auto const value = ava::core::json::integer_field(*usage, key))
      return value;
  }
  return ava::core::json::integer_field(data_json, key);
}

std::optional<long long> integer_field_from_usage_or_entry(std::string_view data_json, std::initializer_list<std::string_view> keys)
{
  for (auto const key : keys)
  {
    if (auto const value = integer_field_from_usage_or_entry(data_json, key))
      return value;
  }
  return std::nullopt;
}

bool bool_field_is_true(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  return start && object.substr(*start, 4) == "true";
}

bool usage_is_estimated(std::string_view usage)
{
  if (bool_field_is_true(usage, "estimated"))
    return true;
  return ava::core::json::string_field(usage, "source").value_or("") == "estimated";
}

bool positive_integer_field(std::string_view object, std::string_view key)
{
  auto const value = ava::core::json::integer_field(object, key);
  return value && *value > 0;
}

bool object_has_billable_tokens(std::string_view object)
{
  return positive_integer_field(object, "input_tokens") || positive_integer_field(object, "output_tokens") ||
         positive_integer_field(object, "reasoning_tokens") || positive_integer_field(object, "cache_read_tokens") ||
         positive_integer_field(object, "cache_read_input_tokens") || positive_integer_field(object, "cache_write_tokens") ||
         positive_integer_field(object, "cache_creation_input_tokens") || positive_integer_field(object, "total_tokens");
}

bool is_number_delimiter(char ch)
{
  return ch == ',' || ch == '}' || ch == ']' || std::isspace(static_cast<unsigned char>(ch)) != 0;
}

std::optional<long double> number_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || *start >= object.size())
    return std::nullopt;

  std::size_t index = *start;
  if (object[index] == '-')
    ++index;
  auto const digits_start = index;
  while (index < object.size() && std::isdigit(static_cast<unsigned char>(object[index])) != 0) ++index;
  if (index == digits_start)
    return std::nullopt;
  if (index < object.size() && object[index] == '.')
  {
    ++index;
    auto const fraction_start = index;
    while (index < object.size() && std::isdigit(static_cast<unsigned char>(object[index])) != 0) ++index;
    if (index == fraction_start)
      return std::nullopt;
  }
  if (index < object.size() && (object[index] == 'e' || object[index] == 'E'))
  {
    ++index;
    if (index < object.size() && (object[index] == '+' || object[index] == '-'))
      ++index;
    auto const exponent_start = index;
    while (index < object.size() && std::isdigit(static_cast<unsigned char>(object[index])) != 0) ++index;
    if (index == exponent_start)
      return std::nullopt;
  }
  if (index < object.size() && !is_number_delimiter(object[index]))
    return std::nullopt;

  try
  {
    return std::stold(std::string(object.substr(*start, index - *start)));
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::optional<long double> cost_field_from_usage_or_entry(std::string_view data_json)
{
  if (auto const usage = ava::core::json::object_field(data_json, "usage"))
  {
    if (auto const value = number_field(*usage, "cost_usd"))
      return value;
    if (auto const value = number_field(*usage, "total_cost_usd"))
      return value;
  }
  if (auto const value = number_field(data_json, "cost_usd"))
    return value;
  return number_field(data_json, "total_cost_usd");
}

void add_optional_cost(std::optional<long double>& total, std::optional<long double> value)
{
  if (!value || *value < 0.0L || !std::isfinite(*value))
    return;
  if (!total)
    total = 0.0L;
  constexpr auto maximum = std::numeric_limits<long double>::max();
  if (!std::isfinite(*total) || *total > maximum - *value)
    *total = maximum;
  else
    *total += *value;
}

void add_usage_and_cost(Stats& stats, std::string_view data_json, bool is_assistant_message)
{
  auto const usage = ava::core::json::object_field(data_json, "usage");
  if (usage)
  {
    if (usage_is_estimated(*usage))
    {
      add_optional_integer(stats.estimated_input_bytes, integer_field_from_usage_or_entry(data_json, "estimated_input_bytes"));
      add_optional_integer(stats.estimated_output_bytes, integer_field_from_usage_or_entry(data_json, "estimated_output_bytes"));
      add_optional_integer(stats.estimated_total_bytes, integer_field_from_usage_or_entry(data_json, "estimated_total_bytes"));
      ++stats.estimated_usage_entries;
      return;
    }
    ++stats.exact_usage_entries;
  }

  add_optional_integer(stats.input_tokens, integer_field_from_usage_or_entry(data_json, "input_tokens"));
  add_optional_integer(stats.output_tokens, integer_field_from_usage_or_entry(data_json, "output_tokens"));
  add_optional_integer(stats.reasoning_tokens, integer_field_from_usage_or_entry(data_json, "reasoning_tokens"));
  add_optional_integer(stats.cache_read_tokens, integer_field_from_usage_or_entry(data_json, {"cache_read_tokens", "cache_read_input_tokens"}));
  add_optional_integer(stats.cache_write_tokens, integer_field_from_usage_or_entry(data_json, {"cache_write_tokens", "cache_creation_input_tokens"}));
  add_optional_integer(stats.total_tokens, integer_field_from_usage_or_entry(data_json, "total_tokens"));

  auto const cost = cost_field_from_usage_or_entry(data_json);
  bool const has_known_cost = cost && *cost >= 0.0L;
  if (has_known_cost)
    add_optional_cost(stats.known_cost_usd, cost);
  bool const has_billable_usage_tokens = usage && object_has_billable_tokens(*usage);
  bool const has_billable_legacy_assistant_tokens = !usage && is_assistant_message && object_has_billable_tokens(data_json);
  if ((has_billable_usage_tokens || has_billable_legacy_assistant_tokens) && !has_known_cost)
    ++stats.unknown_cost_entries;
}

void add_timestamp(Stats& stats, std::string_view timestamp)
{
  if (stats.first_timestamp.empty())
    stats.first_timestamp = timestamp;
  stats.last_timestamp = timestamp;
}

}  // namespace

ava::core::Result<Stats> compute_session_stats(std::vector<SessionEntry> const& entries)
{
  Stats stats;
  auto const assistant_output = classify_assistant_output(entries);
  for (auto const& diagnostic : assistant_output.diagnostics)
  {
    if (diagnostic.severity != AssistantOutputDiagnosticSeverity::Error)
      continue;
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "cannot compute stats from malformed assistant-output session history");
    error.with_context("diagnostic_kind", std::string(to_string(diagnostic.kind)))
        .with_context("diagnostic_entry_id", diagnostic.entry_id)
        .with_context("diagnostic", diagnostic.message);
    return std::unexpected(std::move(error));
  }

  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    auto const& entry = entries[index];
    if (entry.type == EntryType::AssistantOutputItem)
      continue;
    if (entry.type == EntryType::AssistantTurnCommit)
    {
      auto const* turn = assistant_output.find_turn_by_commit_index(index);
      if (!turn)
        continue;

      // A committed v4 group has the same logical persistence shape as its
      // v3 counterpart: one assistant message plus standalone reasoning and
      // tool-call records. Its commit owns response accounting and timestamp.
      ++stats.entry_count;
      ++stats.counts.assistant_message;
      add_timestamp(stats, entry.timestamp);
      for (auto const& output_item : turn->items)
      {
        if (std::holds_alternative<AssistantOutputReasoning>(output_item.item.payload))
        {
          ++stats.entry_count;
          ++stats.counts.reasoning_block;
        }
        else if (std::holds_alternative<AssistantOutputFunctionCall>(output_item.item.payload))
        {
          ++stats.entry_count;
          ++stats.counts.tool_call;
        }
      }
      add_usage_and_cost(stats, entry.data_json, true);
      continue;
    }

    ++stats.entry_count;
    add_timestamp(stats, entry.timestamp);
    switch (entry.type)
    {
      case EntryType::SessionStart:
        ++stats.counts.session_start;
        break;
      case EntryType::SessionMetadata:
        ++stats.counts.session_metadata;
        break;
      case EntryType::UserMessage:
        if (!is_internal_replay_user_message(entry))
          ++stats.counts.user_message;
        break;
      case EntryType::AssistantMessage:
        ++stats.counts.assistant_message;
        break;
      case EntryType::ToolCall:
        ++stats.counts.tool_call;
        break;
      case EntryType::ToolResult:
        ++stats.counts.tool_result;
        break;
      case EntryType::PermissionDecision:
        ++stats.counts.permission_decision;
        break;
      case EntryType::ModeChange:
        ++stats.counts.mode_change;
        break;
      case EntryType::ModelChange:
        ++stats.counts.model_change;
        break;
      case EntryType::ReasoningBlock:
        ++stats.counts.reasoning_block;
        break;
      case EntryType::ReasoningChange:
        ++stats.counts.reasoning_change;
        break;
      case EntryType::AssistantOutputItem:
      case EntryType::AssistantTurnCommit:
        break;
      case EntryType::Compaction:
        ++stats.counts.compaction;
        break;
      case EntryType::BranchSummary:
        ++stats.counts.branch_summary;
        break;
      case EntryType::Error:
        ++stats.counts.error;
        break;
      case EntryType::Cancel:
        ++stats.counts.cancel;
        break;
    }
    add_usage_and_cost(stats, entry.data_json, entry.type == EntryType::AssistantMessage);
  }

  stats.cost_complete = stats.unknown_cost_entries == 0;
  if (stats.cost_complete)
    stats.total_cost_usd = stats.known_cost_usd;

  return stats;
}

}  // namespace ava::session
