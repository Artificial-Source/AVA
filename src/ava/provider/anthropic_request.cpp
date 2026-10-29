#include "sys.h"
#include "ava/http/transport.h"
#include "ava/provider/anthropic_provider.h"
#include "ava/provider/anthropic_request.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/environment_value.h"
#include "ava/core/json.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::provider {
namespace {

constexpr std::string_view kDefaultAnthropicBaseUrl = "https://api.anthropic.com";
constexpr std::string_view kAnthropicVersion = "2023-06-01";
constexpr int kDefaultMaxTokens = 4096;

struct PendingToolUse
{
  std::string id;
  std::size_t message_index = 0;
  std::size_t part_index = 0;
};

std::string configured_base_url()
{
  if (auto value = ava::core::nonempty_environment_value("ANTHROPIC_BASE_URL"))
    return std::move(*value);
  return std::string(kDefaultAnthropicBaseUrl);
}

std::string trim_trailing_slashes(std::string value)
{
  while (value.size() > 1 && value.back() == '/') value.pop_back();
  return value;
}

std::string message_role(ChatMessage const& message)
{
  return message.role == "assistant" ? "assistant" : "user";
}

std::vector<ContentPart> message_content_parts(ChatMessage const& message)
{
  if (!message.content_parts.empty())
    return message.content_parts;
  if (message.content.empty())
    return {};
  return {ContentPart{.type = ContentPartType::Text, .text = message.content, .tool_call_id = "", .tool_name = "", .input_json = "", .is_error = false}};
}

ContentPart text_separator_part()
{
  return ContentPart{.type = ContentPartType::Text, .text = "\n\n", .tool_call_id = "", .tool_name = "", .input_json = "", .is_error = false};
}

void append_content_parts(std::vector<ContentPart>& target, std::vector<ContentPart> const& source, bool separate_text)
{
  for (auto const& part : source)
  {
    if (part.type == ContentPartType::Text && part.text.empty())
      continue;
    if (separate_text && part.type == ContentPartType::Text && !target.empty() && target.back().type == ContentPartType::Text)
    {
      if (!target.back().cache_control_ttl.empty() || !part.cache_control_ttl.empty())
      {
        target.push_back(text_separator_part());
        target.push_back(part);
      }
      else
      {
        target.back().text += "\n\n";
        target.back().text += part.text;
      }
      separate_text = false;
      continue;
    }
    target.push_back(part);
    separate_text = false;
  }
}

ava::core::Error invalid_content_part_error(std::string message, std::size_t message_index, std::size_t part_index)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  error.with_context("provider", "anthropic");
  error.with_context("message_index", std::to_string(message_index));
  error.with_context("content_part_index", std::to_string(part_index));
  return error;
}

bool contains_tool_use_id(std::vector<std::string> const& ids, std::string_view id)
{
  for (auto const& seen : ids)
  {
    if (seen == id)
      return true;
  }
  return false;
}

bool valid_cache_control_ttl(std::string_view ttl)
{
  return ttl == "5m" || ttl == "1h";
}

std::string cache_control_json(std::string_view ttl)
{
  return ",\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"" + std::string(ttl) + "\"}";
}

std::string cache_control_suffix(std::string_view ttl)
{
  return ttl.empty() ? std::string{} : cache_control_json(ttl);
}

bool valid_reasoning_type(std::string_view type)
{
  return type == "enabled" || type == "adaptive";
}

bool valid_reasoning_display(std::string_view display)
{
  return display == "summarized" || display == "omitted";
}

ava::core::VoidResult validate_anthropic_content_parts(std::vector<ChatMessage> const& messages)
{
  std::vector<PendingToolUse> pending_tool_uses;
  std::vector<std::string> seen_tool_use_ids;
  for (std::size_t message_index = 0; message_index < messages.size(); ++message_index)
  {
    auto const& message = messages[message_index];
    auto const role = message_role(message);
    if (role == "assistant" && !pending_tool_uses.empty())
    {
      auto const& pending = pending_tool_uses.front();
      return std::unexpected(invalid_content_part_error("Anthropic tool_use content requires matching tool_result before the next assistant message",
                                                        pending.message_index, pending.part_index));
    }
    for (std::size_t part_index = 0; part_index < message.content_parts.size(); ++part_index)
    {
      auto const& part = message.content_parts[part_index];
      if (!part.cache_control_ttl.empty() && !valid_cache_control_ttl(part.cache_control_ttl))
      {
        return std::unexpected(invalid_content_part_error("Anthropic cache_control ttl must be 5m or 1h", message_index, part_index));
      }
      if (part.type == ContentPartType::Text && part.text.empty())
      {
        if (!part.cache_control_ttl.empty())
        {
          return std::unexpected(invalid_content_part_error("Anthropic cache_control requires non-empty text content", message_index, part_index));
        }
        continue;
      }
      if (role == "user" && !pending_tool_uses.empty() && part.type != ContentPartType::ToolResult)
      {
        return std::unexpected(
            invalid_content_part_error("Anthropic tool_result content must precede ordinary user content after tool_use", message_index, part_index));
      }
      switch (part.type)
      {
        case ContentPartType::Text:
          break;
        case ContentPartType::Image:
          if (role != "user")
          {
            return std::unexpected(invalid_content_part_error("Anthropic image content requires user role", message_index, part_index));
          }
          if (!is_supported_image_mime_type(part.mime_type))
          {
            return std::unexpected(invalid_content_part_error("Anthropic image MIME type is not supported", message_index, part_index));
          }
          if (part.byte_size == 0 || part.byte_size > image_input_policy_for_api_family("anthropic_messages").max_bytes_per_image)
          {
            return std::unexpected(invalid_content_part_error("Anthropic image byte size is outside supported limits", message_index, part_index));
          }
          if (part.data_base64.empty() || !is_valid_base64(part.data_base64))
          {
            return std::unexpected(invalid_content_part_error("Anthropic image content requires verified attachment bytes", message_index, part_index));
          }
          if (!part.cache_control_ttl.empty())
          {
            return std::unexpected(invalid_content_part_error("Anthropic cache_control is not supported on image content", message_index, part_index));
          }
          break;
        case ContentPartType::Reasoning:
          if (role != "assistant")
          {
            return std::unexpected(invalid_content_part_error("Anthropic reasoning content requires assistant role", message_index, part_index));
          }
          if (!part.cache_control_ttl.empty())
          {
            return std::unexpected(invalid_content_part_error("Anthropic cache_control is not supported on reasoning content", message_index, part_index));
          }
          if (!part.reasoning_format.empty() && part.reasoning_format != "anthropic_thinking")
          {
            return std::unexpected(invalid_content_part_error("Anthropic reasoning content requires anthropic_thinking format", message_index, part_index));
          }
          if (part.redacted && part.reasoning_redacted_data.empty())
          {
            return std::unexpected(
                invalid_content_part_error("Anthropic redacted reasoning content requires provider redacted data", message_index, part_index));
          }
          break;
        case ContentPartType::ToolUse:
          if (role != "assistant")
          {
            return std::unexpected(invalid_content_part_error("Anthropic tool_use content requires assistant role", message_index, part_index));
          }
          if (part.tool_call_id.empty() || part.tool_name.empty())
          {
            return std::unexpected(invalid_content_part_error("Anthropic tool_use content requires id and name", message_index, part_index));
          }
          if (!is_valid_json_object(part.input_json))
          {
            return std::unexpected(invalid_content_part_error("Anthropic tool_use input must be a valid JSON object", message_index, part_index));
          }
          if (contains_tool_use_id(seen_tool_use_ids, part.tool_call_id))
          {
            return std::unexpected(invalid_content_part_error("Anthropic tool_use id must be unique", message_index, part_index));
          }
          pending_tool_uses.push_back(PendingToolUse{.id = part.tool_call_id, .message_index = message_index, .part_index = part_index});
          seen_tool_use_ids.push_back(part.tool_call_id);
          break;
        case ContentPartType::ToolResult:
          if (role != "user")
          {
            return std::unexpected(invalid_content_part_error("Anthropic tool_result content requires user role", message_index, part_index));
          }
          if (part.tool_call_id.empty())
          {
            return std::unexpected(invalid_content_part_error("Anthropic tool_result content requires tool_use_id", message_index, part_index));
          }
          if (pending_tool_uses.empty() || pending_tool_uses.front().id != part.tool_call_id)
          {
            return std::unexpected(
                invalid_content_part_error("Anthropic tool_result content requires a preceding unmatched tool_use", message_index, part_index));
          }
          pending_tool_uses.erase(pending_tool_uses.begin());
          break;
      }
    }
    if (role == "user" && !pending_tool_uses.empty())
    {
      auto const& pending = pending_tool_uses.front();
      return std::unexpected(invalid_content_part_error("Anthropic tool_use content requires matching tool_result in the next user message",
                                                        pending.message_index, pending.part_index));
    }
  }
  if (!pending_tool_uses.empty())
  {
    auto const& pending = pending_tool_uses.front();
    return std::unexpected(invalid_content_part_error("Anthropic tool_use content requires matching tool_result", pending.message_index, pending.part_index));
  }
  return {};
}

std::string anthropic_content_part_json(ContentPart const& part)
{
  switch (part.type)
  {
    case ContentPartType::Text:
      return "{\"type\":\"text\",\"text\":\"" + ava::core::json::escape(part.text) + "\"" + cache_control_suffix(part.cache_control_ttl) + "}";
    case ContentPartType::Image:
      return "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"" + ava::core::json::escape(part.mime_type) + "\",\"data\":\"" +
             ava::core::json::escape(part.data_base64) + "\"}}";
    case ContentPartType::Reasoning: {
      if (part.redacted)
      {
        return "{\"type\":\"redacted_thinking\",\"data\":\"" + ava::core::json::escape(part.reasoning_redacted_data) + "\"}";
      }
      std::string json = "{\"type\":\"thinking\",\"thinking\":\"" + ava::core::json::escape(part.text) + "\"";
      if (!part.reasoning_signature.empty())
      {
        json += ",\"signature\":\"" + ava::core::json::escape(part.reasoning_signature) + "\"";
      }
      json += '}';
      return json;
    }
    case ContentPartType::ToolUse: {
      auto const input = is_valid_json_object(part.input_json) ? part.input_json : std::string("{}");
      return "{\"type\":\"tool_use\",\"id\":\"" + ava::core::json::escape(part.tool_call_id) + "\",\"name\":\"" + ava::core::json::escape(part.tool_name) +
             "\",\"input\":" + input + cache_control_suffix(part.cache_control_ttl) + "}";
    }
    case ContentPartType::ToolResult: {
      std::string json = "{\"type\":\"tool_result\",\"tool_use_id\":\"" + ava::core::json::escape(part.tool_call_id) + "\",\"content\":\"" +
                         ava::core::json::escape(part.text) + "\"";
      if (part.is_error)
        json += ",\"is_error\":true";
      json += cache_control_suffix(part.cache_control_ttl);
      json += '}';
      return json;
    }
  }
  return "{\"type\":\"text\",\"text\":\"\"}";
}

std::string message_json(ChatMessage const& message)
{
  std::string const role = message_role(message);
  if (message.content_parts.empty())
  {
    return "{\"role\":\"" + role + "\",\"content\":\"" + ava::core::json::escape(message.content) + "\"}";
  }
  std::string json = "{\"role\":\"" + role + "\",\"content\":[";
  bool first = true;
  for (auto const& part : message.content_parts)
  {
    if (part.type == ContentPartType::Text && part.text.empty())
      continue;
    if (!first)
      json += ',';
    first = false;
    json += anthropic_content_part_json(part);
  }
  if (first)
    json += "{\"type\":\"text\",\"text\":\"\"}";
  json += "]}";
  return json;
}

std::vector<ChatMessage> collapse_consecutive_roles(std::vector<ChatMessage> const& messages)
{
  std::vector<ChatMessage> collapsed;
  for (auto const& message : messages)
  {
    std::string const role = message_role(message);
    if (!collapsed.empty() && collapsed.back().role == role)
    {
      if (collapsed.back().content_parts.empty() && message.content_parts.empty())
      {
        collapsed.back().content += "\n\n";
        collapsed.back().content += message.content;
        continue;
      }
      if (collapsed.back().content_parts.empty())
      {
        collapsed.back().content_parts = message_content_parts(collapsed.back());
      }
      append_content_parts(collapsed.back().content_parts, message_content_parts(message), true);
      if (!message.content.empty())
      {
        collapsed.back().content += "\n\n";
        collapsed.back().content += message.content;
      }
      continue;
    }
    collapsed.push_back(ChatMessage{.role = role, .content = message.content, .content_parts = message.content_parts});
  }
  return collapsed;
}

long long max_tokens_for_request(ProviderRequest const& request)
{
  long long const value = request.max_output_tokens.value_or(kDefaultMaxTokens);
  return value > 0 ? value : kDefaultMaxTokens;
}

ava::core::VoidResult validate_anthropic_request_options(ProviderRequest const& request)
{
  if (!request.system_prompt_cache_ttl.empty() && !valid_cache_control_ttl(request.system_prompt_cache_ttl))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic system cache ttl must be 5m or 1h"));
  }
  if (!request.system_prompt_cache_ttl.empty() && request.system_prompt.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic system cache ttl requires a non-empty system prompt"));
  }
  if (!request.reasoning)
    return {};
  auto const& reasoning = *request.reasoning;
  if (!valid_reasoning_type(reasoning.type))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic reasoning type must be enabled or adaptive"));
  }
  if (!reasoning.display.empty() && !valid_reasoning_display(reasoning.display))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic reasoning display must be summarized or omitted"));
  }
  if (reasoning.budget_tokens && *reasoning.budget_tokens <= 0)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic reasoning budget must be positive"));
  }
  if (reasoning.type == "enabled" && !reasoning.budget_tokens)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic enabled reasoning requires a budget"));
  }
  if (reasoning.type == "enabled" && reasoning.budget_tokens && *reasoning.budget_tokens < 1024)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic reasoning budget must be at least 1024 tokens"));
  }
  if (reasoning.type == "adaptive" && reasoning.budget_tokens)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic adaptive reasoning does not accept a fixed budget"));
  }
  if (reasoning.type == "adaptive" && request.model_id == "claude-sonnet-4-5")
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic adaptive reasoning is not supported by this model"));
  }
  if (reasoning.budget_tokens && *reasoning.budget_tokens >= max_tokens_for_request(request))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic reasoning budget must be below max output tokens"));
  }
  return {};
}

ava::core::VoidResult validate_cache_control_order(ProviderRequest const& request, std::vector<ChatMessage> const& messages)
{
  bool saw_short_ttl = false;
  std::size_t breakpoints = 0;
  auto observe = [&](std::string_view ttl) -> ava::core::VoidResult {
    if (ttl.empty())
      return {};
    ++breakpoints;
    if (breakpoints > 4)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic supports at most four cache breakpoints"));
    }
    if (saw_short_ttl && ttl == "1h")
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic 1h cache ttl cannot follow a 5m cache ttl"));
    }
    if (ttl == "5m")
      saw_short_ttl = true;
    return {};
  };

  if (auto observed = observe(request.system_prompt_cache_ttl); !observed)
    return observed;
  for (auto const& message : messages)
  {
    for (auto const& part : message_content_parts(message))
    {
      if (auto observed = observe(part.cache_control_ttl); !observed)
        return observed;
    }
  }
  return {};
}

std::string system_prompt_json(ProviderRequest const& request)
{
  if (request.system_prompt.empty() && request.system_prompt_cache_ttl.empty())
    return {};
  if (request.system_prompt_cache_ttl.empty())
  {
    return "\"system\":\"" + ava::core::json::escape(request.system_prompt) + "\"";
  }
  return "\"system\":[{\"type\":\"text\",\"text\":\"" + ava::core::json::escape(request.system_prompt) + "\"" +
         cache_control_json(request.system_prompt_cache_ttl) + "}]";
}

std::string reasoning_options_json(ProviderReasoningOptions const& reasoning)
{
  std::string json = ",\"thinking\":{\"type\":\"" + ava::core::json::escape(reasoning.type) + "\"";
  if (reasoning.budget_tokens)
    json += ",\"budget_tokens\":" + std::to_string(*reasoning.budget_tokens);
  if (!reasoning.display.empty())
    json += ",\"display\":\"" + ava::core::json::escape(reasoning.display) + "\"";
  json += '}';
  return json;
}

std::string anthropic_tool_json(std::string_view tool_json)
{
  auto const name = ava::core::json::string_field(tool_json, "name").value_or("");
  auto const description = ava::core::json::string_field(tool_json, "description").value_or("");
  auto const parameters = ava::core::json::object_field(tool_json, "parameters").value_or("{\"type\":\"object\"}");
  return "{\"name\":\"" + ava::core::json::escape(name) + "\",\"description\":\"" + ava::core::json::escape(description) + "\",\"input_schema\":" + parameters +
         "}";
}

std::string request_body_json(ProviderRequest const& request, std::vector<ChatMessage> const& messages)
{
  std::string body = "{\"model\":\"" + ava::core::json::escape(request.model_id) + "\",\"max_tokens\":" + std::to_string(max_tokens_for_request(request)) +
                     ",\"stream\":" + (request.stream ? "true" : "false");
  if (auto const system = system_prompt_json(request); !system.empty())
    body += "," + system;
  body += ",\"messages\":[";
  for (std::size_t index = 0; index < messages.size(); ++index)
  {
    if (index > 0)
      body += ',';
    body += message_json(messages[index]);
  }
  body += ']';
  if (!request.tools_json.empty())
  {
    body += ",\"tools\":[";
    for (std::size_t index = 0; index < request.tools_json.size(); ++index)
    {
      if (index > 0)
        body += ',';
      body += anthropic_tool_json(request.tools_json[index]);
    }
    body += ']';
  }
  if (request.reasoning)
    body += reasoning_options_json(*request.reasoning);
  body += '}';
  return body;
}

}  // namespace

std::string normalize_anthropic_base_url(std::string base_url)
{
  return trim_trailing_slashes(base_url.empty() ? configured_base_url() : std::move(base_url));
}

ava::core::Result<ava::http::HttpRequest> build_anthropic_http_request(AnthropicProviderOptions const& options, ProviderRequest const& request,
                                                                       std::string_view access_token)
{
  if (request.model_id.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "model id is required"));
  }
  if (options.require_credential && access_token.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Anthropic API key is required"));
  }
  auto const messages = collapse_consecutive_roles(request.messages);
  if (auto valid = validate_anthropic_request_options(request); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto valid = validate_anthropic_content_parts(messages); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto valid = validate_cache_control_order(request, messages); !valid)
    return std::unexpected(std::move(valid.error()));
  std::map<std::string, std::string> headers{{"anthropic-version", std::string(kAnthropicVersion)},
                                             {"Content-Type", "application/json"},
                                             {"Accept", request.stream ? "text/event-stream" : "application/json"}};
  if (options.send_api_key_header && !access_token.empty())
    headers["x-api-key"] = std::string(access_token);
  auto const url = !options.endpoint.empty() ? options.endpoint : (options.base_url + "/v1/messages");
  return ava::http::HttpRequest{.method = "POST",
                                .url = url,
                                .headers = std::move(headers),
                                .body = request_body_json(request, messages),
                                .timeout_ms = 60000,
                                .follow_redirects = false,
                                .include_response_headers = false,
                                .resolve_hosts = {}};
}

}  // namespace ava::provider
