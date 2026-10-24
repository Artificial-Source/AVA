#include "sys.h"
#include "ava/http/transport.h"
#include "ava/provider/anthropic_provider.h"
#include "ava/provider/anthropic_request.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::provider {
namespace {

constexpr std::size_t kMaxReasoningOpaqueBytes = 64 * 1024;
constexpr std::string_view kAnthropicOAuthBeta = "oauth-2025-04-20";

std::string_view trim_ascii(std::string_view value)
{
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
    value.remove_suffix(1);
  return value;
}

bool anthropic_beta_has_marker(std::string_view value, std::string_view marker)
{
  std::size_t start = 0;
  while (start <= value.size())
  {
    auto const comma = value.find(',', start);
    auto const item = trim_ascii(comma == std::string_view::npos ? value.substr(start) : value.substr(start, comma - start));
    if (item == marker)
      return true;
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  return false;
}

std::string anthropic_beta_with_marker(std::string existing, std::string_view marker)
{
  if (anthropic_beta_has_marker(existing, marker))
    return existing;
  if (trim_ascii(existing).empty())
    return std::string(marker);
  existing += ',';
  existing += marker;
  return existing;
}

ProviderFinishReason normalized_anthropic_finish_reason(std::string_view reason)
{
  return normalize_provider_finish_reason(ProviderProtocol::Anthropic, reason);
}

std::string stop_details_explanation(std::string_view object)
{
  auto const stop_details = ava::core::json::object_field(object, "stop_details");
  if (!stop_details)
    return {};
  return ava::core::json::string_field(*stop_details, "explanation").value_or("");
}

bool has_stop_details(std::string_view object)
{
  return ava::core::json::object_field(object, "stop_details").has_value();
}

void append_stream_error(std::vector<StreamEvent>& events, std::string_view message)
{
  // Callers pass AVA-owned parser diagnostics only; response fields must never
  // become an Error event payload.
  events.push_back(StreamEvent{
      .type = StreamEventType::Error, .text = "", .tool_call_id = "", .tool_name = "", .error_message = std::string(message), .usage = std::nullopt});
}

std::optional<long long> non_negative_integer_field(std::string_view object, std::string_view key)
{
  auto const value = ava::core::json::integer_field(object, key);
  if (!value || *value < 0)
    return std::nullopt;
  return value;
}

long long saturating_token_add(long long left, long long right) noexcept
{
  constexpr auto maximum = std::numeric_limits<long long>::max();
  return right > maximum - left ? maximum : left + right;
}

void merge_usage(TokenUsage& target, TokenUsage const& source)
{
  if (source.input_tokens)
    target.input_tokens = source.input_tokens;
  if (source.output_tokens)
    target.output_tokens = source.output_tokens;
  if (source.cache_read_tokens)
    target.cache_read_tokens = source.cache_read_tokens;
  if (source.cache_write_tokens)
    target.cache_write_tokens = source.cache_write_tokens;
  if (source.total_tokens)
    target.total_tokens = source.total_tokens;
}

void append_event_for_data(std::vector<StreamEvent>& events, std::map<long long, AnthropicStreamParser::ToolBlock>& tools,
                           std::map<long long, AnthropicStreamParser::ReasoningBlock>& reasoning_blocks, std::optional<TokenUsage>& usage,
                           std::optional<ProviderFinishReason>& finish_reason, bool& saw_data, bool& message_stop_seen, bool& error_seen, std::string_view data,
                           ProviderSseBuffer& framing)
{
  auto append_terminal_error = [&](std::string_view message) {
    error_seen = true;
    append_stream_error(events, message);
  };
  if (!is_json_object_shape(data))
  {
    append_terminal_error("malformed Anthropic stream event");
    return;
  }
  auto const type = ava::core::json::string_field(data, "type").value_or("");
  if (type == "ping")
    return;
  saw_data = true;
  if (type == "message_start")
  {
    if (auto const message = ava::core::json::object_field(data, "message"))
    {
      auto const content = ava::core::json::strict_objects_in_array_field(*message, "content", kMaxProviderParserArrayItems);
      if (ava::core::json::field_value_start(*message, "content") && !content)
      {
        append_terminal_error("Anthropic response parser limit exceeded");
        return;
      }
      if (auto const parsed = parse_anthropic_usage(*message))
      {
        if (!usage)
          usage = TokenUsage{};
        merge_usage(*usage, *parsed);
      }
    }
    return;
  }
  if (type == "content_block_start")
  {
    auto const block = ava::core::json::object_field(data, "content_block");
    auto const index = non_negative_integer_field(data, "index");
    if (!block || !index)
      return;
    auto const block_type = ava::core::json::string_field(*block, "type").value_or("");
    if (block_type == "tool_use")
    {
      auto const id = ava::core::json::string_field(*block, "id").value_or("");
      auto const name = ava::core::json::string_field(*block, "name").value_or("");
      tools[*index] = AnthropicStreamParser::ToolBlock{.id = id, .name = name};
      events.push_back(
          StreamEvent{.type = StreamEventType::ToolCallStart, .text = "", .tool_call_id = id, .tool_name = name, .error_message = "", .usage = std::nullopt});
    }
    else if (block_type == "thinking")
    {
      auto const signature = ava::core::json::string_field(*block, "signature").value_or("");
      if (signature.size() > kMaxReasoningOpaqueBytes)
      {
        append_terminal_error("Anthropic thinking signature exceeded byte limit");
        return;
      }
      if (!framing.retain_fragment(signature))
      {
        append_terminal_error("Anthropic response parser limit exceeded");
        return;
      }
      reasoning_blocks[*index] = AnthropicStreamParser::ReasoningBlock{.signature = signature, .redacted_data = "", .redacted = false};
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningStart,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = "anthropic_thinking"});
    }
    else if (block_type == "redacted_thinking")
    {
      auto const redacted_data = ava::core::json::string_field(*block, "data").value_or("");
      if (redacted_data.size() > kMaxReasoningOpaqueBytes)
      {
        append_terminal_error("Anthropic redacted thinking payload exceeded byte limit");
        return;
      }
      if (!framing.retain_fragment(redacted_data))
      {
        append_terminal_error("Anthropic response parser limit exceeded");
        return;
      }
      reasoning_blocks[*index] = AnthropicStreamParser::ReasoningBlock{.signature = "", .redacted_data = redacted_data, .redacted = true};
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningStart,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = "anthropic_thinking",
                                   .redacted = true});
    }
    return;
  }
  if (type == "content_block_delta")
  {
    auto const delta = ava::core::json::object_field(data, "delta");
    if (!delta)
    {
      append_terminal_error("Anthropic content_block_delta is missing delta");
      return;
    }
    auto const delta_type = ava::core::json::string_field(*delta, "type").value_or("");
    if (delta_type == "text_delta")
    {
      events.push_back(StreamEvent{.type = StreamEventType::TextDelta,
                                   .text = ava::core::json::string_field(*delta, "text").value_or(""),
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt});
      return;
    }
    if (delta_type == "input_json_delta")
    {
      auto const index = non_negative_integer_field(data, "index");
      auto const tool = index ? tools.find(*index) : tools.end();
      events.push_back(StreamEvent{.type = StreamEventType::ToolCallDelta,
                                   .text = ava::core::json::string_field(*delta, "partial_json").value_or(""),
                                   .tool_call_id = tool == tools.end() ? "" : tool->second.id,
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt});
      return;
    }
    if (delta_type == "thinking_delta")
    {
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningDelta,
                                   .text = ava::core::json::string_field(*delta, "thinking").value_or(""),
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = "anthropic_thinking"});
      return;
    }
    if (delta_type == "signature_delta")
    {
      auto const index = non_negative_integer_field(data, "index");
      auto const reasoning = index ? reasoning_blocks.find(*index) : reasoning_blocks.end();
      if (reasoning != reasoning_blocks.end())
      {
        auto const signature_delta = ava::core::json::string_field(*delta, "signature").value_or("");
        if (reasoning->second.signature.size() + signature_delta.size() > kMaxReasoningOpaqueBytes)
        {
          reasoning_blocks.erase(reasoning);
          append_terminal_error("Anthropic thinking signature exceeded byte limit");
          return;
        }
        if (!framing.retain_fragment(signature_delta))
        {
          append_terminal_error("Anthropic response parser limit exceeded");
          return;
        }
        reasoning->second.signature += signature_delta;
      }
      return;
    }
    append_terminal_error("unrecognized Anthropic content_block_delta");
    return;
  }
  if (type == "content_block_stop")
  {
    auto const index = non_negative_integer_field(data, "index");
    if (!index)
      return;
    auto const tool = tools.find(*index);
    auto const reasoning = reasoning_blocks.find(*index);
    if (reasoning != reasoning_blocks.end())
    {
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningEnd,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = "anthropic_thinking",
                                   .reasoning_signature = reasoning->second.signature,
                                   .reasoning_redacted_data = reasoning->second.redacted_data,
                                   .redacted = reasoning->second.redacted});
      reasoning_blocks.erase(reasoning);
      return;
    }
    if (tool == tools.end())
      return;
    events.push_back(StreamEvent{
        .type = StreamEventType::ToolCallEnd, .text = "", .tool_call_id = tool->second.id, .tool_name = "", .error_message = "", .usage = std::nullopt});
    return;
  }
  if (type == "message_delta")
  {
    if (auto const parsed = parse_anthropic_usage(data))
    {
      if (!usage)
        usage = TokenUsage{};
      merge_usage(*usage, *parsed);
    }
    if (auto const delta = ava::core::json::object_field(data, "delta"))
    {
      if (auto const raw_stop_reason = ava::core::json::string_field(*delta, "stop_reason"); raw_stop_reason)
      {
        finish_reason = normalized_anthropic_finish_reason(*raw_stop_reason);
        if (finish_reason == ProviderFinishReason::Refusal)
        {
          if (auto explanation = stop_details_explanation(*delta); !explanation.empty())
          {
            events.push_back(StreamEvent{.type = StreamEventType::TextDelta,
                                         .text = std::move(explanation),
                                         .tool_call_id = "",
                                         .tool_name = "",
                                         .error_message = "",
                                         .usage = std::nullopt});
          }
        }
      }
    }
    return;
  }
  if (type == "message_stop")
  {
    message_stop_seen = true;
    if (!finish_reason)
      finish_reason = tools.empty() ? ProviderFinishReason::Completed : ProviderFinishReason::ToolCalls;
    events.push_back(StreamEvent{.type = StreamEventType::Done,
                                 .text = "",
                                 .tool_call_id = "",
                                 .tool_name = "",
                                 .error_message = "",
                                 .usage = std::move(usage),
                                 .finish_reason = finish_reason});
    usage = std::nullopt;
    finish_reason = std::nullopt;
    return;
  }
  if (type == "error")
  {
    error_seen = true;
    message_stop_seen = true;
    events.push_back(StreamEvent{.type = StreamEventType::Error,
                                 .text = "",
                                 .tool_call_id = "",
                                 .tool_name = "",
                                 .error_message = "Anthropic provider reported a streaming error",
                                 .usage = std::nullopt});
    return;
  }
  append_terminal_error("unrecognized Anthropic stream event");
}

}  // namespace

AnthropicProvider::AnthropicProvider(AnthropicProviderOptions options) : options_(std::move(options))
{
  if (options_.endpoint.empty())
    options_.base_url = normalize_anthropic_base_url(std::move(options_.base_url));
}

AnthropicProvider::AnthropicProvider(std::string base_url) : AnthropicProvider(AnthropicProviderOptions{.base_url = std::move(base_url)})
{
}

ava::core::Result<ava::http::HttpRequest> AnthropicProvider::build_request(ProviderRequest const& request, std::string_view access_token) const
{
  return build_anthropic_http_request(options_, request, access_token);
}

std::unique_ptr<StreamParser> AnthropicProvider::create_stream_parser() const
{
  return std::make_unique<AnthropicStreamParser>();
}

ava::core::VoidResult AnthropicProvider::apply_auth_options(ava::http::HttpRequest& request, ProviderAuthContext const& auth) const
{
  if (!options_.enable_oauth_header_swap || auth.credential_type != "oauth")
    return {};
  if (auth.access_token.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Anthropic OAuth token is required"));
  }

  request.headers.erase("x-api-key");
  request.headers["Authorization"] = "Bearer " + auth.access_token;
  auto existing_beta = request.headers.find("anthropic-beta");
  std::string beta = existing_beta == request.headers.end() ? std::string{} : existing_beta->second;
  request.headers["anthropic-beta"] = anthropic_beta_with_marker(std::move(beta), kAnthropicOAuthBeta);
  return {};
}

ava::core::Result<std::vector<StreamEvent>> AnthropicProvider::parse_response(ava::http::HttpResponse const& response, bool stream) const
{
  return stream ? parse_anthropic_sse_response(response) : parse_anthropic_response(response);
}

ava::core::Result<std::vector<StreamEvent>> AnthropicStreamParser::append(std::string_view chunk)
{
  return sse_.append(chunk, "Anthropic", [this](auto& events, std::string_view data) {
    append_event_for_data(events, tool_blocks_, reasoning_blocks_, usage_, finish_reason_, saw_data_, message_stop_seen_, error_seen_, data, sse_);
  });
}

ava::core::Result<std::vector<StreamEvent>> AnthropicStreamParser::finish()
{
  auto events = sse_.finish(
      "Anthropic",
      [this](auto& output, std::string_view data) {
        append_event_for_data(output, tool_blocks_, reasoning_blocks_, usage_, finish_reason_, saw_data_, message_stop_seen_, error_seen_, data, sse_);
      },
      [this](auto& output) {
        if (saw_data_ && !message_stop_seen_ && !error_seen_)
          append_stream_error(output, "Anthropic SSE stream ended before message_stop");
      });
  tool_blocks_.clear();
  reasoning_blocks_.clear();
  usage_ = std::nullopt;
  finish_reason_ = std::nullopt;
  saw_data_ = false;
  message_stop_seen_ = false;
  error_seen_ = false;
  return events;
}

ava::core::Result<std::vector<StreamEvent>> parse_anthropic_sse(std::string_view sse)
{
  AnthropicStreamParser parser;
  auto events = parser.append(sse);
  if (!events)
    return std::unexpected(std::move(events.error()));
  auto final_events = parser.finish();
  if (!final_events)
    return std::unexpected(std::move(final_events.error()));
  events->insert(events->end(), final_events->begin(), final_events->end());
  return events;
}

ava::core::Result<std::vector<StreamEvent>> parse_anthropic_sse_response(ava::http::HttpResponse const& response)
{
  if (response.status_code < 200 || response.status_code >= 300)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic HTTP request failed with status " + std::to_string(response.status_code));
    error.with_context("status", std::to_string(response.status_code));
    error.with_context("provider_error_kind", to_string(classify_provider_error(response)));
    if (auto const retry_after = ava::http::retry_after_header(response))
      error.with_context("retry_after", *retry_after);
    return std::unexpected(std::move(error));
  }
  return parse_anthropic_sse(response.body);
}

ava::core::Result<std::vector<StreamEvent>> parse_anthropic_response(ava::http::HttpResponse const& response)
{
  if (response.status_code < 200 || response.status_code >= 300)
    return parse_anthropic_sse_response(response);
  if (response.body.size() > kMaxProviderSseBufferedBytes)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic response parser limit exceeded"));
  if (!is_valid_json_object(response.body))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic response was malformed JSON"));
  std::vector<StreamEvent> events;
  bool parsed_content = false;
  auto const raw_finish_reason = ava::core::json::string_field(response.body, "stop_reason").value_or("");
  auto finish_reason = normalized_anthropic_finish_reason(raw_finish_reason);
  auto const content = ava::core::json::strict_objects_in_array_field(response.body, "content", kMaxProviderParserArrayItems);
  if (ava::core::json::field_value_start(response.body, "content") && !content)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic response parser limit exceeded"));
  for (auto const& block : content.value_or(std::vector<std::string>{}))
  {
    auto const type = ava::core::json::string_field(block, "type").value_or("");
    if (type == "text")
    {
      parsed_content = true;
      events.push_back(StreamEvent{.type = StreamEventType::TextDelta,
                                   .text = ava::core::json::string_field(block, "text").value_or(""),
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt});
    }
    else if (type == "refusal")
    {
      parsed_content = true;
      events.push_back(StreamEvent{.type = StreamEventType::TextDelta,
                                   .text = ava::core::json::string_field(block, "refusal").value_or(""),
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt});
    }
    else if (type == "thinking")
    {
      parsed_content = true;
      auto const thinking = ava::core::json::string_field(block, "thinking").value_or("");
      auto const signature = ava::core::json::string_field(block, "signature").value_or("");
      if (signature.size() > kMaxReasoningOpaqueBytes)
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic thinking signature exceeded byte limit"));
      }
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningStart,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = "anthropic_thinking"});
      if (!thinking.empty())
      {
        events.push_back(StreamEvent{.type = StreamEventType::ReasoningDelta,
                                     .text = thinking,
                                     .tool_call_id = "",
                                     .tool_name = "",
                                     .error_message = "",
                                     .usage = std::nullopt,
                                     .reasoning_format = "anthropic_thinking"});
      }
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningEnd,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = "anthropic_thinking",
                                   .reasoning_signature = signature});
    }
    else if (type == "redacted_thinking")
    {
      parsed_content = true;
      auto const redacted_data = ava::core::json::string_field(block, "data").value_or("");
      if (redacted_data.size() > kMaxReasoningOpaqueBytes)
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic redacted thinking payload exceeded byte limit"));
      }
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningStart,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = "anthropic_thinking",
                                   .redacted = true});
      events.push_back(StreamEvent{.type = StreamEventType::ReasoningEnd,
                                   .text = "",
                                   .tool_call_id = "",
                                   .tool_name = "",
                                   .error_message = "",
                                   .usage = std::nullopt,
                                   .reasoning_format = "anthropic_thinking",
                                   .reasoning_redacted_data = redacted_data,
                                   .redacted = true});
    }
    else if (type == "tool_use")
    {
      parsed_content = true;
      auto const id = ava::core::json::string_field(block, "id").value_or("");
      auto const name = ava::core::json::string_field(block, "name").value_or("");
      auto const input = ava::core::json::object_field(block, "input").value_or("{}");
      events.push_back(
          StreamEvent{.type = StreamEventType::ToolCallStart, .text = "", .tool_call_id = id, .tool_name = name, .error_message = "", .usage = std::nullopt});
      events.push_back(
          StreamEvent{.type = StreamEventType::ToolCallDelta, .text = input, .tool_call_id = id, .tool_name = "", .error_message = "", .usage = std::nullopt});
      events.push_back(
          StreamEvent{.type = StreamEventType::ToolCallEnd, .text = "", .tool_call_id = id, .tool_name = "", .error_message = "", .usage = std::nullopt});
    }
  }
  if (!parsed_content && finish_reason == ProviderFinishReason::Refusal)
  {
    if (has_stop_details(response.body))
    {
      parsed_content = true;
      if (auto explanation = stop_details_explanation(response.body); !explanation.empty())
      {
        events.push_back(StreamEvent{.type = StreamEventType::TextDelta,
                                     .text = std::move(explanation),
                                     .tool_call_id = "",
                                     .tool_name = "",
                                     .error_message = "",
                                     .usage = std::nullopt});
      }
    }
  }
  if (!parsed_content)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic response content is missing"));
  if (raw_finish_reason.empty())
  {
    bool const has_tool_call = std::ranges::any_of(events, [](StreamEvent const& event) { return event.type == StreamEventType::ToolCallStart; });
    finish_reason = has_tool_call ? ProviderFinishReason::ToolCalls : ProviderFinishReason::Completed;
  }
  events.push_back(StreamEvent{.type = StreamEventType::Done,
                               .text = "",
                               .tool_call_id = "",
                               .tool_name = "",
                               .error_message = "",
                               .usage = parse_anthropic_usage(response.body),
                               .finish_reason = finish_reason});
  return bounded_provider_events(std::move(events), "Anthropic");
}

std::optional<TokenUsage> parse_anthropic_usage(std::string_view body)
{
  auto const usage_object = ava::core::json::object_field(body, "usage");
  auto const usage_view = usage_object ? std::string_view(*usage_object) : body;
  TokenUsage usage;
  auto const regular_input_tokens = non_negative_integer_field(usage_view, "input_tokens");
  usage.output_tokens = non_negative_integer_field(usage_view, "output_tokens");
  usage.cache_read_tokens = non_negative_integer_field(usage_view, "cache_read_input_tokens");
  usage.cache_write_tokens = non_negative_integer_field(usage_view, "cache_creation_input_tokens");
  auto input_total = saturating_token_add(regular_input_tokens.value_or(0), usage.cache_read_tokens.value_or(0));
  input_total = saturating_token_add(input_total, usage.cache_write_tokens.value_or(0));
  if (input_total > 0)
    usage.input_tokens = input_total;
  long long const total = saturating_token_add(input_total, usage.output_tokens.value_or(0));
  if (total > 0)
    usage.total_tokens = total;
  if (!usage.input_tokens && !usage.output_tokens && !usage.cache_read_tokens && !usage.cache_write_tokens)
  {
    return std::nullopt;
  }
  return usage;
}

}  // namespace ava::provider
