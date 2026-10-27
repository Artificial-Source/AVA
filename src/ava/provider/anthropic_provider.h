#pragma once

#include "ava/http/transport.h"
#include "ava/provider/provider.h"
#include "ava/core/NoLogString.h"

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace ava::provider {

struct AnthropicProviderOptions
{
  std::string base_url = {};
  // When non-empty, used as the exact request URL (no /v1/messages join).
  std::string endpoint = {};
  bool require_credential = true;
  bool send_api_key_header = true;
  // Built-in Anthropic only: swap x-api-key for Bearer + OAuth beta header.
  bool enable_oauth_header_swap = true;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class AnthropicStreamParser final : public StreamParser
{
 public:
  struct ToolBlock
  {
    std::string id;
    std::string name;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };
  struct ReasoningBlock
  {
    ava::core::NoLogString signature;
    ava::core::NoLogString redacted_data;
    bool redacted = false;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> append(std::string_view chunk) override;
  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> finish() override;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  ava::core::NoLogString pending_line_;
  ava::core::NoLogString data_;
  std::size_t scan_offset_ = 0;
  std::map<long long, ToolBlock> tool_blocks_;
  std::map<long long, ReasoningBlock> reasoning_blocks_;
  std::optional<TokenUsage> usage_;
  std::optional<ProviderFinishReason> finish_reason_;
  bool saw_data_ = false;
  bool message_stop_seen_ = false;
  bool error_seen_ = false;
};

class AnthropicProvider final : public Provider
{
 public:
  using Provider::build_request;

  explicit AnthropicProvider(AnthropicProviderOptions options = {});
  explicit AnthropicProvider(std::string base_url);
  [[nodiscard]] ava::core::Result<ava::http::HttpRequest> build_request(ProviderRequest const& request, std::string_view access_token) const override;
  [[nodiscard]] ava::core::VoidResult apply_auth_options(ava::http::HttpRequest& request, ProviderAuthContext const& auth) const override;
  [[nodiscard]] std::unique_ptr<StreamParser> create_stream_parser() const override;
  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> parse_response(ava::http::HttpResponse const& response, bool stream) const override;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  AnthropicProviderOptions options_;
};

[[nodiscard]] ava::core::Result<std::vector<StreamEvent>> parse_anthropic_sse(std::string_view sse);
[[nodiscard]] ava::core::Result<std::vector<StreamEvent>> parse_anthropic_sse_response(ava::http::HttpResponse const& response);
[[nodiscard]] ava::core::Result<std::vector<StreamEvent>> parse_anthropic_response(ava::http::HttpResponse const& response);
[[nodiscard]] std::optional<TokenUsage> parse_anthropic_usage(std::string_view body);

}  // namespace ava::provider
