#include "sys.h"
#include "ava/http/transport.h"
#include "ava/provider/gemini_provider.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/NoLogString.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

namespace ava::provider {
namespace {

constexpr std::string_view kDefaultGeminiBaseUrl = "https://generativelanguage.googleapis.com";
constexpr std::size_t kMaxGeminiNativePartsBytes = std::size_t{64} * 1024;
constexpr std::string_view kGeminiNativeEnvelopePrefix = R"({"type":"reasoning","parts":[)";

using detail::GeminiParseState;

std::string configured_base_url()
{
  if (char const* value = std::getenv("GEMINI_BASE_URL"); value != nullptr && value[0] != '\0')
    return value;
  return std::string(kDefaultGeminiBaseUrl);
}

std::string trim_trailing_slashes(std::string value)
{
  while (value.size() > 1 && value.back() == '/') value.pop_back();
  return value;
}

std::string normalize_gemini_base_url(std::string base_url)
{
  return trim_trailing_slashes(base_url.empty() ? configured_base_url() : std::move(base_url));
}

bool valid_gemini_identifier(std::string_view value)
{
  if (value.empty() || value.size() > 128)
    return false;
  for (char const ch : value)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte) != 0 || ch == '_' || ch == '-')
      continue;
    return false;
  }
  return true;
}

bool valid_gemini_model_id(std::string_view value)
{
  if (value.empty() || value.size() > 128)
    return false;
  for (char const ch : value)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte) != 0 || ch == '_' || ch == '-' || ch == '.')
      continue;
    return false;
  }
  return true;
}

ava::core::Result<std::string> gemini_model_path(std::string_view model_id)
{
  if (model_id.starts_with("models/"))
    model_id.remove_prefix(std::string_view("models/").size());
  if (!valid_gemini_model_id(model_id))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Gemini model id is invalid");
    error.with_context("model", std::string(model_id));
    return std::unexpected(std::move(error));
  }
  return "models/" + std::string(model_id);
}

std::string gemini_role(std::string_view role)
{
  return role == "assistant" ? "model" : "user";
}

ava::core::Error invalid_content_part_error(std::string message, std::size_t message_index, std::size_t part_index)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  error.with_context("provider", "gemini");
  error.with_context("message_index", std::to_string(message_index));
  error.with_context("content_part_index", std::to_string(part_index));
  return error;
}

bool gemini_thought(std::string_view part)
{
  auto const start = ava::core::json::field_value_start(part, "thought");
  return start && part.substr(*start, 4) == "true";
}

ava::core::VoidResult validate_gemini_content_parts(ProviderRequest const& request)
{
  if (auto valid_images = validate_image_content_parts(request, true); !valid_images)
    return valid_images;
  for (std::size_t message_index = 0; message_index < request.messages.size(); ++message_index)
  {
    auto const& message = request.messages[message_index];
    auto const role = gemini_role(message.role);
    for (std::size_t part_index = 0; part_index < message.content_parts.size(); ++part_index)
    {
      auto const& part = message.content_parts[part_index];
      switch (part.type)
      {
        case ContentPartType::Text:
          break;
        case ContentPartType::Reasoning:
          if (part.reasoning_format == "gemini" && !part.reasoning_signature.empty() && part.reasoning_native_item_json.empty())
            return std::unexpected(invalid_content_part_error("Gemini signatures require matching native part replay metadata", message_index, part_index));
          break;
        case ContentPartType::Image:
          if (part.data_base64.empty())
          {
            return std::unexpected(invalid_content_part_error("Gemini image content requires verified attachment bytes", message_index, part_index));
          }
          break;
        case ContentPartType::ToolUse:
          if (role != "model")
          {
            return std::unexpected(invalid_content_part_error("Gemini functionCall content requires assistant role", message_index, part_index));
          }
          if (part.tool_call_id.empty() || !valid_gemini_identifier(part.tool_name))
          {
            return std::unexpected(invalid_content_part_error("Gemini functionCall content requires id and a valid name", message_index, part_index));
          }
          if (!is_valid_json_object(part.input_json))
          {
            return std::unexpected(invalid_content_part_error("Gemini functionCall args must be a valid JSON object", message_index, part_index));
          }
          break;
        case ContentPartType::ToolResult:
          if (role != "user")
          {
            return std::unexpected(invalid_content_part_error("Gemini functionResponse content requires user role", message_index, part_index));
          }
          if (part.tool_call_id.empty() || !valid_gemini_identifier(part.tool_name))
          {
            return std::unexpected(invalid_content_part_error("Gemini functionResponse content requires id and a valid name", message_index, part_index));
          }
          break;
      }
    }
  }
  return {};
}

std::string text_part_json(std::string_view text)
{
  return "{\"text\":\"" + ava::core::json::escape(text) + "\"}";
}

std::string image_part_json(ContentPart const& part)
{
  return "{\"inlineData\":{\"mimeType\":\"" + ava::core::json::escape(part.mime_type) + "\",\"data\":\"" + ava::core::json::escape(part.data_base64) + "\"}}";
}

std::string function_call_part_json(ContentPart const& part)
{
  auto const args = is_valid_json_object(part.input_json) ? part.input_json : std::string("{}");
  return "{\"functionCall\":{\"id\":\"" + ava::core::json::escape(part.tool_call_id) + "\",\"name\":\"" + ava::core::json::escape(part.tool_name) +
         "\",\"args\":" + args + "}}";
}

std::string function_response_payload_json(ContentPart const& part)
{
  std::string json = "{\"content\":";
  if (is_valid_json_object(part.text))
  {
    json += part.text;
  }
  else
  {
    json += "\"" + ava::core::json::escape(part.text) + "\"";
  }
  if (part.is_error)
    json += ",\"is_error\":true";
  json += '}';
  return json;
}

std::string function_response_part_json(ContentPart const& part)
{
  return "{\"functionResponse\":{\"id\":\"" + ava::core::json::escape(part.tool_call_id) + "\",\"name\":\"" + ava::core::json::escape(part.tool_name) +
         "\",\"response\":" + function_response_payload_json(part) + "}}";
}

ava::core::Error invalid_native_parts_error()
{
  return {ava::core::ErrorCategory::InvalidArgument, "Gemini native replay metadata is invalid, exceeds its bound, or does not match assistant history"};
}

void append_json_member(std::string& object, std::string_view member)
{
  // Callers pass complete, non-empty objects produced by this module.
  object.pop_back();
  object += ',';
  object += member;
  object += '}';
}

std::string thought_signature_member(std::string_view signature)
{
  return R"("thoughtSignature":")" + ava::core::json::escape(signature) + '"';
}

struct ReplayHistory
{
  ava::core::NoLogString text;
  ava::core::NoLogString thought;
  // These parts belong to the message used throughout replay canonicalization.
  std::vector<ContentPart const*> calls;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ReplayCursors
{
  std::size_t text_offset = 0;
  std::size_t thought_offset = 0;
  std::size_t call_index = 0;
  bool signed_part_seen = false;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

ava::core::Result<ReplayHistory> collect_replay_history(ChatMessage const& message)
{
  ReplayHistory history;
  for (auto const& part : message.content_parts)
  {
    if (part.type == ContentPartType::Text || (part.type == ContentPartType::Reasoning && part.reasoning_format == "gemini" && !part.redacted))
    {
      auto& target = part.type == ContentPartType::Text ? history.text : history.thought;
      if (part.text.size() > kMaxGeminiNativePartsBytes - target.size())
        return std::unexpected(invalid_native_parts_error());
      target += part.text;
    }
    else if (part.type == ContentPartType::ToolUse)
      history.calls.push_back(&part);
    else
      return std::unexpected(invalid_native_parts_error());
  }
  return history;
}

ava::core::Result<std::string> canonicalize_native_part(std::string_view native_part, ReplayHistory const& history, ReplayCursors& cursors)
{
  auto const native_text = ava::core::json::string_field(native_part, "text");
  auto const function = ava::core::json::object_field(native_part, "functionCall");
  if (native_text.has_value() == function.has_value())
    return std::unexpected(invalid_native_parts_error());
  std::string canonical;
  if (native_text)
  {
    bool const is_thought = gemini_thought(native_part);
    auto const& expected = is_thought ? history.thought : history.text;
    auto& offset = is_thought ? cursors.thought_offset : cursors.text_offset;
    if (std::string_view(expected).substr(offset, native_text->size()) != *native_text)
      return std::unexpected(invalid_native_parts_error());
    offset += native_text->size();
    canonical = text_part_json(*native_text);
    if (is_thought)
      append_json_member(canonical, R"("thought":true)");
  }
  else
  {
    if (cursors.call_index >= history.calls.size())
      return std::unexpected(invalid_native_parts_error());
    auto const& call = *history.calls[cursors.call_index++];
    if (ava::core::json::string_field(*function, "id").value_or("") != call.tool_call_id ||
        ava::core::json::string_field(*function, "name").value_or("") != call.tool_name)
      return std::unexpected(invalid_native_parts_error());
    canonical = function_call_part_json(call);
  }
  if (ava::core::json::field_value_start(native_part, "thoughtSignature"))
  {
    auto const signature = ava::core::json::string_field(native_part, "thoughtSignature");
    if (!signature || signature->empty())
      return std::unexpected(invalid_native_parts_error());
    append_json_member(canonical, thought_signature_member(*signature));
    cursors.signed_part_seen = true;
  }
  return canonical;
}

ava::core::Result<std::vector<std::string>> gemini_replay_parts(ChatMessage const& message)
{
  std::string_view native_json;
  for (auto const& part : message.content_parts)
  {
    if (part.type == ContentPartType::Reasoning && part.reasoning_format == "gemini" && !part.reasoning_native_item_json.empty())
    {
      if (!native_json.empty())
        return std::unexpected(invalid_native_parts_error());
      native_json = part.reasoning_native_item_json;
    }
  }
  if (native_json.empty())
    return std::vector<std::string>{};
  if (message.role != "assistant" || native_json.size() > kMaxGeminiNativePartsBytes || !ava::core::json::is_valid_object(native_json) ||
      ava::core::json::string_field(native_json, "type").value_or("") != "reasoning")
    return std::unexpected(invalid_native_parts_error());
  auto parts = ava::core::json::strict_objects_in_array_field(native_json, "parts", kMaxProviderParserArrayItems);
  if (!parts || parts->empty())
    return std::unexpected(invalid_native_parts_error());
  auto history = collect_replay_history(message);
  if (!history)
    return std::unexpected(std::move(history.error()));
  ReplayCursors cursors;
  for (auto& native_part : *parts)
  {
    auto canonical = canonicalize_native_part(native_part, *history, cursors);
    if (!canonical)
      return std::unexpected(std::move(canonical.error()));
    native_part = std::move(*canonical);
  }
  if (!cursors.signed_part_seen || cursors.text_offset != history->text.size() || cursors.thought_offset != history->thought.size() ||
      cursors.call_index != history->calls.size())
    return std::unexpected(invalid_native_parts_error());
  return std::move(*parts);
}

ava::core::Result<std::string> gemini_content_json(ChatMessage const& message)
{
  std::string json = "{\"role\":\"" + gemini_role(message.role) + "\",\"parts\":[";
  bool first = true;
  auto append_part = [&](std::string part_json) {
    if (!first)
      json += ',';
    first = false;
    json += std::move(part_json);
  };
  auto replay = gemini_replay_parts(message);
  if (!replay)
    return std::unexpected(std::move(replay.error()));
  if (!replay->empty())
  {
    for (auto& part : *replay)
      append_part(std::move(part));
    json += "]}";
    return json;
  }

  if (message.content_parts.empty())
  {
    append_part(text_part_json(message.content));
  }
  else
  {
    for (auto const& part : message.content_parts)
    {
      switch (part.type)
      {
        case ContentPartType::Text:
          if (!part.text.empty())
            append_part(text_part_json(part.text));
          break;
        case ContentPartType::Image:
          append_part(image_part_json(part));
          break;
        case ContentPartType::ToolUse:
          append_part(function_call_part_json(part));
          break;
        case ContentPartType::ToolResult:
          append_part(function_response_part_json(part));
          break;
        case ContentPartType::Reasoning:
          break;
      }
    }
    if (first)
      append_part(text_part_json(message.content));
  }

  json += "]}";
  return json;
}

ava::core::Result<std::string> function_declaration_json(std::string_view schema)
{
  if (!is_valid_json_object(schema))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Gemini tool JSON must be valid JSON"));
  }

  std::string function_storage;
  std::string_view scope = schema;
  if (auto const function = ava::core::json::object_field(schema, "function"))
  {
    function_storage = *function;
    scope = function_storage;
  }

  auto const name = ava::core::json::string_field(scope, "name");
  if (!name || !valid_gemini_identifier(*name))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Gemini tool JSON requires a valid function name"));
  }
  auto const description = ava::core::json::string_field(scope, "description").value_or("");
  auto parameters = ava::core::json::object_field(scope, "parameters");
  if (!parameters)
    parameters = ava::core::json::object_field(scope, "parametersJsonSchema");
  if (!parameters)
    parameters = std::string("{\"type\":\"object\"}");
  if (!is_valid_json_object(*parameters))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Gemini tool parameters must be valid JSON"));
  }

  return "{\"name\":\"" + ava::core::json::escape(*name) + "\",\"description\":\"" + ava::core::json::escape(description) + "\",\"parameters\":" + *parameters +
         "}";
}

ava::core::Result<std::string> gemini_request_body_json(ProviderRequest const& request)
{
  std::string body = "{";
  bool first_member = true;
  auto append_member = [&](std::string member) {
    if (!first_member)
      body += ',';
    first_member = false;
    body += std::move(member);
  };

  if (!request.system_prompt.empty())
  {
    append_member("\"systemInstruction\":{\"parts\":[" + text_part_json(request.system_prompt) + "]}");
  }

  std::string contents = "\"contents\":[";
  for (std::size_t index = 0; index < request.messages.size(); ++index)
  {
    if (index > 0)
      contents += ',';
    auto content = gemini_content_json(request.messages[index]);
    if (!content)
    {
      auto error = std::move(content.error());
      error.with_context("message_index", std::to_string(index));
      return std::unexpected(std::move(error));
    }
    contents += *content;
  }
  contents += ']';
  append_member(std::move(contents));

  if (!request.tools_json.empty())
  {
    std::string tools = "\"tools\":[{\"functionDeclarations\":[";
    for (std::size_t index = 0; index < request.tools_json.size(); ++index)
    {
      auto declaration = function_declaration_json(request.tools_json[index]);
      if (!declaration)
        return std::unexpected(std::move(declaration.error()));
      if (index > 0)
        tools += ',';
      tools += *declaration;
    }
    tools += "]}]";
    append_member(std::move(tools));
  }

  if (request.max_output_tokens && *request.max_output_tokens > 0)
  {
    append_member("\"generationConfig\":{\"maxOutputTokens\":" + std::to_string(*request.max_output_tokens) + "}");
  }

  body += '}';
  return body;
}

std::optional<long long> non_negative_integer_field(std::string_view object, std::string_view key)
{
  auto const value = ava::core::json::integer_field(object, key);
  if (!value || *value < 0)
    return std::nullopt;
  return value;
}

void merge_usage(TokenUsage& target, TokenUsage const& source)
{
  if (source.input_tokens)
    target.input_tokens = source.input_tokens;
  if (source.output_tokens)
    target.output_tokens = source.output_tokens;
  if (source.reasoning_tokens)
    target.reasoning_tokens = source.reasoning_tokens;
  if (source.cache_read_tokens)
    target.cache_read_tokens = source.cache_read_tokens;
  if (source.total_tokens)
    target.total_tokens = source.total_tokens;
}

ProviderFinishReason normalized_finish_reason(std::string_view reason)
{
  return normalize_provider_finish_reason(ProviderProtocol::Gemini, reason);
}

std::string prompt_block_reason(std::string_view body)
{
  auto const prompt_feedback = ava::core::json::object_field(body, "promptFeedback");
  if (!prompt_feedback)
    return {};
  return ava::core::json::string_field(*prompt_feedback, "blockReason").value_or("");
}

void append_error_event(std::vector<StreamEvent>& events, std::string message)
{
  events.push_back(
      StreamEvent{.type = StreamEventType::Error, .text = "", .tool_call_id = "", .tool_name = "", .error_message = std::move(message), .usage = std::nullopt});
}

void append_reasoning_end(std::vector<StreamEvent>& events, GeminiParseState& state)
{
  if (!state.reasoning_active)
    return;
  events.push_back(StreamEvent{.type = StreamEventType::ReasoningEnd,
                               .text = "",
                               .tool_call_id = "",
                               .tool_name = "",
                               .error_message = "",
                               .usage = std::nullopt,
                               .reasoning_format = "gemini"});
  state.reasoning_active = false;
}

void append_done_event(std::vector<StreamEvent>& events, GeminiParseState& state)
{
  if (state.has_thought_signature && !state.native_parts_overflowed)
  {
    // The session codec accepts provider-private reasoning envelopes. Public
    // stream bridges clear the envelope before publishing any event.
    events.push_back(StreamEvent{.type = StreamEventType::ReasoningEnd,
                                 .text = "",
                                 .tool_call_id = "",
                                 .tool_name = "",
                                 .error_message = "",
                                 .usage = std::nullopt,
                                 .reasoning_format = "gemini",
                                 .reasoning_native_item_json = std::string(kGeminiNativeEnvelopePrefix) + state.native_parts_json + "]}",
                                 .reasoning_signature_present = true});
    state.reasoning_active = false;
  }
  else
    append_reasoning_end(events, state);
  state.native_parts_json.clear();
  state.native_part_count = 0;
  state.has_thought_signature = false;
  events.push_back(StreamEvent{.type = StreamEventType::Done,
                               .text = "",
                               .tool_call_id = "",
                               .tool_name = "",
                               .error_message = "",
                               .usage = state.usage,
                               .finish_reason = state.finish_reason});
}

std::string append_function_call_events(std::vector<StreamEvent>& events, std::string_view function_call, GeminiParseState& state)
{
  auto const name = ava::core::json::string_field(function_call, "name").value_or("");
  if (name.empty())
  {
    state.error_seen = true;
    append_error_event(events, "Gemini functionCall is missing name");
    return {};
  }
  auto id = ava::core::json::string_field(function_call, "id").value_or("");
  if (id.empty())
    id = state.fallback_tool_call_prefix + "_" + std::to_string(state.next_tool_call_index++);
  auto args = ava::core::json::object_field(function_call, "args").value_or("{}");
  if (!is_valid_json_object(args))
  {
    state.error_seen = true;
    append_error_event(events, "Gemini functionCall args must be a JSON object");
    return {};
  }

  // Arguments already live in the canonical ToolUse history item. Retain only
  // the logical identity needed to attach its signature during reconstruction.
  auto native_part = state.native_parts_overflowed
                         ? std::string{}
                         : R"({"functionCall":{"id":")" + ava::core::json::escape(id) + R"(","name":")" + ava::core::json::escape(name) + R"("}})";
  state.parsed_content = true;
  events.push_back(
      StreamEvent{.type = StreamEventType::ToolCallStart, .text = "", .tool_call_id = id, .tool_name = name, .error_message = "", .usage = std::nullopt});
  events.push_back(StreamEvent{
      .type = StreamEventType::ToolCallDelta, .text = std::move(args), .tool_call_id = id, .tool_name = "", .error_message = "", .usage = std::nullopt});
  events.push_back(StreamEvent{
      .type = StreamEventType::ToolCallEnd, .text = "", .tool_call_id = std::move(id), .tool_name = "", .error_message = "", .usage = std::nullopt});
  return native_part;
}

void discard_native_parts(GeminiParseState& state)
{
  state.native_parts_json.clear();
  state.native_part_count = 0;
  state.native_parts_overflowed = true;
  state.has_thought_signature = false;
}

void retain_native_part(GeminiParseState& state, std::string native_part, std::optional<std::string> const& signature)
{
  constexpr auto max_parts_bytes = kMaxGeminiNativePartsBytes - kGeminiNativeEnvelopePrefix.size() - 2;
  if (native_part.size() > max_parts_bytes || (signature && signature->size() > max_parts_bytes - native_part.size()))
  {
    discard_native_parts(state);
    return;
  }
  if (signature)
    append_json_member(native_part, thought_signature_member(*signature));
  auto const separator_bytes = state.native_part_count == 0 ? std::size_t{0} : std::size_t{1};
  if (state.native_parts_overflowed || state.native_part_count >= kMaxProviderParserArrayItems ||
      native_part.size() + separator_bytes > max_parts_bytes - state.native_parts_json.size())
  {
    discard_native_parts(state);
    return;
  }
  if (separator_bytes != 0)
    state.native_parts_json += ',';
  state.native_parts_json += native_part;
  ++state.native_part_count;
}

std::string append_text_part_events(std::vector<StreamEvent>& events, std::string_view text, bool thought, GeminiParseState& state)
{
  std::string native_part;
  if (!state.native_parts_overflowed)
  {
    native_part = text_part_json(text);
    if (thought)
      append_json_member(native_part, R"("thought":true)");
  }
  if (!text.empty())
  {
    events.push_back(StreamEvent{.type = thought ? StreamEventType::ReasoningDelta : StreamEventType::TextDelta,
                                 .text = std::string(text),
                                 .tool_call_id = "",
                                 .tool_name = "",
                                 .error_message = "",
                                 .usage = std::nullopt,
                                 .reasoning_format = thought ? "gemini" : ""});
    state.reasoning_active = thought;
  }
  return native_part;
}

void append_part_events(std::vector<StreamEvent>& events, std::string_view part, GeminiParseState& state)
{
  if (state.error_seen)
    return;
  auto const signature = ava::core::json::string_field(part, "thoughtSignature");
  if (ava::core::json::field_value_start(part, "thoughtSignature") && (!signature || signature->empty()))
  {
    state.error_seen = true;
    append_error_event(events, "Gemini thoughtSignature must be a non-empty opaque string");
    return;
  }
  auto const text = ava::core::json::string_field(part, "text");
  auto const function_call = ava::core::json::object_field(part, "functionCall");
  if ((text && function_call) || (signature && !text && !function_call))
  {
    state.error_seen = true;
    append_error_event(events, "Gemini signed part requires exactly one text or functionCall payload");
    return;
  }
  // Replay metadata has a separate, smaller budget than ordinary model
  // output. On overflow preserve the response and fall back to unsigned
  // history, never to a partial signature envelope.
  if ((text && text->size() > kMaxGeminiNativePartsBytes) || (signature && signature->size() > kMaxGeminiNativePartsBytes))
    discard_native_parts(state);
  if (!state.native_parts_overflowed)
    state.has_thought_signature = state.has_thought_signature || signature.has_value();
  bool const thought = text && gemini_thought(part);
  if (!thought)
    append_reasoning_end(events, state);
  std::string native_part;
  if (text)
  {
    state.parsed_content = state.parsed_content || !text->empty() || signature.has_value();
    native_part = append_text_part_events(events, *text, thought, state);
  }
  if (function_call)
    native_part = append_function_call_events(events, *function_call, state);
  if (state.error_seen || state.native_parts_overflowed)
    return;
  if (!native_part.empty())
    retain_native_part(state, std::move(native_part), signature);
}

void append_response_events(std::vector<StreamEvent>& events, GeminiParseState& state, std::string_view body)
{
  if (ava::core::json::object_field(body, "error"))
  {
    state.error_seen = true;
    append_error_event(events, "Gemini provider reported an error");
    return;
  }
  if (auto usage = parse_gemini_usage(body))
  {
    if (!state.usage)
      state.usage = TokenUsage{};
    merge_usage(*state.usage, *usage);
  }
  if (auto block_reason = prompt_block_reason(body); !block_reason.empty())
  {
    state.finish_reason = normalize_provider_finish_reason(ProviderProtocol::Gemini, block_reason);
    if (state.finish_reason == ProviderFinishReason::Refusal)
    {
      // A prompt-level safety block is a semantic refusal even though Gemini
      // returns no candidate content.
      state.parsed_content = true;
    }
    else
    {
      state.error_seen = true;
      append_error_event(events, "Gemini prompt was blocked");
    }
    return;
  }

  auto const candidates = ava::core::json::strict_objects_in_array_field(body, "candidates", kMaxProviderParserArrayItems);
  if (ava::core::json::field_value_start(body, "candidates") && !candidates)
  {
    state.error_seen = true;
    append_error_event(events, "Gemini response parser limit exceeded");
    return;
  }
  for (std::size_t candidate_index = 0; candidates && candidate_index < candidates->size(); ++candidate_index)
  {
    auto const& candidate = candidates->at(candidate_index);
    if (auto const content = ava::core::json::object_field(candidate, "content"))
    {
      auto const parts = ava::core::json::strict_objects_in_array_field(*content, "parts", kMaxProviderParserArrayItems);
      if (ava::core::json::field_value_start(*content, "parts") && !parts)
      {
        state.error_seen = true;
        append_error_event(events, "Gemini response parser limit exceeded");
        return;
      }
      for (std::size_t part_index = 0; parts && part_index < parts->size(); ++part_index)
      {
        append_part_events(events, parts->at(part_index), state);
      }
    }
    if (auto const finish_reason = ava::core::json::string_field(candidate, "finishReason"); finish_reason && !finish_reason->empty())
    {
      state.finish_reason = normalized_finish_reason(*finish_reason);
    }
  }
}

void append_event_for_data(std::vector<StreamEvent>& events, GeminiParseState& state, bool& saw_data, bool& done_seen, std::string_view data)
{
  if (done_seen || state.error_seen)
    return;
  if (data == "[DONE]")
  {
    done_seen = true;
    append_done_event(events, state);
    state.usage = std::nullopt;
    state.finish_reason = std::nullopt;
    return;
  }
  saw_data = true;
  if (!is_json_object_shape(data))
  {
    state.error_seen = true;
    append_error_event(events, "malformed Gemini SSE event");
    return;
  }
  append_response_events(events, state, data);
}

void append_events_for_sse_line(std::vector<StreamEvent>& events, GeminiParseState& state, bool& saw_data, bool& done_seen, std::string& data, std::string line)
{
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  if (line.empty())
  {
    if (!data.empty())
    {
      append_event_for_data(events, state, saw_data, done_seen, data);
      data.clear();
    }
    return;
  }
  if (line.starts_with("data:"))
  {
    if (!data.empty())
      data.push_back('\n');
    auto value = std::string_view(line).substr(5);
    if (!value.empty() && value.front() == ' ')
      value.remove_prefix(1);
    data.append(value);
  }
}

ava::core::Result<std::vector<StreamEvent>> gemini_http_error(ava::http::HttpResponse const& response)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "Gemini HTTP request failed with status " + std::to_string(response.status_code));
  error.with_context("status", std::to_string(response.status_code));
  error.with_context("provider_error_kind", to_string(classify_provider_error(response)));
  if (auto const retry_after = ava::http::retry_after_header(response))
    error.with_context("retry_after", *retry_after);
  return std::unexpected(std::move(error));
}

}  // namespace

GeminiProvider::GeminiProvider(std::string base_url) : base_url_(normalize_gemini_base_url(std::move(base_url)))
{
}

ava::core::Result<ava::http::HttpRequest> GeminiProvider::build_request(ProviderRequest const& request, std::string_view access_token) const
{
  auto model_path = gemini_model_path(request.model_id);
  if (!model_path)
    return std::unexpected(std::move(model_path.error()));
  if (access_token.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Gemini API key is required"));
  }
  if (request.reasoning)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Gemini reasoning options are not supported"));
  }
  if (auto valid_parts = validate_gemini_content_parts(request); !valid_parts)
    return std::unexpected(std::move(valid_parts.error()));
  auto body = gemini_request_body_json(request);
  if (!body)
    return std::unexpected(std::move(body.error()));

  std::string url = base_url_ + "/v1beta/" + *model_path + (request.stream ? ":streamGenerateContent?alt=sse" : ":generateContent");
  return ava::http::HttpRequest{.method = "POST",
                                .url = std::move(url),
                                .headers = {{"x-goog-api-key", std::string(access_token)},
                                            {"Content-Type", "application/json"},
                                            {"Accept", request.stream ? "text/event-stream" : "application/json"}},
                                .body = std::move(*body),
                                .timeout_ms = 60000,
                                .follow_redirects = false,
                                .include_response_headers = false,
                                .resolve_hosts = {}};
}

std::unique_ptr<StreamParser> GeminiProvider::create_stream_parser() const
{
  return std::make_unique<GeminiStreamParser>();
}

ava::core::Result<std::vector<StreamEvent>> GeminiProvider::parse_response(ava::http::HttpResponse const& response, bool stream) const
{
  return stream ? parse_gemini_sse_response(response) : parse_gemini_response(response);
}

GeminiStreamParser::GeminiStreamParser() : state_{.fallback_tool_call_prefix = ava::core::make_id("gemini_call")}
{
}

ava::core::Result<std::vector<StreamEvent>> GeminiStreamParser::append(std::string_view chunk)
{
  std::vector<StreamEvent> events;
  pending_line_.append(chunk);
  std::size_t line_start = 0;
  std::size_t search_from = scan_offset_;
  state_.parsed_content = false;
  while (true)
  {
    auto const newline = pending_line_.find('\n', search_from);
    if (newline == std::string::npos)
      break;
    append_events_for_sse_line(events, state_, saw_data_, done_seen_, data_, pending_line_.substr(line_start, newline - line_start));
    line_start = newline + 1;
    search_from = line_start;
  }
  if (line_start > 0)
    pending_line_.erase(0, line_start);
  scan_offset_ = pending_line_.size();
  return events;
}

ava::core::Result<std::vector<StreamEvent>> GeminiStreamParser::finish()
{
  std::vector<StreamEvent> events;
  state_.parsed_content = false;
  if (!pending_line_.empty())
  {
    append_events_for_sse_line(events, state_, saw_data_, done_seen_, data_, std::move(pending_line_));
    pending_line_.clear();
  }
  scan_offset_ = 0;
  if (!data_.empty())
  {
    append_event_for_data(events, state_, saw_data_, done_seen_, data_);
    data_.clear();
  }
  if (saw_data_ && !done_seen_ && !state_.error_seen)
  {
    append_done_event(events, state_);
  }
  state_.reset_response();
  saw_data_ = false;
  done_seen_ = false;
  return events;
}

ava::core::Result<std::vector<StreamEvent>> parse_gemini_sse(std::string_view sse)
{
  GeminiStreamParser parser;
  auto events = parser.append(sse);
  if (!events)
    return std::unexpected(std::move(events.error()));
  auto final_events = parser.finish();
  if (!final_events)
    return std::unexpected(std::move(final_events.error()));
  events->insert(events->end(), final_events->begin(), final_events->end());
  return events;
}

ava::core::Result<std::vector<StreamEvent>> parse_gemini_sse_response(ava::http::HttpResponse const& response)
{
  if (response.status_code < 200 || response.status_code >= 300)
    return gemini_http_error(response);
  return parse_gemini_sse(response.body);
}

ava::core::Result<std::vector<StreamEvent>> parse_gemini_response(ava::http::HttpResponse const& response)
{
  if (response.status_code < 200 || response.status_code >= 300)
    return gemini_http_error(response);
  std::vector<StreamEvent> events;
  GeminiParseState state{.fallback_tool_call_prefix = ava::core::make_id("gemini_call")};
  if (!is_json_object_shape(response.body))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Gemini response was malformed JSON"));
  append_response_events(events, state, response.body);
  if (state.error_seen)
    return events;
  if (!state.parsed_content)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "Gemini response content is missing");
    if (state.finish_reason.has_value())
      error.with_context("finish_reason", std::string(to_string(*state.finish_reason)));
    return std::unexpected(std::move(error));
  }
  append_done_event(events, state);
  return events;
}

std::optional<TokenUsage> parse_gemini_usage(std::string_view body)
{
  auto const usage_object = ava::core::json::object_field(body, "usageMetadata");
  auto const usage_view = usage_object ? std::string_view(*usage_object) : body;
  TokenUsage usage;
  usage.input_tokens = non_negative_integer_field(usage_view, "promptTokenCount");
  usage.output_tokens = non_negative_integer_field(usage_view, "candidatesTokenCount");
  usage.reasoning_tokens = non_negative_integer_field(usage_view, "thoughtsTokenCount");
  usage.cache_read_tokens = non_negative_integer_field(usage_view, "cachedContentTokenCount");
  usage.total_tokens = non_negative_integer_field(usage_view, "totalTokenCount");
  if (!usage.input_tokens && !usage.output_tokens && !usage.reasoning_tokens && !usage.cache_read_tokens && !usage.total_tokens)
    return std::nullopt;
  return usage;
}

}  // namespace ava::provider
