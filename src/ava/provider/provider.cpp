#include "sys.h"
#include "ava/http/transport.h"
#include "ava/provider/provider.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/NoLogString.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ava::provider {
namespace {

constexpr std::size_t kMaxImageAttachmentIdBytes = 128;
constexpr std::size_t kMaxImageStoragePathBytes = 4096;

class DefaultStreamParser final : public StreamParser
{
 public:
  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> append(std::string_view chunk) override
  {
    pending_.append(chunk);
    return std::vector<StreamEvent>{};
  }

  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> finish() override;

 private:
  ava::core::NoLogString pending_;
};

std::string lower_copy(std::string_view value)
{
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lowered;
}

bool has_any(std::string_view haystack, std::initializer_list<std::string_view> needles)
{
  for (auto const needle : needles)
  {
    if (haystack.find(needle) != std::string_view::npos)
      return true;
  }
  return false;
}

bool has_control_byte(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7f;
  });
}

bool is_hex_string(std::string_view value)
{
  return std::ranges::all_of(value, [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); });
}

ava::core::Error image_part_error(std::string message, std::size_t message_index, std::size_t part_index)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  error.with_context("message_index", std::to_string(message_index));
  error.with_context("content_part_index", std::to_string(part_index));
  return error;
}

bool looks_like_context_overflow(std::string_view text)
{
  auto const lowered = lower_copy(text);
  bool const mentions_context = has_any(lowered, {"context", "window", "token", "tokens", "prompt"});
  bool const mentions_overflow =
      has_any(lowered, {"too many", "too much", "exceed", "exceeded", "exceeds", "maximum", "max", "limit", "length", "larger than"});
  return mentions_context && mentions_overflow;
}

bool looks_like_quota(std::string_view text)
{
  return has_any(text, {"quota", "billing", "insufficient_quota", "credit balance", "insufficient credit", "insufficient credits", "payment required"});
}

bool looks_like_content_filter(std::string_view text)
{
  return has_any(text, {"content filter", "content_filter", "safety", "policy violation", "blocked"});
}

bool looks_like_refusal(std::string_view text)
{
  return has_any(text, {"refusal", "refused", "cannot comply", "can't comply"});
}

std::optional<std::string> default_text_from_json(std::string_view body)
{
  if (auto output = ava::core::json::string_field(body, "output_text"))
    return output;
  if (auto text = ava::core::json::string_field(body, "text"))
    return text;
  if (auto delta = ava::core::json::string_field(body, "delta"))
    return delta;
  return std::nullopt;
}

ava::core::Result<std::vector<StreamEvent>> default_parse_response(ava::http::HttpResponse const& response, bool stream)
{
  if (response.status_code < 200 || response.status_code >= 300)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "provider HTTP request failed with status " + std::to_string(response.status_code));
    error.with_context("status", std::to_string(response.status_code));
    error.with_context("provider_error_kind", to_string(classify_provider_error(response)));
    if (auto const retry_after = ava::http::retry_after_header(response))
      error.with_context("retry_after", *retry_after);
    return std::unexpected(std::move(error));
  }
  if (stream)
  {
    std::vector<StreamEvent> events;
    std::size_t line_start = 0;
    while (line_start <= response.body.size())
    {
      auto const newline = response.body.find('\n', line_start);
      auto line = newline == std::string::npos ? std::string_view(response.body).substr(line_start)
                                               : std::string_view(response.body).substr(line_start, newline - line_start);
      if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
      if (line.starts_with("data:"))
      {
        line.remove_prefix(5);
        if (!line.empty() && line.front() == ' ')
          line.remove_prefix(1);
        if (line == "[DONE]")
        {
          events.push_back(
              StreamEvent{.type = StreamEventType::Done, .text = "", .tool_call_id = "", .tool_name = "", .error_message = "", .usage = std::nullopt});
        }
        else if (auto text = default_text_from_json(line))
        {
          events.push_back(
              StreamEvent{.type = StreamEventType::TextDelta, .text = *text, .tool_call_id = "", .tool_name = "", .error_message = "", .usage = std::nullopt});
        }
      }
      if (newline == std::string::npos)
        break;
      line_start = newline + 1;
    }
    return events;
  }
  auto text = default_text_from_json(response.body);
  if (!text)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "provider response text is missing"));
  }
  return std::vector<StreamEvent>{
      StreamEvent{.type = StreamEventType::TextDelta, .text = *text, .tool_call_id = "", .tool_name = "", .error_message = "", .usage = std::nullopt},
      StreamEvent{.type = StreamEventType::Done, .text = "", .tool_call_id = "", .tool_name = "", .error_message = "", .usage = std::nullopt}};
}

ava::core::Result<std::vector<StreamEvent>> DefaultStreamParser::finish()
{
  return default_parse_response(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = std::move(pending_)}, true);
}

}  // namespace

ava::core::Result<ava::http::HttpRequest> Provider::build_request(ProviderRequest const& request, ProviderAuthContext const& auth) const
{
  auto http_request = build_request(request, auth.access_token);
  if (!http_request)
    return http_request;
  if (auto applied = apply_auth_options(*http_request, auth); !applied)
  {
    return std::unexpected(std::move(applied.error()));
  }
  return http_request;
}

ava::core::VoidResult Provider::apply_auth_options(ava::http::HttpRequest&, ProviderAuthContext const&) const
{
  return {};
}

std::unique_ptr<StreamParser> Provider::create_stream_parser() const
{
  return std::make_unique<DefaultStreamParser>();
}

ava::core::Result<std::vector<StreamEvent>> Provider::parse_response(ava::http::HttpResponse const& response, bool stream) const
{
  return default_parse_response(response, stream);
}

std::string to_string(AssistantPhase phase)
{
  switch (phase)
  {
    case AssistantPhase::Unknown:
      return "unknown";
    case AssistantPhase::Commentary:
      return "commentary";
    case AssistantPhase::FinalAnswer:
      return "final_answer";
  }
  return "unknown";
}

std::optional<AssistantPhase> assistant_phase_from_string(std::string_view value)
{
  if (value == "commentary")
    return AssistantPhase::Commentary;
  if (value == "final_answer")
    return AssistantPhase::FinalAnswer;
  return std::nullopt;
}

bool is_known_assistant_phase(AssistantPhase phase) noexcept
{
  return phase == AssistantPhase::Commentary || phase == AssistantPhase::FinalAnswer;
}

std::string to_string(StreamEventType type)
{
  switch (type)
  {
    case StreamEventType::TextStart:
      return "text_start";
    case StreamEventType::TextDelta:
      return "text_delta";
    case StreamEventType::TextEnd:
      return "text_end";
    case StreamEventType::ReasoningStart:
      return "reasoning_start";
    case StreamEventType::ReasoningDelta:
      return "reasoning_delta";
    case StreamEventType::ReasoningEnd:
      return "reasoning_end";
    case StreamEventType::ToolCallStart:
      return "tool_call_start";
    case StreamEventType::ToolCallDelta:
      return "tool_call_delta";
    case StreamEventType::ToolCallEnd:
      return "tool_call_end";
    case StreamEventType::Done:
      return "done";
    case StreamEventType::Error:
      return "error";
  }
  return "error";
}

std::string to_string(ProviderErrorKind kind)
{
  switch (kind)
  {
    case ProviderErrorKind::Authentication:
      return "authentication";
    case ProviderErrorKind::RateLimited:
      return "rate_limited";
    case ProviderErrorKind::Quota:
      return "quota";
    case ProviderErrorKind::InvalidRequest:
      return "invalid_request";
    case ProviderErrorKind::ContextOverflow:
      return "context_overflow";
    case ProviderErrorKind::Refusal:
      return "refusal";
    case ProviderErrorKind::ContentFilter:
      return "content_filter";
    case ProviderErrorKind::Transient:
      return "transient";
    case ProviderErrorKind::Unknown:
      return "unknown";
  }
  return "unknown";
}

ProviderErrorKind classify_provider_error(ava::http::HttpResponse const& response)
{
  if (response.status_code >= 200 && response.status_code < 300)
    return ProviderErrorKind::Unknown;
  auto const body = lower_copy(response.body);
  if (response.status_code == 401 || response.status_code == 403)
    return ProviderErrorKind::Authentication;
  if (response.status_code == 402)
    return ProviderErrorKind::Quota;
  if (looks_like_context_overflow(body))
    return ProviderErrorKind::ContextOverflow;
  if (looks_like_quota(body))
    return ProviderErrorKind::Quota;
  if (looks_like_content_filter(body))
    return ProviderErrorKind::ContentFilter;
  if (looks_like_refusal(body))
    return ProviderErrorKind::Refusal;
  if (response.status_code == 429)
    return ProviderErrorKind::RateLimited;
  if (response.status_code == 400 || response.status_code == 404 || response.status_code == 422)
  {
    return ProviderErrorKind::InvalidRequest;
  }
  if (response.status_code == 408 || response.status_code == 409 || (response.status_code >= 500 && response.status_code < 600))
  {
    return ProviderErrorKind::Transient;
  }
  return ProviderErrorKind::Unknown;
}

ava::http::ResponseRetryDecision provider_retry_decision(ava::http::HttpResponse const& response)
{
  switch (classify_provider_error(response))
  {
    case ProviderErrorKind::RateLimited:
      return ava::http::ResponseRetryDecision::RateLimited;
    case ProviderErrorKind::Transient:
      return ava::http::ResponseRetryDecision::Transient;
    case ProviderErrorKind::Authentication:
    case ProviderErrorKind::Quota:
    case ProviderErrorKind::InvalidRequest:
    case ProviderErrorKind::ContextOverflow:
    case ProviderErrorKind::Refusal:
    case ProviderErrorKind::ContentFilter:
    case ProviderErrorKind::Unknown:
      return ava::http::ResponseRetryDecision::NoRetry;
  }
  return ava::http::ResponseRetryDecision::NoRetry;
}

bool is_context_overflow_error(ava::core::Error const& error)
{
  if (error.category() != ava::core::ErrorCategory::Provider)
    return false;
  if (looks_like_context_overflow(error.message()))
    return true;
  for (auto const& item : error.context())
  {
    if ((item.key == "provider_error_kind" && item.value == "context_overflow") || looks_like_context_overflow(item.value))
      return true;
  }
  return false;
}

ImageInputPolicy image_input_policy_for_api_family(std::string_view api_family) noexcept
{
  auto policy = ImageInputPolicy{};
  if (api_family == "anthropic_messages")
    policy.max_bytes_per_image = 5 * 1024 * 1024;
  return policy;
}

bool is_supported_image_mime_type(std::string_view mime_type)
{
  return mime_type == "image/png" || mime_type == "image/jpeg" || mime_type == "image/webp" || mime_type == "image/gif";
}

bool request_has_image_parts(ProviderRequest const& request)
{
  for (auto const& message : request.messages)
  {
    for (auto const& part : message.content_parts)
    {
      if (part.type == ContentPartType::Image)
        return true;
    }
  }
  return false;
}

bool valid_image_storage_path(std::string_view path)
{
  if (path.empty() || path.size() > kMaxImageStoragePathBytes || has_control_byte(path))
    return false;
  if (!path.starts_with("attachments/"))
    return false;
  if (path.starts_with('/') || path.starts_with('~') || path.find('\\') != std::string_view::npos)
    return false;
  if (path.find(':') != std::string_view::npos)
    return false;
  std::size_t segment_start = 0;
  while (segment_start <= path.size())
  {
    auto const slash = path.find('/', segment_start);
    auto const segment = path.substr(segment_start, slash == std::string_view::npos ? std::string_view::npos : slash - segment_start);
    if (segment.empty() || segment == "." || segment == "..")
      return false;
    if (slash == std::string_view::npos)
      break;
    segment_start = slash + 1;
  }
  return true;
}

ava::core::VoidResult validate_image_content_parts(ProviderRequest const& request, bool model_supports_images)
{
  auto const policy = image_input_policy_for_api_family({});
  std::size_t image_count = 0;
  std::size_t total_image_bytes = 0;
  for (std::size_t message_index = 0; message_index < request.messages.size(); ++message_index)
  {
    auto const& message = request.messages[message_index];
    for (std::size_t part_index = 0; part_index < message.content_parts.size(); ++part_index)
    {
      auto const& part = message.content_parts[part_index];
      if (part.type != ContentPartType::Image)
        continue;
      if (!model_supports_images)
      {
        return std::unexpected(image_part_error("selected model does not support image input", message_index, part_index));
      }
      if (message.role != "user")
      {
        return std::unexpected(image_part_error("image content requires user role", message_index, part_index));
      }
      if (part.attachment_id.empty() || part.attachment_id.size() > kMaxImageAttachmentIdBytes || has_control_byte(part.attachment_id))
      {
        return std::unexpected(image_part_error("image attachment id is invalid", message_index, part_index));
      }
      if (!is_supported_image_mime_type(part.mime_type))
      {
        return std::unexpected(image_part_error("image attachment MIME type is not supported", message_index, part_index));
      }
      if (!valid_image_storage_path(part.storage_path))
      {
        return std::unexpected(image_part_error("image attachment storage path is invalid", message_index, part_index));
      }
      if (part.byte_size == 0 || part.byte_size > policy.max_bytes_per_image)
      {
        return std::unexpected(image_part_error("image attachment byte size is outside supported limits", message_index, part_index));
      }
      if (part.sha256.size() != 64 || !is_hex_string(part.sha256))
      {
        return std::unexpected(image_part_error("image attachment sha256 is invalid", message_index, part_index));
      }
      if (!part.data_base64.empty() && !is_valid_base64(part.data_base64))
      {
        return std::unexpected(image_part_error("image attachment base64 payload is invalid", message_index, part_index));
      }
      ++image_count;
      if (image_count > policy.max_attachments_per_request)
      {
        return std::unexpected(image_part_error("image attachment count exceeds supported limits", message_index, part_index));
      }
      if (part.byte_size > policy.max_bytes_per_request - total_image_bytes)
      {
        return std::unexpected(image_part_error("image attachment total byte size exceeds supported limits", message_index, part_index));
      }
      total_image_bytes += part.byte_size;
    }
  }
  return {};
}

}  // namespace ava::provider
