#include "sys.h"
#include "ava/http/transport.h"
#include "ava/provider/openai_compatible_provider.h"
#include "ava/provider/openai_compatible_request.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace ava::provider {
namespace {

ProviderFinishReason normalized_finish_reason(std::string_view reason)
{
  return normalize_provider_finish_reason(ProviderProtocol::OpenAIChat, reason);
}

std::vector<StreamEvent> finish_reasoning_if_open(bool& reasoning_open, std::string_view reasoning_format)
{
  if (!reasoning_open)
    return {};
  reasoning_open = false;
  return {StreamEvent{.type = StreamEventType::ReasoningEnd,
                      .text = "",
                      .tool_call_id = "",
                      .tool_name = "",
                      .error_message = "",
                      .usage = std::nullopt,
                      .reasoning_format = std::string(reasoning_format)}};
}

void append_finish_reasoning_if_open(std::vector<StreamEvent>& events, bool& reasoning_open, std::string_view reasoning_format)
{
  auto reasoning_end = finish_reasoning_if_open(reasoning_open, reasoning_format);
  events.insert(events.end(), reasoning_end.begin(), reasoning_end.end());
}

void append_done(std::vector<StreamEvent>& events, std::optional<TokenUsage> usage, std::optional<ProviderFinishReason> finish_reason)
{
  events.push_back(StreamEvent{.type = StreamEventType::Done,
                               .text = "",
                               .tool_call_id = "",
                               .tool_name = "",
                               .error_message = "",
                               .usage = std::move(usage),
                               .finish_reason = finish_reason});
}

void append_tool_call_end_events(std::vector<StreamEvent>& events, std::map<int, std::string>& open_tool_call_ids)
{
  for (auto const& [_, id] : open_tool_call_ids)
  {
    events.push_back(
        StreamEvent{.type = StreamEventType::ToolCallEnd, .text = "", .tool_call_id = id, .tool_name = "", .error_message = "", .usage = std::nullopt});
  }
  open_tool_call_ids.clear();
}

bool append_tool_call_delta_events(std::vector<StreamEvent>& events, std::map<int, std::string>& open_tool_call_ids, std::string_view delta,
                                   std::string_view fallback_tool_call_prefix, std::size_t& remaining_tool_calls)
{
  auto const tool_calls = ava::core::json::strict_objects_in_array_field(delta, "tool_calls", remaining_tool_calls);
  if (!tool_calls)
    return !ava::core::json::field_value_start(delta, "tool_calls");
  remaining_tool_calls -= tool_calls->size();
  for (auto const& call : *tool_calls)
  {
    auto const index_value = ava::core::json::integer_field(call, "index").value_or(0);
    auto const index = static_cast<int>(index_value);
    auto const function = ava::core::json::object_field(call, "function");
    auto id = ava::core::json::string_field(call, "id").value_or("");
    if (id.empty())
      id = std::string(fallback_tool_call_prefix) + "_" + std::to_string(index);
    auto const name = function ? ava::core::json::string_field(*function, "name").value_or("") : "";
    if (!open_tool_call_ids.contains(index))
    {
      open_tool_call_ids[index] = id;
      events.push_back(
          StreamEvent{.type = StreamEventType::ToolCallStart, .text = "", .tool_call_id = id, .tool_name = name, .error_message = "", .usage = std::nullopt});
    }
    if (function)
    {
      if (auto arguments = ava::core::json::string_field(*function, "arguments"))
      {
        events.push_back(StreamEvent{.type = StreamEventType::ToolCallDelta,
                                     .text = *arguments,
                                     .tool_call_id = open_tool_call_ids[index],
                                     .tool_name = "",
                                     .error_message = "",
                                     .usage = std::nullopt});
      }
    }
  }
  return true;
}

bool append_choice_delta_events(std::vector<StreamEvent>& events, std::map<int, std::string>& open_tool_call_ids, bool& reasoning_open,
                                std::optional<ProviderFinishReason>& finish_reason, std::string_view choice, std::string_view reasoning_format,
                                std::string_view fallback_tool_call_prefix, std::size_t& remaining_tool_calls)
{
  if (auto raw_finish_reason = ava::core::json::string_field(choice, "finish_reason"))
  {
    finish_reason = normalized_finish_reason(*raw_finish_reason);
    if (*raw_finish_reason == "tool_calls" || *raw_finish_reason == "function_call")
    {
      append_tool_call_end_events(events, open_tool_call_ids);
    }
    append_finish_reasoning_if_open(events, reasoning_open, reasoning_format);
  }
  auto const delta = ava::core::json::object_field(choice, "delta");
  if (!delta)
    return true;
  if (auto reasoning = ava::core::json::string_field(*delta, "reasoning_content"))
  {
    if (!reasoning_open)
    {
      reasoning_open = true;
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningStart,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = std::string(reasoning_format)});
    }
    if (!reasoning->empty())
    {
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningDelta,
                                   .text = *reasoning,
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = std::string(reasoning_format)});
    }
  }
  if (auto content = ava::core::json::string_field(*delta, "content"))
  {
    append_finish_reasoning_if_open(events, reasoning_open, reasoning_format);
    if (!content->empty())
    {
      events.push_back(
          StreamEvent{.type = StreamEventType::TextDelta, .text = *content, .tool_call_id = "", .tool_name = "", .error_message = "", .usage = std::nullopt});
    }
  }
  return append_tool_call_delta_events(events, open_tool_call_ids, *delta, fallback_tool_call_prefix, remaining_tool_calls);
}

void append_event_for_data(std::vector<StreamEvent>& events, std::map<int, std::string>& open_tool_call_ids, bool& reasoning_open,
                           std::optional<TokenUsage>& usage, std::optional<ProviderFinishReason>& finish_reason, bool& saw_data, bool& done_seen,
                           bool& error_seen, std::string_view data, std::string_view reasoning_format, std::string_view fallback_tool_call_prefix)
{
  if (done_seen || error_seen)
    return;
  if (data == "[DONE]")
  {
    done_seen = true;
    append_finish_reasoning_if_open(events, reasoning_open, reasoning_format);
    if (!finish_reason)
      finish_reason = open_tool_call_ids.empty() ? ProviderFinishReason::Completed : ProviderFinishReason::ToolCalls;
    append_tool_call_end_events(events, open_tool_call_ids);
    append_done(events, std::move(usage), finish_reason);
    usage = std::nullopt;
    finish_reason = std::nullopt;
    return;
  }
  saw_data = true;
  if (!is_json_object_shape(data))
  {
    error_seen = true;
    append_tool_call_end_events(events, open_tool_call_ids);
    append_finish_reasoning_if_open(events, reasoning_open, reasoning_format);
    events.push_back(StreamEvent{.type = StreamEventType::Error,
                                 .text = "",
                                 .tool_call_id = "",
                                 .tool_name = "",
                                 .error_message = "malformed OpenAI-compatible SSE event",
                                 .usage = std::nullopt});
    return;
  }
  if (auto const parsed_usage = parse_openai_usage(data))
    usage = parsed_usage;
  if (ava::core::json::field_value_start(data, "choices"))
  {
    auto const choices = ava::core::json::strict_objects_in_array_field(data, "choices", kMaxProviderParserArrayItems);
    if (!choices)
    {
      error_seen = true;
      append_tool_call_end_events(events, open_tool_call_ids);
      append_finish_reasoning_if_open(events, reasoning_open, reasoning_format);
      events.push_back(StreamEvent{.type = StreamEventType::Error,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "OpenAI-compatible response parser limit exceeded",
                                   .usage = std::nullopt});
      return;
    }
    std::size_t remaining_tool_calls = kMaxProviderParserArrayItems;
    for (auto const& choice : *choices)
    {
      if (!append_choice_delta_events(events, open_tool_call_ids, reasoning_open, finish_reason, choice, reasoning_format, fallback_tool_call_prefix,
                                      remaining_tool_calls))
      {
        error_seen = true;
        events.push_back(provider_parser_limit_event("OpenAI-compatible"));
        return;
      }
    }
  }
  if (auto const error_object = ava::core::json::object_field(data, "error"))
  {
    done_seen = true;
    error_seen = true;
    append_tool_call_end_events(events, open_tool_call_ids);
    append_finish_reasoning_if_open(events, reasoning_open, reasoning_format);
    events.push_back(StreamEvent{.type = StreamEventType::Error,
                                 .text = "",
                                 .tool_call_id = "",
                                 .tool_name = "",
                                 .error_message = "OpenAI-compatible provider reported a streaming error",
                                 .usage = std::nullopt});
  }
}

ava::core::Result<std::vector<StreamEvent>> parse_chat_completion_message(std::string_view body, std::string_view reasoning_format,
                                                                          std::string_view fallback_tool_call_prefix)
{
  if (body.size() > kMaxProviderSseBufferedBytes)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI-compatible response parser limit exceeded"));
  std::vector<StreamEvent> events;
  auto const usage = parse_openai_usage(body);
  std::optional<ProviderFinishReason> finish_reason;
  bool parsed_message = false;
  std::size_t tool_call_index = 0;
  auto const choices = ava::core::json::strict_objects_in_array_field(body, "choices", kMaxProviderParserArrayItems);
  if (!choices)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI-compatible response parser limit exceeded"));
  for (auto const& choice : *choices)
  {
    if (auto raw_finish_reason = ava::core::json::string_field(choice, "finish_reason"))
    {
      finish_reason = normalized_finish_reason(*raw_finish_reason);
    }
    auto const message = ava::core::json::object_field(choice, "message");
    if (!message)
    {
      if (finish_reason == ProviderFinishReason::Refusal)
      {
        parsed_message = true;
        break;
      }
      continue;
    }
    bool parsed_content = false;
    if (auto reasoning = ava::core::json::string_field(*message, "reasoning_content"))
    {
      parsed_content = true;
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningStart,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = std::string(reasoning_format)});
      if (!reasoning->empty())
      {
        events.push_back(StreamEvent{.type = StreamEventType::ReasoningDelta,
                                     .text = *reasoning,
                                     .tool_call_id = "",
                                     .tool_name = "",
                                     .error_message = "",
                                     .usage = std::nullopt,
                                     .reasoning_format = std::string(reasoning_format)});
      }
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningEnd,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = std::string(reasoning_format)});
    }
    if (auto content = ava::core::json::string_field(*message, "content"))
    {
      parsed_content = true;
      if (!content->empty())
      {
        events.push_back(
            StreamEvent{.type = StreamEventType::TextDelta, .text = *content, .tool_call_id = "", .tool_name = "", .error_message = "", .usage = std::nullopt});
      }
    }
    auto const tool_calls = ava::core::json::strict_objects_in_array_field(*message, "tool_calls", kMaxProviderParserArrayItems);
    if (ava::core::json::field_value_start(*message, "tool_calls") && !tool_calls)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI-compatible response parser limit exceeded"));
    if (tool_calls && !tool_calls->empty())
      parsed_content = true;
    for (auto const& tool_call : tool_calls.value_or(std::vector<std::string>{}))
    {
      auto const function = ava::core::json::object_field(tool_call, "function");
      auto id = ava::core::json::string_field(tool_call, "id").value_or("");
      if (id.empty())
        id = std::string(fallback_tool_call_prefix) + "_" + std::to_string(tool_call_index);
      ++tool_call_index;
      auto const name = function ? ava::core::json::string_field(*function, "name").value_or("") : "";
      auto const arguments = function ? ava::core::json::string_field(*function, "arguments").value_or("") : "";
      events.push_back(
          StreamEvent{.type = StreamEventType::ToolCallStart, .text = "", .tool_call_id = id, .tool_name = name, .error_message = "", .usage = std::nullopt});
      events.push_back(StreamEvent{
          .type = StreamEventType::ToolCallDelta, .text = arguments, .tool_call_id = id, .tool_name = "", .error_message = "", .usage = std::nullopt});
      events.push_back(
          StreamEvent{.type = StreamEventType::ToolCallEnd, .text = "", .tool_call_id = id, .tool_name = "", .error_message = "", .usage = std::nullopt});
    }
    if (!parsed_content && finish_reason != ProviderFinishReason::Refusal)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI-compatible response content is missing"));
    }
    parsed_message = true;
    break;
  }
  if (!parsed_message)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI-compatible response message is missing"));
  }
  if (!finish_reason)
  {
    bool const has_tool_call = std::ranges::any_of(events, [](StreamEvent const& event) { return event.type == StreamEventType::ToolCallStart; });
    finish_reason = has_tool_call ? ProviderFinishReason::ToolCalls : ProviderFinishReason::Completed;
  }
  append_done(events, usage, finish_reason);
  return bounded_provider_events(std::move(events), "OpenAI-compatible");
}

}  // namespace

OpenAICompatibleStreamParser::OpenAICompatibleStreamParser(std::string reasoning_format)
    : reasoning_format_(std::move(reasoning_format)), fallback_tool_call_prefix_(ava::core::make_id("openai_call"))
{
}

ava::core::Result<std::vector<StreamEvent>> OpenAICompatibleStreamParser::append(std::string_view chunk)
{
  return sse_.append(chunk, "OpenAI-compatible", [this](auto& events, std::string_view data) {
    append_event_for_data(events, open_tool_call_ids_, reasoning_open_, usage_, finish_reason_, saw_data_, done_seen_, error_seen_, data, reasoning_format_,
                          fallback_tool_call_prefix_);
  });
}

ava::core::Result<std::vector<StreamEvent>> OpenAICompatibleStreamParser::finish()
{
  auto events = sse_.finish(
      "OpenAI-compatible",
      [this](auto& output, std::string_view data) {
        append_event_for_data(output, open_tool_call_ids_, reasoning_open_, usage_, finish_reason_, saw_data_, done_seen_, error_seen_, data, reasoning_format_,
                              fallback_tool_call_prefix_);
      },
      [this](auto& output) {
        append_tool_call_end_events(output, open_tool_call_ids_);
        append_finish_reasoning_if_open(output, reasoning_open_, reasoning_format_);
        if (saw_data_ && !done_seen_ && !error_seen_)
          output.push_back(StreamEvent{.type = StreamEventType::Error,
                                       .text = "",
                                       .tool_call_id = "",
                                       .tool_name = "",
                                       .error_message = "OpenAI-compatible SSE stream ended before done marker",
                                       .usage = std::nullopt});
      });
  saw_data_ = false;
  done_seen_ = false;
  error_seen_ = false;
  usage_ = std::nullopt;
  finish_reason_ = std::nullopt;
  open_tool_call_ids_.clear();
  reasoning_open_ = false;
  return events;
}

OpenAICompatibleProvider::OpenAICompatibleProvider(OpenAICompatibleProviderOptions options) : options_(std::move(options))
{
}

ava::core::Result<ava::http::HttpRequest> OpenAICompatibleProvider::build_request(ProviderRequest const& request, std::string_view access_token) const
{
  if (request.model_id.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "model id is required"));
  }
  if (options_.require_credential && access_token.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, options_.provider_name + " credential is required"));
  }
  if (auto valid_images = validate_outbound_image_payloads(request, options_.provider_name); !valid_images)
  {
    return std::unexpected(std::move(valid_images.error()));
  }
  if (auto valid_tools = validate_openai_compatible_tools_json(request); !valid_tools)
    return std::unexpected(std::move(valid_tools.error()));
  for (auto const& tool : request.tools_json)
  {
    auto converted = chat_completion_tool_json(tool);
    if (!converted)
      return std::unexpected(std::move(converted.error()));
  }

  std::map<std::string, std::string> headers{{"Content-Type", "application/json"}, {"Accept", request.stream ? "text/event-stream" : "application/json"}};
  if (options_.send_authorization_bearer && !access_token.empty())
    headers["Authorization"] = "Bearer " + std::string(access_token);
  if (!options_.user_agent.empty())
    headers["User-Agent"] = options_.user_agent;
  auto const url = !options_.endpoint.empty() ? options_.endpoint : join_openai_compatible_url(options_.base_url, options_.chat_completions_path);
  return ava::http::HttpRequest{.method = "POST",
                                .url = url,
                                .headers = std::move(headers),
                                .body = openai_compatible_request_body_json(request, options_),
                                .timeout_ms = 60000,
                                .follow_redirects = options_.follow_redirects,
                                .include_response_headers = false,
                                .resolve_hosts = {}};
}

std::unique_ptr<StreamParser> OpenAICompatibleProvider::create_stream_parser() const
{
  return std::make_unique<OpenAICompatibleStreamParser>(options_.reasoning_format);
}

ava::core::Result<std::vector<StreamEvent>> OpenAICompatibleProvider::parse_response(ava::http::HttpResponse const& response, bool stream) const
{
  if (response.status_code < 200 || response.status_code >= 300)
  {
    auto const kind = classify_provider_error(response);
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider,
                                  options_.provider_name + " HTTP request failed with status " + std::to_string(response.status_code));
    error.with_context("status", std::to_string(response.status_code));
    error.with_context("provider_error_kind", to_string(kind));
    if (auto const retry_after = ava::http::retry_after_header(response))
      error.with_context("retry_after", *retry_after);
    return std::unexpected(std::move(error));
  }
  if (stream)
    return parse_openai_compatible_sse(response.body, options_.reasoning_format);
  return parse_chat_completion_message(response.body, options_.reasoning_format, ava::core::make_id("openai_call"));
}

ava::core::Result<std::vector<StreamEvent>> parse_openai_compatible_sse(std::string_view sse, std::string reasoning_format)
{
  OpenAICompatibleStreamParser parser(std::move(reasoning_format));
  auto events = parser.append(sse);
  if (!events)
    return std::unexpected(std::move(events.error()));
  auto final_events = parser.finish();
  if (!final_events)
    return std::unexpected(std::move(final_events.error()));
  events->insert(events->end(), final_events->begin(), final_events->end());
  return events;
}

}  // namespace ava::provider
