#include "sys.h"
#include "ava/agent/provider_output_validation.h"

#include <string>
#include <string_view>
#include <utility>

namespace ava::agent {
namespace {

constexpr std::size_t kMaxSummaryValueBytes = 80;

std::string safe_summary_text(std::string text, std::size_t max_bytes = kMaxSummaryValueBytes)
{
  for (char& ch : text)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
      ch = '?';
  }
  if (text.size() <= max_bytes)
    return text;
  constexpr std::string_view marker = "...";
  if (max_bytes <= marker.size())
  {
    text.resize(max_bytes);
    return text;
  }
  text.resize(max_bytes - marker.size());
  text += marker;
  return text;
}

bool has_control_byte(std::string_view value)
{
  for (char const ch : value)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
      return true;
  }
  return false;
}

}  // namespace

ava::core::Error output_limit_error(std::string message, std::string_view limit_name, std::size_t limit)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Provider, std::move(message));
  error.with_context(std::string(limit_name), std::to_string(limit));
  return error;
}

ava::core::Error provider_event_limit_error(std::size_t limit)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "provider output event limit exceeded", ava::core::ErrorCode::ProviderEventLimit);
  error.with_context("max_provider_events", std::to_string(limit));
  return error;
}

bool would_exceed(std::size_t current, std::size_t added, std::size_t limit)
{
  return limit > 0 && (current > limit || added > limit - current);
}

ava::core::VoidResult validate_provider_tool_call_id(std::string_view id)
{
  constexpr std::size_t kMaxProviderToolCallIdBytes = 256;
  if (id.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "provider tool call id is empty"));
  }
  if (id.size() > kMaxProviderToolCallIdBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "provider tool call id is too long");
    error.with_context("max_bytes", std::to_string(kMaxProviderToolCallIdBytes));
    return std::unexpected(std::move(error));
  }
  if (!has_control_byte(id))
    return {};
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "provider tool call id contains a forbidden control byte");
  error.with_context("call_id", safe_summary_text(std::string(id)));
  return std::unexpected(std::move(error));
}

}  // namespace ava::agent
