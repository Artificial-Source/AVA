#include "sys.h"
#include "ava/http/transport.h"
#include "ava/provider/openai_reasoning.h"
#include "ava/provider/openai_response_parser.h"
#include "ava/provider/openai_response_parser_detail.h"
#include "ava/provider/openai_stream_parser.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ava::provider {
namespace {

ProviderFinishReason normalized_openai_finish_reason(std::string_view reason)
{
  return normalize_provider_finish_reason(ProviderProtocol::OpenAIResponses, reason);
}

std::optional<long long> non_negative_integer_field(std::string_view object, std::string_view key)
{
  auto const value = ava::core::json::integer_field(object, key);
  if (!value || *value < 0)
    return std::nullopt;
  return value;
}

std::optional<long long> first_integer_field(std::string_view object, std::initializer_list<std::string_view> keys)
{
  for (auto const key : keys)
  {
    if (auto const value = non_negative_integer_field(object, key))
      return value;
  }
  return std::nullopt;
}

void append_joined_text(std::string& output, std::string_view text)
{
  if (text.empty())
    return;
  if (!output.empty())
    output += "\n\n";
  output += text;
}

std::optional<TokenUsage> usage_from_object(std::string_view usage_object)
{
  TokenUsage usage;
  usage.input_tokens = first_integer_field(usage_object, {"input_tokens", "prompt_tokens"});
  usage.output_tokens = first_integer_field(usage_object, {"output_tokens", "completion_tokens"});
  usage.total_tokens = first_integer_field(usage_object, {"total_tokens"});

  if (auto const output_details = ava::core::json::object_field(usage_object, "output_tokens_details"))
  {
    usage.reasoning_tokens = first_integer_field(*output_details, {"reasoning_tokens"});
  }
  if (!usage.reasoning_tokens)
  {
    if (auto const completion_details = ava::core::json::object_field(usage_object, "completion_tokens_details"))
    {
      usage.reasoning_tokens = first_integer_field(*completion_details, {"reasoning_tokens"});
    }
  }
  if (!usage.reasoning_tokens)
    usage.reasoning_tokens = first_integer_field(usage_object, {"reasoning_tokens"});

  if (auto const input_details = ava::core::json::object_field(usage_object, "input_tokens_details"))
  {
    usage.cache_read_tokens = first_integer_field(*input_details, {"cached_tokens", "cache_read_tokens"});
    usage.cache_write_tokens = first_integer_field(*input_details, {"cache_creation_tokens", "cache_write_tokens"});
  }
  if (!usage.cache_read_tokens || !usage.cache_write_tokens)
  {
    if (auto const prompt_details = ava::core::json::object_field(usage_object, "prompt_tokens_details"))
    {
      if (!usage.cache_read_tokens)
      {
        usage.cache_read_tokens = first_integer_field(*prompt_details, {"cached_tokens", "cache_read_tokens"});
      }
      if (!usage.cache_write_tokens)
      {
        usage.cache_write_tokens = first_integer_field(*prompt_details, {"cache_creation_tokens", "cache_write_tokens"});
      }
    }
  }
  if (!usage.cache_read_tokens)
  {
    usage.cache_read_tokens = first_integer_field(usage_object, {"cached_tokens", "cache_read_tokens", "cache_read_input_tokens"});
  }
  if (!usage.cache_write_tokens)
  {
    usage.cache_write_tokens = first_integer_field(usage_object, {"cache_write_tokens", "cache_creation_input_tokens"});
  }

  if (!usage.input_tokens && !usage.output_tokens && !usage.reasoning_tokens && !usage.cache_read_tokens && !usage.cache_write_tokens && !usage.total_tokens)
  {
    return std::nullopt;
  }
  return usage;
}

}  // namespace

namespace detail {

std::optional<std::string> first_string_field(std::string_view object, std::initializer_list<std::string_view> keys)
{
  for (auto const key : keys)
  {
    if (auto value = ava::core::json::string_field(object, key))
      return value;
  }
  return std::nullopt;
}

bool is_null_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || object.substr(*start, 4) != "null")
    return false;
  auto const end = *start + 4;
  return end == object.size() || std::isspace(static_cast<unsigned char>(object[end])) != 0 || object[end] == ',' || object[end] == '}' || object[end] == ']';
}

ProviderFinishReason openai_response_finish_reason(std::string_view object)
{
  std::string status = ava::core::json::string_field(object, "status").value_or("");
  if (auto const response = ava::core::json::object_field(object, "response"))
  {
    if (status.empty())
      status = ava::core::json::string_field(*response, "status").value_or("");
    if (auto const details = ava::core::json::object_field(*response, "incomplete_details"))
    {
      if (auto reason = ava::core::json::string_field(*details, "reason"))
        return normalized_openai_finish_reason(*reason);
    }
  }
  if (auto const details = ava::core::json::object_field(object, "incomplete_details"))
  {
    if (auto reason = ava::core::json::string_field(*details, "reason"))
      return normalized_openai_finish_reason(*reason);
  }
  return normalized_openai_finish_reason(status);
}

ava::core::Result<std::string> reasoning_summary_text_from_object(std::string_view object, std::size_t& remaining_summary_parts)
{
  std::string text;
  if (auto summary_text = ava::core::json::string_field(object, "summary_text"))
  {
    append_joined_text(text, *summary_text);
  }
  if (auto direct_text = ava::core::json::string_field(object, "text"))
  {
    append_joined_text(text, *direct_text);
  }
  if (ava::core::json::field_value_start(object, "summary"))
  {
    auto const summaries = ava::core::json::strict_objects_in_array_field(object, "summary", remaining_summary_parts);
    if (!summaries)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response parser nested summary-part limit exceeded"));
    }
    remaining_summary_parts -= summaries->size();
    for (auto const& summary : *summaries)
    {
      if (auto summary_text = ava::core::json::string_field(summary, "text"))
      {
        append_joined_text(text, *summary_text);
      }
      else if (auto nested_summary = ava::core::json::string_field(summary, "summary_text"))
      {
        append_joined_text(text, *nested_summary);
      }
    }
  }
  if (auto const part = ava::core::json::object_field(object, "part"))
  {
    if (auto part_text = ava::core::json::string_field(*part, "text"))
      append_joined_text(text, *part_text);
  }
  if (auto const item = ava::core::json::object_field(object, "item"))
  {
    auto nested = reasoning_summary_text_from_object(*item, remaining_summary_parts);
    if (!nested)
      return std::unexpected(std::move(nested.error()));
    append_joined_text(text, *nested);
  }
  return text;
}

}  // namespace detail

ava::core::Result<std::vector<StreamEvent>> parse_openai_sse(std::string_view sse)
{
  OpenAIStreamParser parser;
  auto events = parser.append(sse);
  if (!events)
    return std::unexpected(std::move(events.error()));
  auto final_events = parser.finish();
  if (!final_events)
    return std::unexpected(std::move(final_events.error()));
  events->insert(events->end(), final_events->begin(), final_events->end());
  return events;
}

ava::core::Result<std::vector<StreamEvent>> parse_openai_sse_response(ava::http::HttpResponse const& response)
{
  if (response.status_code < 200 || response.status_code >= 300)
  {
    auto const kind = classify_provider_error(response);
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI HTTP request failed with status " + std::to_string(response.status_code));
    error.with_context("status", std::to_string(response.status_code));
    error.with_context("provider_error_kind", to_string(kind));
    if (auto const retry_after = ava::http::retry_after_header(response))
      error.with_context("retry_after", *retry_after);
    return std::unexpected(std::move(error));
  }
  return parse_openai_sse(response.body);
}

ava::core::Result<std::string> parse_openai_response_text(std::string_view body)
{
  if (body.size() > kMaxProviderSseBufferedBytes)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response parser limit exceeded"));
  // Native Responses output is authoritative over the convenience top-level
  // output_text field. A refusal is visible assistant text, not an error blob.
  auto const output_start = ava::core::json::field_value_start(body, "output");
  if (output_start)
  {
    auto const output_items = ava::core::json::strict_objects_in_array_field(body, "output", kMaxProviderParserArrayItems);
    if (!output_items)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response parser limit exceeded"));
    std::size_t remaining_content_parts = kMaxProviderParserArrayItems;
    std::string output_text;
    bool native_message_present = false;
    for (auto const& item : *output_items)
    {
      if (ava::core::json::string_field(item, "type").value_or("") != "message")
        continue;
      native_message_present = true;
      auto const contents = ava::core::json::strict_objects_in_array_field(item, "content", remaining_content_parts);
      if (!contents)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response parser limit exceeded"));
      remaining_content_parts -= contents->size();
      for (auto const& content : *contents)
      {
        auto const type = ava::core::json::string_field(content, "type").value_or("");
        if (type == "output_text")
        {
          auto const text = ava::core::json::string_field(content, "text");
          if (!text)
            return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI output_text content part requires string text"));
          append_joined_text(output_text, *text);
        }
        else if (type == "refusal")
        {
          auto const refusal = ava::core::json::string_field(content, "refusal");
          if (!refusal)
            return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI refusal content part requires string refusal"));
          append_joined_text(output_text, *refusal);
        }
        else
        {
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI message output item has an unsupported content-part type"));
        }
      }
    }
    if (native_message_present && !output_text.empty())
      return output_text;
  }
  if (auto output = ava::core::json::string_field(body, "output_text"))
    return *output;
  if (auto text = ava::core::json::string_field(body, "text"))
    return *text;
  if (auto refusal = ava::core::json::string_field(body, "refusal"))
    return *refusal;
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response text is missing"));
}

namespace detail {

ava::core::Result<std::vector<StreamEvent>> parse_openai_non_stream_response(std::string_view body)
{
  if (body.size() > kMaxProviderSseBufferedBytes)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response parser limit exceeded"));
  if (!ava::core::json::is_valid_object(body))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response was malformed JSON"));

  std::vector<StreamEvent> events;
  auto const finish_reason = openai_response_finish_reason(body);
  std::unordered_set<std::string> output_item_ids;
  std::unordered_set<std::size_t> output_indexes;
  std::unordered_map<std::string, std::string> call_ids_by_item_id;
  std::unordered_map<std::string, std::string> item_ids_by_call_id;
  bool legacy_message_text_seen = false;
  bool refusal_seen = false;

  auto append_error = [&events](std::string message) {
    events.push_back(StreamEvent{
        .type = StreamEventType::Error, .text = "", .tool_call_id = "", .tool_name = "", .error_message = std::move(message), .usage = std::nullopt});
  };
  auto parser_limit_events = [] {
    return std::vector<StreamEvent>{StreamEvent{.type = StreamEventType::Error,
                                                .text = "",
                                                .tool_call_id = "",
                                                .tool_name = "",
                                                .error_message = "OpenAI response parser limit exceeded",
                                                .usage = std::nullopt}};
  };
  auto output_index = [](std::string_view item, std::size_t physical_index) -> ava::core::Result<std::size_t> {
    if (!ava::core::json::field_value_start(item, "output_index"))
      return physical_index;
    auto const value = ava::core::json::integer_field(item, "output_index");
    if (!value || *value < 0 || static_cast<unsigned long long>(*value) > std::numeric_limits<std::size_t>::max())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI output item has an invalid output_index"));
    }
    if (static_cast<std::size_t>(*value) != physical_index)
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI output item output_index disagrees with its physical output array position"));
    }
    return physical_index;
  };
  auto message_phase = [](std::string_view item) -> ava::core::Result<AssistantPhase> {
    if (!ava::core::json::field_value_start(item, "phase") || is_null_field(item, "phase"))
      return AssistantPhase::Unknown;
    auto const value = ava::core::json::string_field(item, "phase");
    if (!value || value->empty())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI message output item has an empty or invalid phase"));
    auto const phase = assistant_phase_from_string(*value);
    if (!phase)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI message output item has an unknown phase"));
    return *phase;
  };
  auto register_output_item = [&](std::string const& id, std::size_t index) -> bool {
    if (!output_item_ids.insert(id).second)
    {
      append_error("OpenAI output item ID is duplicated");
      return false;
    }
    if (!output_indexes.insert(index).second)
    {
      append_error("OpenAI output item index is duplicated");
      return false;
    }
    return true;
  };

  auto const output_field = ava::core::json::field_value_start(body, "output");
  auto const strict_output_items =
      output_field ? ava::core::json::strict_objects_in_array_field(body, "output", kMaxProviderParserArrayItems) : std::optional<std::vector<std::string>>{};
  if (output_field && !strict_output_items)
    return parser_limit_events();
  std::size_t remaining_nested_parts = kMaxProviderParserArrayItems;
  std::vector<std::string> const empty_output_items;
  auto const& output_items = strict_output_items ? *strict_output_items : empty_output_items;
  bool const explicit_zero_item_output = strict_output_items && strict_output_items->empty();
  bool const native_message_present = std::any_of(
      output_items.begin(), output_items.end(), [](std::string const& item) { return ava::core::json::string_field(item, "type").value_or("") == "message"; });

  // The top-level text is a convenience projection. It remains a legacy
  // fallback only when the native output array contains no message items.
  auto const top_level_text = ava::core::json::string_field(body, "output_text").or_else([&body] { return ava::core::json::string_field(body, "text"); });
  if (top_level_text && !native_message_present)
  {
    events.push_back(StreamEvent{
        .type = StreamEventType::TextDelta, .text = *top_level_text, .tool_call_id = "", .tool_name = "", .error_message = "", .usage = std::nullopt});
  }

  for (std::size_t physical_index = 0; physical_index < output_items.size(); ++physical_index)
  {
    auto const& item = output_items[physical_index];
    auto const item_type = ava::core::json::string_field(item, "type").value_or("");
    if (item_type == "reasoning")
    {
      auto const item_id = ava::core::json::string_field(item, "id");
      auto index = output_index(item, physical_index);
      if (!index)
      {
        append_error(index.error().message());
        continue;
      }
      bool const has_native_metadata = item_id.has_value() || ava::core::json::field_value_start(item, "output_index").has_value();
      if (has_native_metadata && (!item_id || !is_valid_openai_opaque_id(*item_id)))
      {
        append_error("OpenAI reasoning output item requires a bounded item ID");
        continue;
      }
      if (item_id && !register_output_item(*item_id, *index))
        continue;
      auto summary = reasoning_summary_text_from_object(item, remaining_nested_parts);
      if (!summary)
        return parser_limit_events();
      auto const provider_index = item_id ? std::optional<std::size_t>{*index} : std::nullopt;
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningStart,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .provider_item_id = item_id.value_or(""),
                                   .provider_output_index = provider_index,
                                   .reasoning_format = std::string(kOpenAIResponsesReasoningFormat)});
      if (!summary->empty())
      {
        events.push_back(StreamEvent{.type = StreamEventType::ReasoningDelta,
                                     .text = std::move(*summary),
                                     .tool_call_id = "",
                                     .tool_name = "",
                                     .error_message = "",
                                     .usage = std::nullopt,
                                     .provider_item_id = item_id.value_or(""),
                                     .provider_output_index = provider_index,
                                     .reasoning_format = std::string(kOpenAIResponsesReasoningFormat)});
      }
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningEnd,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .provider_item_id = item_id.value_or(""),
                                   .provider_output_index = provider_index,
                                   .reasoning_format = std::string(kOpenAIResponsesReasoningFormat),
                                   .reasoning_native_item_json = is_valid_openai_native_reasoning_item_json(item) ? item : std::string{}});
      continue;
    }
    if (item_type == "message")
    {
      auto const item_id = ava::core::json::string_field(item, "id");
      auto index = output_index(item, physical_index);
      auto phase = message_phase(item);
      bool const has_native_message_metadata = item_id.has_value() || ava::core::json::field_value_start(item, "output_index").has_value() ||
                                               (ava::core::json::field_value_start(item, "phase").has_value() && !is_null_field(item, "phase"));
      if (!index || !phase || (has_native_message_metadata && (!item_id || !is_valid_openai_opaque_id(*item_id))))
      {
        append_error(!index ? index.error().message() : (!phase ? phase.error().message() : "OpenAI message output item requires a bounded item ID"));
        continue;
      }
      if (has_native_message_metadata)
      {
        if (!register_output_item(*item_id, *index))
          continue;
        events.push_back(StreamEvent{.type = StreamEventType::TextStart,
                                     .text = "",
                                     .tool_call_id = "",
                                     .tool_name = "",
                                     .error_message = "",
                                     .usage = std::nullopt,
                                     .provider_item_id = *item_id,
                                     .provider_output_index = *index,
                                     .assistant_phase = *phase});
      }
      auto const contents = ava::core::json::strict_objects_in_array_field(item, "content", remaining_nested_parts);
      if (!contents)
        return parser_limit_events();
      remaining_nested_parts -= contents->size();
      for (auto const& content : *contents)
      {
        auto const content_type = ava::core::json::string_field(content, "type").value_or("");
        std::optional<std::string> visible_text;
        if (content_type == "output_text")
        {
          visible_text = ava::core::json::string_field(content, "text");
          if (!visible_text)
          {
            append_error("OpenAI output_text content part at output_index " + std::to_string(*index) + " requires string text");
            return events;
          }
        }
        else if (content_type == "refusal")
        {
          visible_text = ava::core::json::string_field(content, "refusal");
          if (!visible_text)
          {
            append_error("OpenAI refusal content part at output_index " + std::to_string(*index) + " requires string refusal");
            return events;
          }
          refusal_seen = true;
        }
        else
        {
          append_error("OpenAI message output item has an unsupported content-part type");
          return events;
        }
        if (has_native_message_metadata)
        {
          events.push_back(StreamEvent{.type = StreamEventType::TextDelta,
                                       .text = std::move(*visible_text),
                                       .tool_call_id = "",
                                       .tool_name = "",
                                       .error_message = "",
                                       .usage = std::nullopt,
                                       .provider_item_id = *item_id,
                                       .provider_output_index = *index,
                                       .assistant_phase = *phase});
        }
        else
        {
          auto legacy_text = std::move(*visible_text);
          if (legacy_message_text_seen)
            legacy_text.insert(0, "\n\n");
          legacy_message_text_seen = true;
          events.push_back(StreamEvent{.type = StreamEventType::TextDelta,
                                       .text = std::move(legacy_text),
                                       .tool_call_id = "",
                                       .tool_name = "",
                                       .error_message = "",
                                       .usage = std::nullopt});
        }
      }
      if (has_native_message_metadata)
      {
        events.push_back(StreamEvent{.type = StreamEventType::TextEnd,
                                     .text = "",
                                     .tool_call_id = "",
                                     .tool_name = "",
                                     .error_message = "",
                                     .usage = std::nullopt,
                                     .provider_item_id = *item_id,
                                     .provider_output_index = *index,
                                     .assistant_phase = *phase});
      }
      continue;
    }
    if (item_type != "function_call")
    {
      auto index = output_index(item, physical_index);
      if (!index)
        append_error(index.error().message());
      else
        append_error("OpenAI response contains an unsupported output item type");
      return events;
    }

    // Responses output-item IDs (for example, fc_...) identify stream items;
    // call_id is the separate opaque identity required for dispatch and replay.
    auto const item_id = ava::core::json::string_field(item, "id");
    auto const call_id = ava::core::json::string_field(item, "call_id");
    auto const name = ava::core::json::string_field(item, "name");
    auto const arguments = ava::core::json::string_field(item, "arguments");
    auto index = output_index(item, physical_index);
    if (!item_id || !call_id || !name || !arguments || !index || !is_valid_openai_opaque_id(*item_id) || !is_valid_openai_opaque_id(*call_id) ||
        !is_valid_openai_opaque_id(*name) || !ava::core::json::is_valid_object(*arguments))
    {
      append_error(!index ? index.error().message()
                          : "OpenAI function call item requires bounded item ID, logical call ID, name, and JSON-object string arguments");
      continue;
    }
    if (!register_output_item(*item_id, *index) || call_ids_by_item_id.contains(*item_id) || item_ids_by_call_id.contains(*call_id))
    {
      if (call_ids_by_item_id.contains(*item_id) || item_ids_by_call_id.contains(*call_id))
        append_error("OpenAI function call item ID and logical call ID must form a unique one-to-one mapping");
      continue;
    }
    call_ids_by_item_id.emplace(*item_id, *call_id);
    item_ids_by_call_id.emplace(*call_id, *item_id);
    events.push_back(StreamEvent{.type = StreamEventType::ToolCallStart,
                                 .text = "",
                                 .tool_call_id = *call_id,
                                 .tool_name = *name,
                                 .error_message = "",
                                 .usage = std::nullopt,
                                 .provider_item_id = *item_id,
                                 .provider_output_index = *index});
    events.push_back(StreamEvent{.type = StreamEventType::ToolCallDelta,
                                 .text = *arguments,
                                 .tool_call_id = *call_id,
                                 .tool_name = "",
                                 .error_message = "",
                                 .usage = std::nullopt,
                                 .provider_item_id = *item_id,
                                 .provider_output_index = *index});
    events.push_back(StreamEvent{.type = StreamEventType::ToolCallEnd,
                                 .text = "",
                                 .tool_call_id = *call_id,
                                 .tool_name = "",
                                 .error_message = "",
                                 .usage = std::nullopt,
                                 .provider_item_id = *item_id,
                                 .provider_output_index = *index});
  }

  auto const terminal_finish_reason = refusal_seen ? ProviderFinishReason::Refusal : finish_reason;
  bool const allows_empty_terminal = explicit_zero_item_output || terminal_finish_reason == ProviderFinishReason::MaxTokens ||
                                     terminal_finish_reason == ProviderFinishReason::Refusal || terminal_finish_reason == ProviderFinishReason::Cancelled ||
                                     terminal_finish_reason == ProviderFinishReason::Error;
  if (events.empty() && !allows_empty_terminal)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response text is missing"));
  }
  events.push_back(StreamEvent{.type = StreamEventType::Done,
                               .text = "",
                               .tool_call_id = "",
                               .tool_name = "",
                               .error_message = "",
                               .usage = parse_openai_usage(body),
                               .finish_reason = terminal_finish_reason});
  return bounded_provider_events(std::move(events), "OpenAI");
}

}  // namespace detail

std::optional<TokenUsage> parse_openai_usage(std::string_view body)
{
  if (auto const usage = ava::core::json::object_field(body, "usage"))
    return usage_from_object(*usage);
  if (auto const response = ava::core::json::object_field(body, "response"))
  {
    if (auto const usage = ava::core::json::object_field(*response, "usage"))
      return usage_from_object(*usage);
  }
  return usage_from_object(body);
}

bool is_retryable_status(int status_code) noexcept
{
  return status_code == 408 || status_code == 409 || status_code == 429 || (status_code >= 500 && status_code < 600);
}

bool is_auth_status(int status_code) noexcept
{
  return status_code == 401 || status_code == 403;
}

}  // namespace ava::provider
