#include "sys.h"
#include "ava/debug/print_members_on.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/core/ascii.h"
#include "ava/core/json.h"
#include "ava/core/openai_wire.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ava::session {
namespace {

inline constexpr std::size_t kMaxAssistantTurnIdBytes = 128;
inline constexpr std::size_t kMaxAssistantOutputIdentifierBytes = 256;
inline constexpr std::size_t kMaxAssistantOutputContentBytes = 256U * 1024U;
inline constexpr std::size_t kMaxAssistantOutputUsageBytes = 256U * 1024U;

ava::core::Error codec_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Session, std::move(message));
}

void skip_json_ws(std::string_view value, std::size_t& cursor)
{
  while (cursor < value.size() && std::isspace(static_cast<unsigned char>(value[cursor])) != 0) ++cursor;
}

std::optional<std::size_t> json_string_end(std::string_view value, std::size_t start)
{
  if (start >= value.size() || value[start] != '"')
    return std::nullopt;
  bool escaped = false;
  for (std::size_t cursor = start + 1; cursor < value.size(); ++cursor)
  {
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (value[cursor] == '\\')
    {
      escaped = true;
      continue;
    }
    if (value[cursor] == '"')
      return cursor;
  }
  return std::nullopt;
}

std::optional<std::size_t> json_balanced_end(std::string_view value, std::size_t start, char open, char close)
{
  if (start >= value.size() || value[start] != open)
    return std::nullopt;
  bool in_string = false;
  bool escaped = false;
  std::size_t depth = 0;
  for (std::size_t cursor = start; cursor < value.size(); ++cursor)
  {
    char const ch = value[cursor];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (in_string && ch == '\\')
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
        return cursor;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> json_value_end(std::string_view value, std::size_t start)
{
  skip_json_ws(value, start);
  if (start >= value.size())
    return std::nullopt;
  if (value[start] == '"')
    return json_string_end(value, start);
  if (value[start] == '{')
    return json_balanced_end(value, start, '{', '}');
  if (value[start] == '[')
    return json_balanced_end(value, start, '[', ']');

  std::size_t end = start;
  while (end < value.size() && value[end] != ',' && value[end] != '}') ++end;
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
  if (end == start)
    return std::nullopt;
  return end - 1;
}

template <std::size_t N>
bool object_has_only_keys(std::string_view object, std::array<std::string_view, N> const& allowed_keys)
{
  if (!ava::core::json::is_valid_object(object))
    return false;

  std::unordered_set<std::string> seen;
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
    auto const raw_key = object.substr(cursor + 1, *key_end - cursor - 1);
    // Schema keys are ASCII literals. Reject escaped spellings so the strict
    // codec has one canonical field identity and can prove duplicate absence.
    if (raw_key.find('\\') != std::string_view::npos)
      return false;
    bool const allowed = std::find(allowed_keys.begin(), allowed_keys.end(), raw_key) != allowed_keys.end();
    if (!allowed || !seen.emplace(raw_key).second)
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
    return cursor < object.size() && object[cursor] == '}';
  }
  return false;
}

bool valid_identifier(std::string_view value, std::size_t max_bytes)
{
  return !value.empty() && value.size() <= max_bytes && !ava::core::has_c0_or_del(value);
}

bool has_field(std::string_view object, std::string_view key)
{
  return ava::core::json::field_value_start(object, key).has_value();
}

std::optional<std::size_t> nonnegative_size_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || *start >= object.size() || !std::isdigit(static_cast<unsigned char>(object[*start])))
    return std::nullopt;

  std::size_t end = *start;
  while (end < object.size() && std::isdigit(static_cast<unsigned char>(object[end])) != 0) ++end;
  if (end - *start > 1 && object[*start] == '0')
    return std::nullopt;
  std::size_t delimiter = end;
  skip_json_ws(object, delimiter);
  if (delimiter >= object.size() || (object[delimiter] != ',' && object[delimiter] != '}'))
    return std::nullopt;

  unsigned long long parsed = 0;
  auto const conversion = std::from_chars(object.data() + *start, object.data() + end, parsed);
  if (conversion.ec != std::errc{} || conversion.ptr != object.data() + end || parsed > std::numeric_limits<std::size_t>::max())
    return std::nullopt;
  return static_cast<std::size_t>(parsed);
}

std::optional<bool> boolean_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::nullopt;
  auto value_matches = [&](std::string_view literal) {
    std::size_t end = *start + literal.size();
    if (end > object.size() || object.substr(*start, literal.size()) != literal)
      return false;
    skip_json_ws(object, end);
    return end < object.size() && (object[end] == ',' || object[end] == '}');
  };
  if (value_matches("true"))
    return true;
  if (value_matches("false"))
    return false;
  return std::nullopt;
}

bool nonnegative_number_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || *start >= object.size() || object[*start] == '-')
    return false;
  std::size_t end = *start;
  while (end < object.size() && object[end] != ',' && object[end] != '}') ++end;
  while (end > *start && std::isspace(static_cast<unsigned char>(object[end - 1])) != 0) --end;
  if (end == *start)
    return false;
  std::string const raw(object.substr(*start, end - *start));
  char* parse_end = nullptr;
  auto const parsed = std::strtold(raw.c_str(), &parse_end);
  return parse_end == raw.c_str() + raw.size() && std::isfinite(parsed) && parsed >= 0.0L;
}

bool valid_optional_string(std::string_view object, std::string_view key, std::size_t max_bytes, bool allow_empty, bool reject_controls)
{
  if (!has_field(object, key))
    return true;
  auto const value = ava::core::json::string_field(object, key);
  return value && value->size() <= max_bytes && (allow_empty || !value->empty()) && (!reject_controls || !ava::core::has_c0_or_del(*value));
}

bool nonnegative_signed_integer_field(std::string_view object, std::string_view key)
{
  auto const value = nonnegative_size_field(object, key);
  return value && *value <= static_cast<std::size_t>(std::numeric_limits<long long>::max());
}

bool valid_usage_json(std::string_view usage)
{
  constexpr std::array<std::string_view, 15> allowed_keys = {
      "input_tokens",
      "output_tokens",
      "reasoning_tokens",
      "cache_read_tokens",
      "cache_write_tokens",
      "total_tokens",
      "estimated_input_bytes",
      "estimated_output_bytes",
      "estimated_total_bytes",
      "estimated",
      "source",
      "estimation_method",
      "cost_usd",
      "total_cost_usd",
      "cost_estimated",
  };
  if (usage.size() > kMaxAssistantOutputUsageBytes || !object_has_only_keys(usage, allowed_keys))
    return false;

  for (std::string_view key : {"input_tokens", "output_tokens", "reasoning_tokens", "cache_read_tokens", "cache_write_tokens", "total_tokens",
                               "estimated_input_bytes", "estimated_output_bytes", "estimated_total_bytes"})
  {
    // Downstream stats and RPC expose these as signed values. Do not admit an
    // unsigned value that would vanish or overflow at that boundary.
    if (has_field(usage, key) && !nonnegative_signed_integer_field(usage, key))
      return false;
  }
  for (std::string_view key : {"estimated", "cost_estimated"})
  {
    if (has_field(usage, key) && !boolean_field(usage, key))
      return false;
  }
  if (has_field(usage, "source"))
  {
    auto const source = ava::core::json::string_field(usage, "source");
    if (!source || (*source != "provider" && *source != "estimated"))
      return false;
  }
  if (has_field(usage, "estimation_method"))
  {
    auto const method = ava::core::json::string_field(usage, "estimation_method");
    if (!method || *method != "byte_count")
      return false;
  }
  for (std::string_view key : {"cost_usd", "total_cost_usd"})
  {
    if (has_field(usage, key) && !nonnegative_number_field(usage, key))
      return false;
  }
  return true;
}

bool valid_finish_reason(std::string_view reason) noexcept
{
  return reason == "completed" || reason == "max_tokens" || reason == "tool_calls" || reason == "refusal" || reason == "cancelled" || reason == "error";
}

bool variant_matches_kind(AssistantOutputItem const& item)
{
  switch (item.kind)
  {
    case AssistantOutputItemKind::Text:
      return std::holds_alternative<AssistantOutputText>(item.payload);
    case AssistantOutputItemKind::Reasoning:
      return std::holds_alternative<AssistantOutputReasoning>(item.payload);
    case AssistantOutputItemKind::FunctionCall:
      return std::holds_alternative<AssistantOutputFunctionCall>(item.payload);
  }
  return false;
}

ava::core::Result<AssistantOutputItem> parse_assistant_output_item_data(std::string_view data)
{
  constexpr std::array<std::string_view, 17> allowed_keys = {
      "schema_version", "assistant_turn_id", "sequence",  "kind",          "provider_item_id", "provider_output_index",           "text",    "assistant_phase",
      "format",         "redacted",          "signature", "redacted_data", "native_item_json", "private_replay_metadata_omitted", "call_id", "name",
      "arguments",
  };
  if (!object_has_only_keys(data, allowed_keys))
    return std::unexpected(codec_error("assistant_output_item data must be a strict JSON object with no duplicate or unknown keys"));

  auto const schema_version = nonnegative_size_field(data, "schema_version");
  auto const turn_id = ava::core::json::string_field(data, "assistant_turn_id");
  auto const sequence = nonnegative_size_field(data, "sequence");
  auto const kind = ava::core::json::string_field(data, "kind");
  if (!schema_version || *schema_version != kCurrentAssistantOutputSchemaVersion || !turn_id || !valid_identifier(*turn_id, kMaxAssistantTurnIdBytes) ||
      !sequence || *sequence >= kMaxAssistantOutputItemsPerTurn || !kind)
  {
    return std::unexpected(codec_error("assistant_output_item data is missing required bounded schema_version, assistant_turn_id, sequence, or kind"));
  }

  std::optional<std::string> provider_item_id;
  if (has_field(data, "provider_item_id"))
  {
    provider_item_id = ava::core::json::string_field(data, "provider_item_id");
    if (!provider_item_id || !valid_identifier(*provider_item_id, kMaxAssistantOutputIdentifierBytes))
      return std::unexpected(codec_error("assistant_output_item provider_item_id must be a bounded non-empty identifier"));
  }
  std::optional<std::size_t> provider_output_index;
  if (has_field(data, "provider_output_index"))
  {
    provider_output_index = nonnegative_size_field(data, "provider_output_index");
    if (!provider_output_index || *provider_output_index >= kMaxAssistantOutputItemsPerTurn)
      return std::unexpected(codec_error("assistant_output_item provider_output_index must be a nonnegative integer below 4096"));
  }

  auto const reject_fields = [&](std::initializer_list<std::string_view> fields, std::string_view message) -> ava::core::VoidResult {
    for (auto const field : fields)
    {
      if (has_field(data, field))
        return std::unexpected(codec_error(std::string(message)));
    }
    return {};
  };

  AssistantOutputItem parsed{.assistant_turn_id = *turn_id,
                             .sequence = *sequence,
                             .provider_item_id = std::move(provider_item_id),
                             .provider_output_index = provider_output_index,
                             .payload = AssistantOutputText{}};
  if (*kind == "text")
  {
    if (auto rejected = reject_fields(
            {"format", "redacted", "signature", "redacted_data", "native_item_json", "private_replay_metadata_omitted", "call_id", "name", "arguments"},
            "assistant_output_item text variant contains incompatible fields");
        !rejected)
      return std::unexpected(std::move(rejected.error()));
    auto const text = ava::core::json::string_field(data, "text");
    auto const phase = ava::core::json::string_field(data, "assistant_phase");
    if (!text || text->size() > kMaxAssistantOutputContentBytes || !phase)
      return std::unexpected(codec_error("assistant_output_item text variant requires bounded text and assistant_phase"));

    AssistantOutputTextPhase parsed_phase = AssistantOutputTextPhase::Unknown;
    if (*phase == "commentary")
      parsed_phase = AssistantOutputTextPhase::Commentary;
    else if (*phase == "final_answer")
      parsed_phase = AssistantOutputTextPhase::FinalAnswer;
    else if (*phase != "unknown")
      return std::unexpected(codec_error("assistant_output_item text assistant_phase is unknown"));

    parsed.kind = AssistantOutputItemKind::Text;
    parsed.payload = AssistantOutputText{.text = *text, .assistant_phase = parsed_phase};
    return parsed;
  }

  if (*kind == "reasoning")
  {
    if (auto rejected =
            reject_fields({"assistant_phase", "call_id", "name", "arguments"}, "assistant_output_item reasoning variant contains incompatible fields");
        !rejected)
      return std::unexpected(std::move(rejected.error()));
    auto const text = ava::core::json::string_field(data, "text");
    auto const format = ava::core::json::string_field(data, "format");
    auto const redacted = boolean_field(data, "redacted");
    auto const private_replay_metadata_omitted =
        has_field(data, "private_replay_metadata_omitted") ? boolean_field(data, "private_replay_metadata_omitted") : std::optional<bool>{false};
    if (!private_replay_metadata_omitted || (!*private_replay_metadata_omitted && has_field(data, "private_replay_metadata_omitted")))
      return std::unexpected(codec_error("assistant_output_item reasoning private_replay_metadata_omitted must be true when present"));
    if (!text || text->size() > kMaxAssistantOutputContentBytes || !format || !valid_identifier(*format, kMaxAssistantOutputIdentifierBytes) || !redacted)
      return std::unexpected(codec_error("assistant_output_item reasoning variant requires text, format, and redacted"));
    if (!valid_optional_string(data, "signature", kMaxAssistantOutputContentBytes, true, false) ||
        !valid_optional_string(data, "redacted_data", kMaxAssistantOutputContentBytes, true, false) ||
        !valid_optional_string(data, "native_item_json", kMaxAssistantOutputContentBytes, true, false))
    {
      return std::unexpected(codec_error("assistant_output_item reasoning private fields are invalid or exceed their bound"));
    }

    auto const signature = has_field(data, "signature") ? ava::core::json::string_field(data, "signature") : std::nullopt;
    auto const redacted_data = has_field(data, "redacted_data") ? ava::core::json::string_field(data, "redacted_data") : std::nullopt;
    auto native_item_json = has_field(data, "native_item_json") ? ava::core::json::string_field(data, "native_item_json") : std::nullopt;
    if (*private_replay_metadata_omitted && (signature || redacted_data || native_item_json))
      return std::unexpected(codec_error("portable assistant_output_item reasoning marker forbids retained private replay fields"));
    if (text->empty() && (!signature || signature->empty()) && (!redacted_data || redacted_data->empty()) && (!native_item_json || native_item_json->empty()))
    {
      return std::unexpected(codec_error("assistant_output_item reasoning requires non-empty text, signature, redacted_data, or native_item_json"));
    }
    if (native_item_json)
    {
      bool const valid_native = *format == "openai_responses" ? ava::core::is_valid_openai_native_reasoning_item_json(*native_item_json)
                                                              : ava::core::json::is_valid_object(*native_item_json) &&
                                                                    ava::core::json::string_field(*native_item_json, "type").value_or("") == "reasoning";
      if (!valid_native)
        return std::unexpected(codec_error("assistant_output_item reasoning native_item_json is not a valid provider reasoning object"));
    }

    parsed.kind = AssistantOutputItemKind::Reasoning;
    parsed.payload = AssistantOutputReasoning{.text = *text,
                                              .format = *format,
                                              .redacted = *redacted,
                                              .signature = signature,
                                              .redacted_data = redacted_data,
                                              .native_item_json = std::move(native_item_json),
                                              .private_replay_metadata_omitted = *private_replay_metadata_omitted};
    return parsed;
  }

  if (*kind == "function_call")
  {
    if (auto rejected = reject_fields(
            {"text", "assistant_phase", "format", "redacted", "signature", "redacted_data", "native_item_json", "private_replay_metadata_omitted"},
            "assistant_output_item function_call variant contains incompatible fields");
        !rejected)
      return std::unexpected(std::move(rejected.error()));
    auto const call_id = ava::core::json::string_field(data, "call_id");
    auto const name = ava::core::json::string_field(data, "name");
    auto const arguments = ava::core::json::string_field(data, "arguments");
    if (!call_id || !valid_identifier(*call_id, kMaxAssistantOutputIdentifierBytes) || !name || !valid_identifier(*name, kMaxAssistantOutputIdentifierBytes) ||
        !arguments || arguments->size() > kMaxAssistantOutputContentBytes || !ava::core::json::is_valid_object(*arguments))
    {
      return std::unexpected(codec_error("assistant_output_item function_call requires bounded call_id, name, and JSON-object arguments"));
    }
    parsed.kind = AssistantOutputItemKind::FunctionCall;
    parsed.payload = AssistantOutputFunctionCall{.call_id = *call_id, .name = *name, .arguments_json = *arguments};
    return parsed;
  }

  return std::unexpected(codec_error("assistant_output_item kind is unknown"));
}

ava::core::Result<AssistantTurnCommit> parse_assistant_turn_commit_data(std::string_view data)
{
  constexpr std::array<std::string_view, 9> allowed_keys = {
      "schema_version", "assistant_turn_id", "item_count", "provider", "model", "api_family", "reasoning_format", "finish_reason", "usage",
  };
  if (!object_has_only_keys(data, allowed_keys))
    return std::unexpected(codec_error("assistant_turn_commit data must be a strict JSON object with no duplicate or unknown keys"));

  auto const schema_version = nonnegative_size_field(data, "schema_version");
  auto const turn_id = ava::core::json::string_field(data, "assistant_turn_id");
  auto const item_count = nonnegative_size_field(data, "item_count");
  auto const provider = ava::core::json::string_field(data, "provider");
  auto const model = ava::core::json::string_field(data, "model");
  auto const finish_reason = ava::core::json::string_field(data, "finish_reason");
  if (!schema_version || *schema_version != kCurrentAssistantOutputSchemaVersion || !turn_id || !valid_identifier(*turn_id, kMaxAssistantTurnIdBytes) ||
      !item_count || *item_count > kMaxAssistantOutputItemsPerTurn || !provider || !valid_identifier(*provider, kMaxAssistantOutputIdentifierBytes) || !model ||
      !valid_identifier(*model, kMaxAssistantOutputIdentifierBytes) || !finish_reason || !valid_finish_reason(*finish_reason))
  {
    return std::unexpected(codec_error("assistant_turn_commit data is missing required bounded turn, count, provider, model, or finish metadata"));
  }

  std::optional<std::string> api_family;
  if (has_field(data, "api_family"))
  {
    api_family = ava::core::json::string_field(data, "api_family");
    if (!api_family || !valid_identifier(*api_family, kMaxAssistantOutputIdentifierBytes))
      return std::unexpected(codec_error("assistant_turn_commit api_family must be a bounded string when present"));
  }
  std::optional<std::string> reasoning_format;
  if (has_field(data, "reasoning_format"))
  {
    reasoning_format = ava::core::json::string_field(data, "reasoning_format");
    if (!reasoning_format || !valid_identifier(*reasoning_format, kMaxAssistantOutputIdentifierBytes))
      return std::unexpected(codec_error("assistant_turn_commit reasoning_format must be a bounded string when present"));
  }

  std::optional<std::string> usage_json;
  if (has_field(data, "usage"))
  {
    usage_json = ava::core::json::object_field(data, "usage");
    if (!usage_json || !valid_usage_json(*usage_json))
      return std::unexpected(codec_error("assistant_turn_commit usage must use the bounded established usage/cost object shape"));
  }

  return AssistantTurnCommit{.assistant_turn_id = *turn_id,
                             .item_count = *item_count,
                             .provider = *provider,
                             .model = *model,
                             .api_family = std::move(api_family),
                             .reasoning_format = std::move(reasoning_format),
                             .finish_reason = *finish_reason,
                             .usage_json = std::move(usage_json)};
}

ava::core::VoidResult validate_v4_entry(SessionEntry const& entry, EntryType expected_type)
{
  if (entry.type != expected_type)
    return std::unexpected(codec_error("session entry has the wrong v4 assistant-output type"));
  if (entry.version < 4 || entry.version > kCurrentSessionEntryVersion)
    return std::unexpected(codec_error("v4 assistant-output entry requires a supported session version 4 or newer"));
  return {};
}

void add_diagnostic(AssistantOutputProjection& projection, AssistantOutputDiagnosticSeverity severity, AssistantOutputDiagnosticKind kind,
                    std::size_t entry_index, std::string entry_id, std::string message)
{
  projection.diagnostics.push_back(AssistantOutputDiagnostic{
      .severity = severity, .kind = kind, .entry_index = entry_index, .entry_id = std::move(entry_id), .message = std::move(message)});
}

void add_diagnostic(AssistantOutputProjection& projection, AssistantOutputDiagnosticSeverity severity, AssistantOutputDiagnosticKind kind,
                    std::size_t entry_index, SessionEntry const& entry, std::string message)
{
  add_diagnostic(projection, severity, kind, entry_index, entry.id, std::move(message));
}

struct PendingAssistantOutputGroup
{
  std::size_t start_index = 0;
  std::vector<CommittedAssistantOutputItem> items;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

bool group_has_dense_sequence(PendingAssistantOutputGroup const& group)
{
  for (std::size_t index = 0; index < group.items.size(); ++index)
  {
    if (group.items[index].item.sequence != index)
      return false;
  }
  return true;
}

bool group_has_unique_provider_identity(PendingAssistantOutputGroup const& group, AssistantOutputProjection& projection, std::string_view group_description)
{
  std::unordered_set<std::string> item_ids;
  std::unordered_set<std::size_t> output_indices;
  bool valid = true;
  for (auto const& item : group.items)
  {
    if (item.item.provider_item_id && !item_ids.insert(*item.item.provider_item_id).second)
    {
      add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, item.entry_index,
                     item.entry_id, std::string(group_description) + " duplicates provider_item_id");
      valid = false;
    }
    if (item.item.provider_output_index && !output_indices.insert(*item.item.provider_output_index).second)
    {
      add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, item.entry_index,
                     item.entry_id, std::string(group_description) + " duplicates provider_output_index");
      valid = false;
    }
  }
  return valid;
}

bool group_has_unique_physical_entry_ids(PendingAssistantOutputGroup const& group, AssistantOutputProjection& projection, std::string_view group_description)
{
  std::unordered_set<std::string> entry_ids;
  bool valid = true;
  for (auto const& item : group.items)
  {
    if (!entry_ids.insert(item.entry_id).second)
    {
      add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, item.entry_index,
                     item.entry_id, std::string(group_description) + " duplicates physical output entry id");
      valid = false;
    }
  }
  return valid;
}

bool group_is_valid_staging_prefix(PendingAssistantOutputGroup const& group, AssistantOutputProjection& projection)
{
  bool valid = true;
  auto const& first = group.items.front();
  if (group.items.size() > kMaxAssistantOutputItemsPerTurn)
  {
    add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, first.entry_index,
                   first.entry_id, "final staged assistant turn exceeds 4096 output items");
    valid = false;
  }
  if (!group_has_dense_sequence(group))
  {
    add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, first.entry_index,
                   first.entry_id, "final staged assistant turn sequence values must be dense from zero through item_count minus one");
    valid = false;
  }
  if (!group_has_unique_provider_identity(group, projection, "final staged assistant turn"))
    valid = false;
  if (!group_has_unique_physical_entry_ids(group, projection, "final staged assistant turn"))
    valid = false;
  return valid;
}

ava::core::Error append_state_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Session, std::move(message));
}

bool closes_committed_assistant_output_tool_result_window(EntryType type) noexcept
{
  return type == EntryType::UserMessage || type == EntryType::AssistantMessage || type == EntryType::AssistantOutputItem ||
         type == EntryType::AssistantTurnCommit || type == EntryType::Compaction;
}

}  // namespace

std::string_view to_string(AssistantOutputTextPhase phase) noexcept
{
  switch (phase)
  {
    case AssistantOutputTextPhase::Unknown:
      return "unknown";
    case AssistantOutputTextPhase::Commentary:
      return "commentary";
    case AssistantOutputTextPhase::FinalAnswer:
      return "final_answer";
  }
  return "unknown";
}

std::string_view to_string(AssistantOutputItemKind kind) noexcept
{
  switch (kind)
  {
    case AssistantOutputItemKind::Text:
      return "text";
    case AssistantOutputItemKind::Reasoning:
      return "reasoning";
    case AssistantOutputItemKind::FunctionCall:
      return "function_call";
  }
  return "unknown";
}

std::string_view to_string(AssistantOutputDiagnosticSeverity severity) noexcept
{
  switch (severity)
  {
    case AssistantOutputDiagnosticSeverity::Warning:
      return "warning";
    case AssistantOutputDiagnosticSeverity::Error:
      return "error";
  }
  return "unknown";
}

std::string_view to_string(AssistantOutputDiagnosticKind kind) noexcept
{
  switch (kind)
  {
    case AssistantOutputDiagnosticKind::InvalidAssistantOutputItem:
      return "invalid_assistant_output_item";
    case AssistantOutputDiagnosticKind::InvalidAssistantTurnCommit:
      return "invalid_assistant_turn_commit";
    case AssistantOutputDiagnosticKind::IncompleteAssistantTurn:
      return "incomplete_assistant_turn";
    case AssistantOutputDiagnosticKind::MalformedAssistantTurn:
      return "malformed_assistant_turn";
  }
  return "unknown";
}

ava::core::Result<std::string> serialize_assistant_output_item_data_json(AssistantOutputItem const& item)
{
  if (!variant_matches_kind(item))
    return std::unexpected(codec_error("assistant_output_item kind does not match its payload variant"));

  std::string data = "{\"schema_version\":" + std::to_string(kCurrentAssistantOutputSchemaVersion) + ",\"assistant_turn_id\":\"" +
                     ava::core::json::escape(item.assistant_turn_id) + "\",\"sequence\":" + std::to_string(item.sequence) + ",\"kind\":\"" +
                     std::string(to_string(item.kind)) + "\"";
  if (item.provider_item_id)
    data += ",\"provider_item_id\":\"" + ava::core::json::escape(*item.provider_item_id) + "\"";
  if (item.provider_output_index)
    data += ",\"provider_output_index\":" + std::to_string(*item.provider_output_index);

  if (auto const* text = std::get_if<AssistantOutputText>(&item.payload))
  {
    data += ",\"text\":\"" + ava::core::json::escape(text->text) + "\",\"assistant_phase\":\"" + std::string(to_string(text->assistant_phase)) + "\"";
  }
  else if (auto const* reasoning = std::get_if<AssistantOutputReasoning>(&item.payload))
  {
    data += ",\"text\":\"" + ava::core::json::escape(reasoning->text) + "\",\"format\":\"" + ava::core::json::escape(reasoning->format) +
            "\",\"redacted\":" + (reasoning->redacted ? "true" : "false");
    if (reasoning->signature)
      data += ",\"signature\":\"" + ava::core::json::escape(*reasoning->signature) + "\"";
    if (reasoning->redacted_data)
      data += ",\"redacted_data\":\"" + ava::core::json::escape(*reasoning->redacted_data) + "\"";
    if (reasoning->native_item_json)
      data += ",\"native_item_json\":\"" + ava::core::json::escape(*reasoning->native_item_json) + "\"";
    if (reasoning->private_replay_metadata_omitted)
      data += ",\"private_replay_metadata_omitted\":true";
  }
  else if (auto const* function = std::get_if<AssistantOutputFunctionCall>(&item.payload))
  {
    data += ",\"call_id\":\"" + ava::core::json::escape(function->call_id) + "\",\"name\":\"" + ava::core::json::escape(function->name) +
            "\",\"arguments\":\"" + ava::core::json::escape(function->arguments_json) + "\"";
  }
  data += '}';

  auto parsed = parse_assistant_output_item_data(data);
  if (!parsed)
    return std::unexpected(std::move(parsed.error()));
  return data;
}

ava::core::Result<std::string> serialize_assistant_turn_commit_data_json(AssistantTurnCommit const& commit)
{
  std::string data = "{\"schema_version\":" + std::to_string(kCurrentAssistantOutputSchemaVersion) + ",\"assistant_turn_id\":\"" +
                     ava::core::json::escape(commit.assistant_turn_id) + "\",\"item_count\":" + std::to_string(commit.item_count) + ",\"provider\":\"" +
                     ava::core::json::escape(commit.provider) + "\",\"model\":\"" + ava::core::json::escape(commit.model) + "\"";
  if (commit.api_family)
    data += ",\"api_family\":\"" + ava::core::json::escape(*commit.api_family) + "\"";
  if (commit.reasoning_format)
    data += ",\"reasoning_format\":\"" + ava::core::json::escape(*commit.reasoning_format) + "\"";
  data += ",\"finish_reason\":\"" + ava::core::json::escape(commit.finish_reason) + "\"";
  if (commit.usage_json)
    data += ",\"usage\":" + *commit.usage_json;
  data += '}';

  auto parsed = parse_assistant_turn_commit_data(data);
  if (!parsed)
    return std::unexpected(std::move(parsed.error()));
  return data;
}

ava::core::Result<AssistantOutputItem> parse_assistant_output_item(SessionEntry const& entry)
{
  if (auto valid_entry = validate_v4_entry(entry, EntryType::AssistantOutputItem); !valid_entry)
    return std::unexpected(std::move(valid_entry.error()));
  return parse_assistant_output_item_data(entry.data_json);
}

ava::core::Result<AssistantTurnCommit> parse_assistant_turn_commit(SessionEntry const& entry)
{
  if (auto valid_entry = validate_v4_entry(entry, EntryType::AssistantTurnCommit); !valid_entry)
    return std::unexpected(std::move(valid_entry.error()));
  return parse_assistant_turn_commit_data(entry.data_json);
}

AssistantOutputProjection classify_assistant_output(std::vector<SessionEntry> const& entries)
{
  AssistantOutputProjection projection;
  std::optional<PendingAssistantOutputGroup> pending;
  std::unordered_set<std::string> committed_turn_ids;
  std::unordered_set<std::string> committed_output_entry_ids;

  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    auto const& entry = entries[index];
    if (entry.type == EntryType::AssistantOutputItem)
    {
      auto item = parse_assistant_output_item(entry);
      if (!item)
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::InvalidAssistantOutputItem, index, entry,
                       item.error().message());
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       pending ? "staged assistant turn contains an invalid output item" : "invalid assistant output item cannot form a staged assistant turn");
        pending.reset();
        continue;
      }

      CommittedAssistantOutputItem physical_item{.item = std::move(*item), .entry_index = index, .entry_id = entry.id};
      if (committed_turn_ids.contains(physical_item.item.assistant_turn_id))
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       "staged assistant output item reuses a committed assistant_turn_id");
        pending.reset();
        continue;
      }
      if (committed_output_entry_ids.contains(physical_item.entry_id))
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       "staged assistant output item reuses a committed physical output entry id");
        pending.reset();
        continue;
      }
      if (!pending)
      {
        pending = PendingAssistantOutputGroup{.start_index = index, .items = {std::move(physical_item)}};
        continue;
      }
      if (pending->items.front().item.assistant_turn_id != physical_item.item.assistant_turn_id)
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       "staged assistant turn is followed by an output item for another assistant_turn_id");
        pending = PendingAssistantOutputGroup{.start_index = index, .items = {std::move(physical_item)}};
        continue;
      }
      if (pending->items.size() >= kMaxAssistantOutputItemsPerTurn)
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       "staged assistant turn exceeds 4096 output items");
        pending.reset();
        continue;
      }
      pending->items.push_back(std::move(physical_item));
      continue;
    }

    if (entry.type == EntryType::AssistantTurnCommit)
    {
      auto commit = parse_assistant_turn_commit(entry);
      if (!commit)
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::InvalidAssistantTurnCommit, index, entry,
                       commit.error().message());
        if (pending)
        {
          add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                         "staged assistant turn is not followed by a valid commit");
          pending.reset();
        }
        continue;
      }

      if (!pending)
      {
        if (commit->item_count != 0)
        {
          add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                         "assistant_turn_commit with no staged items must have item_count zero");
          continue;
        }
        if (!committed_turn_ids.insert(commit->assistant_turn_id).second)
        {
          add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                         "assistant_turn_id is duplicated by a committed assistant turn");
          continue;
        }
        auto const turn_index = projection.turns.size();
        projection.turn_indices_by_commit_index.emplace(index, turn_index);
        projection.turns.push_back(
            CommittedAssistantTurn{.start_index = index, .commit_index = index, .commit_entry_id = entry.id, .commit = std::move(*commit), .items = {}});
        continue;
      }

      bool valid = true;
      if (pending->items.front().item.assistant_turn_id != commit->assistant_turn_id)
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       "assistant_turn_commit assistant_turn_id does not match its staged output items");
        valid = false;
      }
      if (pending->items.size() != commit->item_count)
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       "assistant_turn_commit item_count does not match its staged output items");
        valid = false;
      }
      if (!group_has_dense_sequence(*pending))
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       "assistant_output_item sequence values must be dense from zero through item_count minus one");
        valid = false;
      }
      if (!group_has_unique_provider_identity(*pending, projection, "committed assistant turn"))
        valid = false;
      if (!group_has_unique_physical_entry_ids(*pending, projection, "committed assistant turn"))
        valid = false;
      if (committed_turn_ids.contains(commit->assistant_turn_id))
      {
        add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                       "assistant_turn_id is duplicated by a committed assistant turn");
        valid = false;
      }
      std::unordered_set<std::string> group_output_entry_ids;
      for (auto const& item : pending->items)
      {
        if (committed_output_entry_ids.contains(item.entry_id) || !group_output_entry_ids.insert(item.entry_id).second)
        {
          add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, item.entry_index,
                         item.entry_id, "committed assistant output entry id is duplicated");
          valid = false;
        }
      }

      if (valid)
      {
        committed_turn_ids.insert(commit->assistant_turn_id);
        auto const turn_index = projection.turns.size();
        projection.turn_indices_by_commit_index.emplace(index, turn_index);
        for (std::size_t item_index = 0; item_index < pending->items.size(); ++item_index)
        {
          auto const& item = pending->items[item_index];
          committed_output_entry_ids.insert(item.entry_id);
          projection.item_references_by_entry_id.emplace(item.entry_id, AssistantOutputItemReference{.turn_index = turn_index, .item_index = item_index});
        }
        projection.turns.push_back(CommittedAssistantTurn{.start_index = pending->start_index,
                                                          .commit_index = index,
                                                          .commit_entry_id = entry.id,
                                                          .commit = std::move(*commit),
                                                          .items = std::move(pending->items)});
      }
      pending.reset();
      continue;
    }

    if (pending)
    {
      add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Error, AssistantOutputDiagnosticKind::MalformedAssistantTurn, index, entry,
                     "staged assistant turn is followed by an unrelated session entry instead of its commit");
      pending.reset();
    }
  }

  if (pending)
  {
    auto const& first = pending->items.front();
    if (group_is_valid_staging_prefix(*pending, projection))
    {
      add_diagnostic(projection, AssistantOutputDiagnosticSeverity::Warning, AssistantOutputDiagnosticKind::IncompleteAssistantTurn, first.entry_index,
                     first.entry_id, "final staged assistant turn has no commit and is ignored");
    }
  }

  return projection;
}

AssistantOutputToolResultWindow committed_assistant_output_tool_result_window(std::vector<SessionEntry> const& entries,
                                                                              CommittedAssistantTurn const& turn) noexcept
{
  if (turn.commit_index >= entries.size() || turn.commit_index == std::numeric_limits<std::size_t>::max())
    return AssistantOutputToolResultWindow{.begin_index = entries.size(), .end_index = entries.size()};

  auto const begin_index = turn.commit_index + 1;
  auto end_index = entries.size();
  for (std::size_t index = begin_index; index < entries.size(); ++index)
  {
    if (closes_committed_assistant_output_tool_result_window(entries[index].type))
    {
      end_index = index;
      break;
    }
  }
  return AssistantOutputToolResultWindow{.begin_index = begin_index, .end_index = end_index};
}

ava::core::Result<std::vector<UnresolvedCommittedFunctionCall>> find_unresolved_committed_function_calls(std::vector<SessionEntry> const& entries)
{
  auto const projection = classify_assistant_output(entries);
  for (auto const& diagnostic : projection.diagnostics)
  {
    if (diagnostic.severity != AssistantOutputDiagnosticSeverity::Error)
      continue;
    auto error = codec_error("cannot reconcile function calls from malformed assistant-output history");
    error.with_context("diagnostic_kind", std::string(to_string(diagnostic.kind)))
        .with_context("diagnostic_entry_id", diagnostic.entry_id)
        .with_context("diagnostic", diagnostic.message);
    return std::unexpected(std::move(error));
  }

  std::vector<UnresolvedCommittedFunctionCall> unresolved;
  std::unordered_map<std::string, std::size_t> unresolved_indices_by_output_entry_id;
  std::unordered_set<std::string> committed_function_call_ids;
  try
  {
    for (auto const& turn : projection.turns)
    {
      for (auto const& output_item : turn.items)
      {
        auto const* function = std::get_if<AssistantOutputFunctionCall>(&output_item.item.payload);
        if (!function)
          continue;
        auto const index = unresolved.size();
        if (!unresolved_indices_by_output_entry_id.emplace(output_item.entry_id, index).second)
        {
          auto error = codec_error("committed assistant function output entry id is ambiguous");
          error.with_context("assistant_output_entry_id", output_item.entry_id);
          return std::unexpected(std::move(error));
        }
        if (!committed_function_call_ids.insert(function->call_id).second)
        {
          auto error = codec_error("committed assistant function call id is ambiguous");
          error.with_context("call_id", function->call_id).with_context("assistant_output_entry_id", output_item.entry_id);
          return std::unexpected(std::move(error));
        }
        unresolved.push_back(UnresolvedCommittedFunctionCall{.committed_entry_index = turn.commit_index,
                                                             .assistant_output_entry_id = output_item.entry_id,
                                                             .call_id = function->call_id,
                                                             .name = function->name});
      }
    }
  }
  catch (...)
  {
    return std::unexpected(codec_error("failed to enumerate committed assistant function calls"));
  }

  std::vector<bool> result_seen(unresolved.size(), false);
  for (std::size_t entry_index = 0; entry_index < entries.size(); ++entry_index)
  {
    auto const& entry = entries[entry_index];
    if (entry.type != EntryType::ToolResult)
      continue;

    auto const binding_present = ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id");
    auto const call_id = ava::core::json::string_field(entry.data_json, "call_id").value_or("");
    auto const name = ava::core::json::string_field(entry.data_json, "name").value_or("");
    if (!binding_present)
    {
      if (committed_function_call_ids.contains(call_id))
      {
        auto error = codec_error("tool_result for a committed assistant function call is missing its exact output-item binding");
        error.with_context("call_id", call_id).with_context("tool_name", name).with_context("tool_result_entry_id", entry.id);
        return std::unexpected(std::move(error));
      }
      continue;
    }

    auto const output_entry_id = ava::core::json::string_field(entry.data_json, "assistant_output_entry_id");
    if (!output_entry_id || output_entry_id->empty())
    {
      auto error = codec_error("tool_result assistant_output_entry_id must be a nonempty string naming a committed function item");
      error.with_context("tool_result_entry_id", entry.id);
      return std::unexpected(std::move(error));
    }
    auto const found = unresolved_indices_by_output_entry_id.find(*output_entry_id);
    if (found == unresolved_indices_by_output_entry_id.end())
    {
      auto error = codec_error("tool_result assistant_output_entry_id does not name a committed function item");
      error.with_context("assistant_output_entry_id", *output_entry_id).with_context("tool_result_entry_id", entry.id);
      return std::unexpected(std::move(error));
    }

    auto const& function = unresolved[found->second];
    auto const* turn = projection.find_turn_by_output_entry_id(*output_entry_id);
    auto const window = turn ? committed_assistant_output_tool_result_window(entries, *turn) : AssistantOutputToolResultWindow{};
    if (!turn || !window.contains(entry_index) || call_id != function.call_id || name != function.name)
    {
      auto error = codec_error("tool_result does not exactly bind its committed assistant function output item in that turn's result window");
      error.with_context("assistant_output_entry_id", function.assistant_output_entry_id)
          .with_context("call_id", function.call_id)
          .with_context("tool_name", function.name)
          .with_context("tool_result_entry_id", entry.id);
      return std::unexpected(std::move(error));
    }
    if (result_seen[found->second])
    {
      auto error = codec_error("committed assistant function output item has multiple bound tool results");
      error.with_context("assistant_output_entry_id", function.assistant_output_entry_id)
          .with_context("call_id", function.call_id)
          .with_context("tool_name", function.name)
          .with_context("tool_result_entry_id", entry.id);
      return std::unexpected(std::move(error));
    }
    result_seen[found->second] = true;
  }

  // Synthetic terminal results are safe only while every missing call is in
  // the active tail's immediate result window. A later turn (or compaction)
  // changes the logical context; fail before the caller can append even one
  // result for a batch containing such a stale call.
  for (std::size_t index = 0; index < unresolved.size(); ++index)
  {
    if (result_seen[index])
      continue;
    auto const* turn = projection.find_turn_by_output_entry_id(unresolved[index].assistant_output_entry_id);
    auto const window = turn ? committed_assistant_output_tool_result_window(entries, *turn) : AssistantOutputToolResultWindow{};
    if (!turn || window.end_index != entries.size())
    {
      auto error = codec_error("unresolved committed function call is no longer in the active EOF tool-result window");
      error.with_context("assistant_output_entry_id", unresolved[index].assistant_output_entry_id)
          .with_context("call_id", unresolved[index].call_id)
          .with_context("tool_name", unresolved[index].name);
      return std::unexpected(std::move(error));
    }
  }

  std::vector<UnresolvedCommittedFunctionCall> result;
  try
  {
    result.reserve(unresolved.size());
    for (std::size_t index = 0; index < unresolved.size(); ++index)
      if (!result_seen[index])
        result.push_back(std::move(unresolved[index]));
  }
  catch (...)
  {
    return std::unexpected(codec_error("failed to collect unresolved committed function calls"));
  }
  return result;
}

bool AssistantOutputAppendState::ready() const noexcept
{
  return mode_ == Mode::Closed;
}

ava::core::Result<AssistantOutputAppendState> AssistantOutputAppendState::from_validated_history(std::vector<SessionEntry> const& entries)
{
  try
  {
    auto const projection = classify_assistant_output(entries);
    for (auto const& diagnostic : projection.diagnostics)
    {
      if (diagnostic.severity == AssistantOutputDiagnosticSeverity::Warning && diagnostic.kind == AssistantOutputDiagnosticKind::IncompleteAssistantTurn)
        continue;

      auto error = append_state_error("session assistant-output history is malformed; recover it before creating an append target");
      error.with_context("diagnostic_kind", std::string(to_string(diagnostic.kind)))
          .with_context("diagnostic_entry_id", diagnostic.entry_id)
          .with_context("diagnostic", diagnostic.message);
      return std::unexpected(std::move(error));
    }

    AssistantOutputAppendState state;
    for (auto const& turn : projection.turns)
    {
      if (!state.committed_turn_ids_.insert(turn.commit.assistant_turn_id).second)
      {
        auto error = append_state_error("session assistant-output history duplicates a committed assistant_turn_id");
        error.with_context("assistant_turn_id", turn.commit.assistant_turn_id);
        return std::unexpected(std::move(error));
      }
      for (auto const& item : turn.items)
      {
        if (!state.committed_output_entry_ids_.insert(item.entry_id).second)
        {
          auto error = append_state_error("session assistant-output history duplicates a committed physical output entry id");
          error.with_context("entry_id", item.entry_id);
          return std::unexpected(std::move(error));
        }
      }
    }

    bool const has_incomplete_suffix = std::ranges::any_of(projection.diagnostics, [](AssistantOutputDiagnostic const& diagnostic) {
      return diagnostic.severity == AssistantOutputDiagnosticSeverity::Warning && diagnostic.kind == AssistantOutputDiagnosticKind::IncompleteAssistantTurn;
    });
    if (!has_incomplete_suffix)
      return state;

    std::vector<SessionEntry const*> suffix;
    for (auto cursor = entries.rbegin(); cursor != entries.rend() && cursor->type == EntryType::AssistantOutputItem; ++cursor) suffix.push_back(&*cursor);
    if (suffix.empty())
      return std::unexpected(append_state_error("incomplete assistant-output history has no staged suffix"));
    std::ranges::reverse(suffix);
    for (auto const* entry : suffix)
    {
      if (auto applied = state.apply_candidate(*entry); !applied)
        return std::unexpected(std::move(applied.error()));
    }
    if (state.mode_ != Mode::Pending)
      return std::unexpected(append_state_error("incomplete assistant-output history did not produce a pending staging state"));
    return state;
  }
  catch (...)
  {
    return std::unexpected(append_state_error("failed to initialize assistant-output append state from session history"));
  }
}

ava::core::VoidResult AssistantOutputAppendState::apply_candidate(SessionEntry const& entry)
{
  if (entry.type != EntryType::AssistantOutputItem && entry.type != EntryType::AssistantTurnCommit)
  {
    if (mode_ == Mode::Pending)
    {
      auto error = append_state_error("pending assistant-output staging requires the exact next output item or matching commit");
      error.with_context("assistant_turn_id", pending_turn_id_).with_context("next_sequence", std::to_string(pending_next_sequence_));
      return std::unexpected(std::move(error));
    }
    return {};
  }

  if (entry.type == EntryType::AssistantOutputItem)
  {
    auto item = parse_assistant_output_item(entry);
    if (!item)
    {
      auto error = append_state_error("assistant-output append item is not a strict valid v4 record");
      error.with_context("cause", item.error().message());
      return std::unexpected(std::move(error));
    }

    if (committed_output_entry_ids_.contains(entry.id) || pending_output_entry_ids_.contains(entry.id))
    {
      auto error = append_state_error("assistant-output staging cannot reuse a physical output entry id");
      error.with_context("entry_id", entry.id);
      return std::unexpected(std::move(error));
    }

    if (mode_ == Mode::Closed)
    {
      if (item->sequence != 0)
      {
        auto error = append_state_error("new assistant-output staging must begin at sequence zero");
        error.with_context("assistant_turn_id", item->assistant_turn_id).with_context("sequence", std::to_string(item->sequence));
        return std::unexpected(std::move(error));
      }
      if (committed_turn_ids_.contains(item->assistant_turn_id))
      {
        auto error = append_state_error("assistant-output staging cannot reuse a committed assistant_turn_id");
        error.with_context("assistant_turn_id", item->assistant_turn_id);
        return std::unexpected(std::move(error));
      }
      mode_ = Mode::Pending;
      pending_turn_id_ = item->assistant_turn_id;
      pending_next_sequence_ = 1;
      pending_output_entry_ids_.clear();
      pending_provider_item_ids_.clear();
      pending_provider_output_indices_.clear();
    }
    else
    {
      if (item->assistant_turn_id != pending_turn_id_ || item->sequence != pending_next_sequence_)
      {
        auto error = append_state_error("pending assistant-output staging requires the exact next item for its assistant_turn_id");
        error.with_context("assistant_turn_id", pending_turn_id_)
            .with_context("next_sequence", std::to_string(pending_next_sequence_))
            .with_context("candidate_turn_id", item->assistant_turn_id)
            .with_context("candidate_sequence", std::to_string(item->sequence));
        return std::unexpected(std::move(error));
      }
      ++pending_next_sequence_;
    }

    pending_output_entry_ids_.insert(entry.id);
    if (item->provider_item_id && !pending_provider_item_ids_.insert(*item->provider_item_id).second)
    {
      auto error = append_state_error("pending assistant-output staging duplicates provider_item_id");
      error.with_context("assistant_turn_id", pending_turn_id_).with_context("provider_item_id", *item->provider_item_id);
      return std::unexpected(std::move(error));
    }
    if (item->provider_output_index && !pending_provider_output_indices_.insert(*item->provider_output_index).second)
    {
      auto error = append_state_error("pending assistant-output staging duplicates provider_output_index");
      error.with_context("assistant_turn_id", pending_turn_id_).with_context("provider_output_index", std::to_string(*item->provider_output_index));
      return std::unexpected(std::move(error));
    }
    return {};
  }

  auto commit = parse_assistant_turn_commit(entry);
  if (!commit)
  {
    auto error = append_state_error("assistant-output append commit is not a strict valid v4 record");
    error.with_context("cause", commit.error().message());
    return std::unexpected(std::move(error));
  }
  if (mode_ == Mode::Closed)
  {
    if (commit->item_count != 0)
    {
      auto error = append_state_error("assistant-output commit without staged items must have item_count zero");
      error.with_context("assistant_turn_id", commit->assistant_turn_id).with_context("item_count", std::to_string(commit->item_count));
      return std::unexpected(std::move(error));
    }
    if (!committed_turn_ids_.insert(commit->assistant_turn_id).second)
    {
      auto error = append_state_error("assistant-output commit cannot reuse a committed assistant_turn_id");
      error.with_context("assistant_turn_id", commit->assistant_turn_id);
      return std::unexpected(std::move(error));
    }
    return {};
  }

  if (commit->assistant_turn_id != pending_turn_id_ || commit->item_count != pending_next_sequence_)
  {
    auto error = append_state_error("pending assistant-output staging requires a matching commit with the exact item_count");
    error.with_context("assistant_turn_id", pending_turn_id_)
        .with_context("expected_item_count", std::to_string(pending_next_sequence_))
        .with_context("candidate_turn_id", commit->assistant_turn_id)
        .with_context("candidate_item_count", std::to_string(commit->item_count));
    return std::unexpected(std::move(error));
  }
  if (!committed_turn_ids_.insert(commit->assistant_turn_id).second)
  {
    auto error = append_state_error("assistant-output commit cannot reuse a committed assistant_turn_id");
    error.with_context("assistant_turn_id", commit->assistant_turn_id);
    return std::unexpected(std::move(error));
  }
  for (auto const& entry_id : pending_output_entry_ids_) committed_output_entry_ids_.insert(entry_id);
  mode_ = Mode::Closed;
  pending_turn_id_.clear();
  pending_next_sequence_ = 0;
  pending_output_entry_ids_.clear();
  pending_provider_item_ids_.clear();
  pending_provider_output_indices_.clear();
  return {};
}

CommittedAssistantTurn const* AssistantOutputProjection::find_turn_by_commit_index(std::size_t commit_index) const noexcept
{
  auto const found = turn_indices_by_commit_index.find(commit_index);
  if (found == turn_indices_by_commit_index.end() || found->second >= turns.size())
    return nullptr;
  return &turns[found->second];
}

CommittedAssistantTurn const* AssistantOutputProjection::find_turn_by_output_entry_id(std::string_view entry_id) const noexcept
{
  auto const found = item_references_by_entry_id.find(std::string(entry_id));
  if (found == item_references_by_entry_id.end() || found->second.turn_index >= turns.size())
    return nullptr;
  return &turns[found->second.turn_index];
}

CommittedAssistantOutputItem const* AssistantOutputProjection::find_item_by_output_entry_id(std::string_view entry_id) const noexcept
{
  auto const found = item_references_by_entry_id.find(std::string(entry_id));
  auto const* turn = find_turn_by_output_entry_id(entry_id);
  if (found == item_references_by_entry_id.end() || !turn || found->second.item_index >= turn->items.size())
    return nullptr;
  return &turn->items[found->second.item_index];
}

}  // namespace ava::session
