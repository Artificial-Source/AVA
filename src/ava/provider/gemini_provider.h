#pragma once
#include "ava/debug/print_members_on.h"
#include "ava/http/transport.h"
#include "ava/provider/parser_budget.h"
#include "ava/provider/provider.h"

#include <memory>
#include <optional>
#include <string>

namespace ava::provider {

namespace detail {

struct GeminiParseState
{
  std::optional<TokenUsage> usage = std::nullopt;
  std::optional<ProviderFinishReason> finish_reason = std::nullopt;
  std::string fallback_tool_call_prefix;
  // Sequence for fallback IDs across SSE records; retained on parser reuse
  // because the parser's unique prefix is retained too, preventing ID reuse.
  std::size_t next_tool_call_index = 0;
  // Bounded native parts use the existing private reasoning replay channel.
  std::string native_parts_json = {};
  std::size_t native_part_count = 0;
  bool native_parts_overflowed = false;
  bool has_thought_signature = false;
  bool reasoning_active = false;
  bool parsed_content = false;
  bool error_seen = false;

  void reset_response()
  {
    // Keep the fallback prefix and sequence together so parser reuse cannot
    // regenerate a tool-call ID already used by an earlier response.
    native_parts_json.clear();
    native_part_count = 0;
    native_parts_overflowed = false;
    has_thought_signature = false;
    reasoning_active = false;
    usage = std::nullopt;
    finish_reason = std::nullopt;
    parsed_content = false;
    error_seen = false;
  }

  // Opaque thought signatures in native_parts_json must never reach debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace detail

class GeminiStreamParser final : public StreamParser
{
 public:
  GeminiStreamParser();
  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> append(std::string_view chunk) override;
  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> finish() override;

 private:
  ProviderSseBuffer sse_;
  detail::GeminiParseState state_;
  bool saw_data_ = false;
  bool done_seen_ = false;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class GeminiProvider final : public Provider
{
 public:
  using Provider::build_request;

  explicit GeminiProvider(std::string base_url = "");
  [[nodiscard]] ava::core::Result<ava::http::HttpRequest> build_request(ProviderRequest const& request, std::string_view access_token) const override;
  [[nodiscard]] std::unique_ptr<StreamParser> create_stream_parser() const override;
  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> parse_response(ava::http::HttpResponse const& response, bool stream) const override;

 private:
  std::string base_url_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ava::core::Result<std::vector<StreamEvent>> parse_gemini_sse(std::string_view sse);
[[nodiscard]] ava::core::Result<std::vector<StreamEvent>> parse_gemini_sse_response(ava::http::HttpResponse const& response);
[[nodiscard]] ava::core::Result<std::vector<StreamEvent>> parse_gemini_response(ava::http::HttpResponse const& response);
[[nodiscard]] std::optional<TokenUsage> parse_gemini_usage(std::string_view body);

}  // namespace ava::provider
