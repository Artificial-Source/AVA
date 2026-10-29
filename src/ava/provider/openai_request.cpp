#include "sys.h"
#include "ava/http/transport.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/openai_reasoning.h"
#include "ava/provider/openai_request.h"
#include "ava/provider/openai_response_parser_detail.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/json.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ava::provider {
namespace detail {
namespace {

constexpr std::string_view kCodexResponsesUrl = "https://chatgpt.com/backend-api/codex/responses";

std::string image_data_url(ContentPart const& part)
{
  return "data:" + ava::core::json::escape(part.mime_type) + ";base64," + ava::core::json::escape(part.data_base64);
}

bool has_image_parts(ChatMessage const& message)
{
  for (auto const& part : message.content_parts)
  {
    if (part.type == ContentPartType::Image)
      return true;
  }
  return false;
}

std::optional<std::string> native_content_item_json(ChatMessage const& message, std::vector<ContentPart const*> const& parts)
{
  bool has_image = false;
  std::string text;
  for (auto const* part : parts)
  {
    if (part->type == ContentPartType::Text)
    {
      if (part->text.empty())
        continue;
      if (!text.empty())
        text += "\n\n";
      text += part->text;
    }
    else if (part->type == ContentPartType::Reasoning)
    {
      // When native replay cannot be used, retain readable reasoning summaries
      // in the ordinary synthetic history without exposing private metadata.
      if (!part->text.empty())
      {
        if (!text.empty())
          text += "\n\n";
        text += part->text;
      }
    }
    else if (part->type == ContentPartType::Image)
    {
      has_image = true;
    }
  }
  if (!has_image && text.empty())
    return std::nullopt;
  if (!has_image)
  {
    return "{\"role\":\"" + ava::core::json::escape(message.role) + "\",\"content\":\"" + ava::core::json::escape(text) + "\"}";
  }

  std::string json = "{\"role\":\"" + ava::core::json::escape(message.role) + "\",\"content\":[";
  bool first = true;
  for (auto const* part : parts)
  {
    if (part->type == ContentPartType::Text)
    {
      if (part->text.empty())
        continue;
      if (!first)
        json += ',';
      first = false;
      json += "{\"type\":\"input_text\",\"text\":\"" + ava::core::json::escape(part->text) + "\"}";
    }
    else if (part->type == ContentPartType::Reasoning)
    {
      if (part->text.empty())
        continue;
      if (!first)
        json += ',';
      first = false;
      json += "{\"type\":\"input_text\",\"text\":\"" + ava::core::json::escape(part->text) + "\"}";
    }
    else if (part->type == ContentPartType::Image)
    {
      if (!first)
        json += ',';
      first = false;
      json += "{\"type\":\"input_image\",\"image_url\":\"" + image_data_url(*part) + "\"}";
    }
  }
  if (first)
    return std::nullopt;
  json += "]}";
  return json;
}

std::string input_item_json(ChatMessage const& message)
{
  if (!message.content_parts.empty() && has_image_parts(message))
  {
    std::vector<ContentPart const*> parts;
    parts.reserve(message.content_parts.size());
    for (auto const& part : message.content_parts) parts.push_back(&part);
    if (auto item = native_content_item_json(message, parts))
      return std::move(*item);
  }
  return "{\"role\":\"" + ava::core::json::escape(message.role) + "\",\"content\":\"" + ava::core::json::escape(message.content) + "\"}";
}

bool valid_tool_use_part(ContentPart const& part)
{
  return is_valid_openai_opaque_id(part.tool_call_id) && !part.tool_name.empty() && is_valid_json_object(part.input_json);
}

bool valid_tool_result_part(ContentPart const& part)
{
  return is_valid_openai_opaque_id(part.tool_call_id);
}

bool valid_openai_reasoning_item_part(ContentPart const& part)
{
  return part.type == ContentPartType::Reasoning && part.reasoning_format == detail::kOpenAIResponsesReasoningFormat &&
         is_valid_openai_native_reasoning_item_json(part.reasoning_native_item_json);
}

bool has_unreplayable_openai_reasoning(std::vector<ContentPart> const& parts)
{
  for (auto const& part : parts)
  {
    if (part.type == ContentPartType::Reasoning && part.reasoning_format == detail::kOpenAIResponsesReasoningFormat && !valid_openai_reasoning_item_part(part))
      return true;
  }
  return false;
}

using NativeToolPartMask = std::vector<std::vector<bool>>;

NativeToolPartMask paired_native_tool_part_mask(std::vector<ChatMessage> const& messages)
{
  NativeToolPartMask mask;
  mask.reserve(messages.size());
  for (auto const& message : messages) mask.emplace_back(message.content_parts.size(), false);

  for (std::size_t assistant_index = 0; assistant_index + 1 < messages.size(); ++assistant_index)
  {
    auto const& assistant = messages[assistant_index];
    auto const& result = messages[assistant_index + 1];
    if (assistant.role != "assistant" || result.role != "user")
      continue;
    if (has_unreplayable_openai_reasoning(assistant.content_parts))
      continue;

    std::vector<std::size_t> tool_use_indexes;
    bool malformed = false;
    for (std::size_t part_index = 0; part_index < assistant.content_parts.size(); ++part_index)
    {
      auto const& part = assistant.content_parts[part_index];
      if (part.type == ContentPartType::ToolResult)
      {
        malformed = true;
        break;
      }
      if (part.type == ContentPartType::ToolUse)
      {
        if (!valid_tool_use_part(part))
        {
          malformed = true;
          break;
        }
        tool_use_indexes.push_back(part_index);
      }
    }
    if (malformed || tool_use_indexes.empty())
      continue;

    std::vector<bool> matched_tool_uses(tool_use_indexes.size(), false);
    std::vector<std::size_t> tool_result_indexes;
    for (std::size_t part_index = 0; part_index < result.content_parts.size(); ++part_index)
    {
      auto const& part = result.content_parts[part_index];
      if (part.type == ContentPartType::ToolUse)
      {
        malformed = true;
        break;
      }
      if (part.type != ContentPartType::ToolResult)
        continue;
      if (!valid_tool_result_part(part))
      {
        malformed = true;
        break;
      }
      bool matched = false;
      for (std::size_t use_index = 0; use_index < tool_use_indexes.size(); ++use_index)
      {
        if (!matched_tool_uses[use_index] && assistant.content_parts[tool_use_indexes[use_index]].tool_call_id == part.tool_call_id)
        {
          matched_tool_uses[use_index] = true;
          matched = true;
          break;
        }
      }
      if (!matched)
      {
        malformed = true;
        break;
      }
      tool_result_indexes.push_back(part_index);
    }
    if (malformed || tool_result_indexes.size() != tool_use_indexes.size())
      continue;
    for (bool const matched : matched_tool_uses)
    {
      if (!matched)
      {
        malformed = true;
        break;
      }
    }
    if (malformed)
      continue;

    for (auto const part_index : tool_use_indexes) mask[assistant_index][part_index] = true;
    for (auto const part_index : tool_result_indexes) mask[assistant_index + 1][part_index] = true;
    ++assistant_index;
  }
  return mask;
}

bool valid_openai_assistant_text_item_part(ContentPart const& part)
{
  // A valid provider item ID is sufficient to preserve an output message as a
  // native Responses item. Unknown phase is an absence of phase metadata, not
  // a reason to discard the opaque provider identity or alter item order.
  return part.type == ContentPartType::Text && !part.provider_item_id.empty() && is_valid_openai_opaque_id(part.provider_item_id);
}

std::string bounded_openai_message_item_id(std::string_view provider_item_id)
{
  // Responses rejects message item IDs above 64 bytes. Preserve normal IDs
  // exactly; the fallback is deterministic and derived only from the opaque
  // ID, never replayed text or any other user/model content.
  if (provider_item_id.size() <= 64 && is_valid_openai_opaque_id(provider_item_id))
    return std::string(provider_item_id);
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char const byte : provider_item_id)
  {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string fallback = "msg_ava_";
  for (int shift = 60; shift >= 0; shift -= 4) fallback += hex[(hash >> shift) & 0xFU];
  return fallback;
}

std::string assistant_message_output_item_json(ContentPart const& part)
{
  std::string item = "{\"type\":\"message\",\"id\":\"" + ava::core::json::escape(bounded_openai_message_item_id(part.provider_item_id)) +
                     "\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"" + ava::core::json::escape(part.text) +
                     "\",\"annotations\":[]}],\"status\":\"completed\"";
  if (part.assistant_phase == AssistantPhase::Commentary)
    item += ",\"phase\":\"commentary\"";
  else if (part.assistant_phase == AssistantPhase::FinalAnswer)
    item += ",\"phase\":\"final_answer\"";
  item += '}';
  return item;
}

NativeToolPartMask native_replay_part_mask(std::vector<ChatMessage> const& messages)
{
  auto mask = paired_native_tool_part_mask(messages);
  for (std::size_t message_index = 0; message_index < messages.size(); ++message_index)
  {
    auto const& message = messages[message_index];
    if (message.role != "assistant" || has_unreplayable_openai_reasoning(message.content_parts))
      continue;
    for (std::size_t part_index = 0; part_index < message.content_parts.size(); ++part_index)
    {
      auto const& part = message.content_parts[part_index];
      if (valid_openai_reasoning_item_part(part) || valid_openai_assistant_text_item_part(part))
        mask[message_index][part_index] = true;
    }
  }
  return mask;
}

std::string reasoning_input_item_json(ContentPart const& part)
{
  return part.reasoning_native_item_json;
}

std::string function_call_input_item_json(ContentPart const& part)
{
  std::string json = "{\"type\":\"function_call\"";
  if (!part.provider_item_id.empty() && is_valid_openai_opaque_id(part.provider_item_id))
    json += ",\"id\":\"" + ava::core::json::escape(part.provider_item_id) + "\"";
  json += ",\"call_id\":\"" + ava::core::json::escape(part.tool_call_id) + "\",\"name\":\"" + ava::core::json::escape(part.tool_name) + "\",\"arguments\":\"" +
          ava::core::json::escape(part.input_json) + "\"}";
  return json;
}

std::string function_call_output_input_item_json(ContentPart const& part)
{
  return "{\"type\":\"function_call_output\",\"call_id\":\"" + ava::core::json::escape(part.tool_call_id) + "\",\"output\":\"" +
         ava::core::json::escape(part.text) + "\"}";
}

ava::core::VoidResult validate_tools_json(ProviderRequest const& request)
{
  for (std::size_t index = 0; index < request.tools_json.size(); ++index)
  {
    if (is_json_object_shape(request.tools_json[index]))
      continue;
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "OpenAI tool JSON must be an object");
    error.with_context("tool_index", std::to_string(index));
    return std::unexpected(std::move(error));
  }
  return {};
}

bool valid_openai_reasoning_effort(std::string_view effort)
{
  return effort == "none" || effort == "minimal" || effort == "low" || effort == "medium" || effort == "high" || effort == "xhigh" || effort == "max";
}

ava::core::VoidResult validate_reasoning_options(ProviderRequest const& request)
{
  if (!request.reasoning)
    return {};
  if (!valid_openai_reasoning_effort(request.reasoning->type))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "OpenAI reasoning effort is not supported by this model");
    error.with_context("effort", request.reasoning->type);
    return std::unexpected(std::move(error));
  }
  if (request.reasoning->budget_tokens || !request.reasoning->display.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "OpenAI Responses reasoning supports effort only"));
  }
  return {};
}

std::string reasoning_options_json(ProviderRequest const& request)
{
  if (!request.reasoning)
    return {};
  std::string json = ",\"reasoning\":{\"effort\":\"" + ava::core::json::escape(request.reasoning->type) + "\"";
  if (request.reasoning->type != "none")
  {
    json += ",\"summary\":\"auto\"";
  }
  json += '}';
  return json;
}

std::string request_body_json(ProviderRequest const& request, bool include_max_output_tokens)
{
  std::string body = "{\"model\":\"" + ava::core::json::escape(request.model_id) + "\",\"instructions\":\"" + ava::core::json::escape(request.system_prompt) +
                     "\",\"stream\":" + (request.stream ? "true" : "false") + ",\"input\":[";
  auto const native_replay_parts = native_replay_part_mask(request.messages);
  bool first_input_item = true;
  auto append_input_item = [&body, &first_input_item](std::string item) {
    if (!first_input_item)
      body += ',';
    first_input_item = false;
    body += std::move(item);
  };
  for (std::size_t message_index = 0; message_index < request.messages.size(); ++message_index)
  {
    auto const& message = request.messages[message_index];
    bool has_native_replay_part = false;
    for (bool const native : native_replay_parts[message_index]) has_native_replay_part = has_native_replay_part || native;
    if (!has_native_replay_part)
    {
      append_input_item(input_item_json(message));
      continue;
    }

    std::vector<ContentPart const*> ordinary_parts;
    ordinary_parts.reserve(message.content_parts.size());
    auto append_ordinary_parts = [&]() {
      if (ordinary_parts.empty())
        return;
      if (auto item = native_content_item_json(message, ordinary_parts))
        append_input_item(std::move(*item));
      ordinary_parts.clear();
    };
    for (std::size_t part_index = 0; part_index < message.content_parts.size(); ++part_index)
    {
      auto const& part = message.content_parts[part_index];
      if (part.type == ContentPartType::Reasoning && valid_openai_reasoning_item_part(part) && native_replay_parts[message_index][part_index])
      {
        append_ordinary_parts();
        append_input_item(reasoning_input_item_json(part));
        continue;
      }
      if (part.type == ContentPartType::Text && valid_openai_assistant_text_item_part(part) && native_replay_parts[message_index][part_index])
      {
        append_ordinary_parts();
        append_input_item(assistant_message_output_item_json(part));
        continue;
      }
      if (!native_replay_parts[message_index][part_index])
      {
        ordinary_parts.push_back(&part);
        continue;
      }
      append_ordinary_parts();
      if (part.type == ContentPartType::ToolUse)
        append_input_item(function_call_input_item_json(part));
      else
        append_input_item(function_call_output_input_item_json(part));
    }
    append_ordinary_parts();
  }
  body += ']';
  if (include_max_output_tokens && request.max_output_tokens && *request.max_output_tokens > 0)
  {
    body += ",\"max_output_tokens\":";
    body += std::to_string(*request.max_output_tokens);
  }
  body += reasoning_options_json(request);
  body += ",\"tools\":[";
  for (std::size_t index = 0; index < request.tools_json.size(); ++index)
  {
    if (index > 0)
      body += ',';
    body += request.tools_json[index];
  }
  body += "]}";
  return body;
}

void apply_codex_oauth_request_options(ava::http::HttpRequest& request)
{
  request.url = std::string(kCodexResponsesUrl);
  request.headers["OpenAI-Beta"] = "responses=experimental";
  request.headers["originator"] = "ava";
  if (!request.body.empty() && request.body.back() == '}')
  {
    request.body.insert(request.body.size() - 1, ",\"store\":false");
  }
}

}  // namespace

ava::core::Result<ava::http::HttpRequest> build_openai_responses_request(ProviderRequest const& request, std::string_view access_token,
                                                                         OpenAIProviderOptions const& options, bool include_max_output_tokens)
{
  if (request.model_id.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "model id is required"));
  }
  if (options.require_credential && access_token.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "OpenAI credential is required"));
  }
  if (auto valid_images = validate_outbound_image_payloads(request, "OpenAI"); !valid_images)
  {
    return std::unexpected(std::move(valid_images.error()));
  }
  if (auto valid_tools = validate_tools_json(request); !valid_tools)
  {
    return std::unexpected(std::move(valid_tools.error()));
  }
  if (auto valid_reasoning = validate_reasoning_options(request); !valid_reasoning)
  {
    return std::unexpected(std::move(valid_reasoning.error()));
  }

  std::map<std::string, std::string> headers{{"Content-Type", "application/json"}, {"Accept", "text/event-stream"}};
  if (options.send_authorization_bearer && !access_token.empty())
    headers["Authorization"] = "Bearer " + std::string(access_token);
  auto const url = !options.endpoint.empty() ? options.endpoint : (options.base_url + "/v1/responses");
  return ava::http::HttpRequest{
      .method = "POST",
      .url = url,
      .headers = std::move(headers),
      .body = request_body_json(request, include_max_output_tokens),
      .timeout_ms = 60000,
      .follow_redirects = options.follow_redirects,
      .include_response_headers = false,
      .resolve_hosts = {},
  };
}

ava::core::VoidResult apply_openai_auth_options(ava::http::HttpRequest& request, ProviderAuthContext const& auth)
{
  if (auth.credential_type != "oauth")
    return {};
  apply_codex_oauth_request_options(request);
  if (!auth.account_id.empty())
  {
    request.headers["ChatGPT-Account-Id"] = auth.account_id;
    request.headers["chatgpt-account-id"] = auth.account_id;
  }
  return {};
}

}  // namespace detail

ava::core::Result<ava::http::HttpRequest> OpenAIProvider::build_request(ProviderRequest const& request, std::string_view access_token) const
{
  bool const include_max = true;
  return detail::build_openai_responses_request(request, access_token, options_, include_max);
}

ava::core::Result<ava::http::HttpRequest> OpenAIProvider::build_request(ProviderRequest const& request, ProviderAuthContext const& auth) const
{
  bool const include_max = options_.force_include_max_output_tokens || auth.credential_type != "oauth";
  auto http_request = detail::build_openai_responses_request(request, auth.access_token, options_, include_max);
  if (!http_request)
    return http_request;
  if (auto applied = apply_auth_options(*http_request, auth); !applied)
  {
    return std::unexpected(std::move(applied.error()));
  }
  return http_request;
}

ava::core::VoidResult OpenAIProvider::apply_auth_options(ava::http::HttpRequest& request, ProviderAuthContext const& auth) const
{
  if (!options_.enable_codex_oauth_mutations)
    return {};
  return detail::apply_openai_auth_options(request, auth);
}

ava::core::Result<ava::http::HttpRequest> OpenAIProvider::build_request(ProviderRequest const& request, ava::config::OpenAICredential const& credential,
                                                                        long long now_seconds) const
{
  auto access_token = ava::config::openai_access_token_for_request(credential, now_seconds);
  if (!access_token)
    return std::unexpected(std::move(access_token.error()));
  return build_request(request, ProviderAuthContext{.access_token = *access_token,
                                                    .credential_type = credential.type == ava::config::OpenAICredentialType::OAuth ? "oauth" : "api_key",
                                                    .account_id = credential.account_id});
}

ava::core::Result<ava::http::HttpRequest> OpenAIProvider::build_request(ProviderRequest const& request, ava::config::OpenAICredential const& credential) const
{
  auto access_token = ava::config::openai_access_token_for_request(credential);
  if (!access_token)
    return std::unexpected(std::move(access_token.error()));
  return build_request(request, ProviderAuthContext{.access_token = *access_token,
                                                    .credential_type = credential.type == ava::config::OpenAICredentialType::OAuth ? "oauth" : "api_key",
                                                    .account_id = credential.account_id});
}

}  // namespace ava::provider
