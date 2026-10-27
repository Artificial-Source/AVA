#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/http/transport.h"
#include "ava/provider/finish_reason.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "debug.h"

namespace ava::provider {

// Provider parsers enforce this boundary before constructing StreamEvent
// vectors. AgentLoop applies its configurable limit afterwards.
inline constexpr std::size_t kMaxProviderParserEvents = 4096;
inline constexpr std::size_t kMaxProviderParserArrayItems = 1024;
inline constexpr std::size_t kMaxProviderSseBufferedBytes = 256U * 1024U;

enum class ContentPartType
{
  Text,
  Image,
  Reasoning,
  ToolUse,
  ToolResult,
};

// The native Responses API distinguishes interim commentary from the final
// answer. Unknown is deliberately retained for legacy provider event families
// that do not expose an equivalent phase.
enum class AssistantPhase
{
  Unknown,
  Commentary,
  FinalAnswer,
};

struct ContentPart
{
  // Provider-neutral native content item. Providers that do not support native
  // parts can ignore these fields and serialize ChatMessage::content instead.
  ContentPartType type = ContentPartType::Text;
  ava::core::NoLogString text = {};
  std::string tool_call_id = {};
  std::string tool_name = {};
  ava::core::NoLogString input_json = {};
  bool is_error = false;
  std::string cache_control_ttl = {};
  // Provider-native reasoning/thinking metadata. `text` carries the visible or
  // summarized reasoning content; signatures stay provider-private for replay.
  std::string reasoning_format = {};
  ava::core::NoLogString reasoning_signature = {};
  ava::core::NoLogString reasoning_redacted_data = {};
  // Opaque provider-only JSON for exact reasoning-item replay. It must never
  // cross public stream/event boundaries or be included in exports.
  ava::core::NoLogString reasoning_native_item_json = {};
  bool redacted = false;
  // Internal provider-output identity retained for exact continuation. Other
  // provider families may ignore it. It is never exposed through public
  // stream/event surfaces.
  std::string provider_item_id = {};
  std::optional<std::size_t> provider_output_index = std::nullopt;
  AssistantPhase assistant_phase = AssistantPhase::Unknown;
  // Image attachment metadata. Session/RPC records store references and
  // provider serializers may load bytes transiently after validation.
  std::string attachment_id = {};
  std::string mime_type = {};
  ava::core::NoLogString storage_path = {};
  std::string sha256 = {};
  std::size_t byte_size = 0;
  // Transient provider payload populated only after session attachment storage
  // has verified size, hash, and path containment. Never persist this field.
  ava::core::NoLogString data_base64 = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ChatMessage
{
  std::string role = {};
  // Readable legacy fallback content. This remains populated for providers that
  // do not understand content_parts and for diagnostics/session reconstruction.
  ava::core::NoLogString content = {};
  // Optional native structure for providers that support text/tool-use/tool-result
  // content blocks. When non-empty, native-capable providers should serialize
  // these parts as canonical and treat content only as fallback/diagnostic text.
  // Text-only providers should ignore this field.
  std::vector<ContentPart> content_parts = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ProviderReasoningOptions
{
  std::string type = {};
  std::optional<long long> budget_tokens = std::nullopt;
  std::string display = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ProviderRequest
{
  std::string provider_id;
  std::string model_id;
  ava::core::NoLogString system_prompt;
  std::vector<ChatMessage> messages;
  std::vector<ava::core::NoLogString> tools_json;
  bool stream = true;
  std::optional<long long> max_output_tokens = std::nullopt;
  std::optional<ProviderReasoningOptions> reasoning = std::nullopt;
  std::string system_prompt_cache_ttl = {};
  // Per-model transport quirks copied from ModelInfo.compatibility_quirks so
  // request serializers can stay free of hardcoded model-id special cases.
  std::vector<std::string> compatibility_quirks = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ProviderAuthContext
{
  ava::core::NoLogString access_token;
  std::string credential_type;
  ava::core::NoLogString account_id;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TokenUsage
{
  std::optional<long long> input_tokens;
  std::optional<long long> output_tokens;
  std::optional<long long> reasoning_tokens;
  std::optional<long long> cache_read_tokens;
  std::optional<long long> cache_write_tokens;
  std::optional<long long> total_tokens;
  std::optional<long long> estimated_input_bytes;
  std::optional<long long> estimated_output_bytes;
  std::optional<long long> estimated_total_bytes;
  bool estimated = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class StreamEventType
{
  TextStart,
  TextDelta,
  TextEnd,
  ReasoningStart,
  ReasoningDelta,
  ReasoningEnd,
  ToolCallStart,
  ToolCallDelta,
  ToolCallEnd,
  Done,
  Error,
};

enum class ProviderErrorKind
{
  Authentication,
  RateLimited,
  Quota,
  InvalidRequest,
  ContextOverflow,
  Refusal,
  ContentFilter,
  Transient,
  Unknown,
};

struct StreamEvent
{
  StreamEventType type = StreamEventType::Done;
  ava::core::NoLogString text;
  std::string tool_call_id;
  std::string tool_name;
  ava::core::NoLogString error_message;
  std::optional<TokenUsage> usage;
  // Native output-item identity is intentionally distinct from the logical
  // function call ID used for tool dispatch. It remains internal to provider
  // parsing and assistant-turn reconstruction.
  std::string provider_item_id = {};
  std::optional<std::size_t> provider_output_index = std::nullopt;
  AssistantPhase assistant_phase = AssistantPhase::Unknown;
  // Present only on a provider terminal event. The value is closed at the
  // provider boundary; unknown native values normalize to Error.
  std::optional<ProviderFinishReason> finish_reason = std::nullopt;
  std::string reasoning_format = {};
  ava::core::NoLogString reasoning_signature = {};
  ava::core::NoLogString reasoning_redacted_data = {};
  // Opaque provider-only JSON for exact reasoning-item replay. Stream bridges
  // must clear this field before publishing an event outside the agent loop.
  ava::core::NoLogString reasoning_native_item_json = {};
  bool redacted = false;
  bool reasoning_signature_present = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class StreamParser
{
 public:
  virtual ~StreamParser() = default;
  [[nodiscard]] virtual ava::core::Result<std::vector<StreamEvent>> append(std::string_view chunk) = 0;
  [[nodiscard]] virtual ava::core::Result<std::vector<StreamEvent>> finish() = 0;

  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

class Provider
{
 public:
  virtual ~Provider() = default;
  [[nodiscard]] virtual ava::core::Result<ava::http::HttpRequest> build_request(ProviderRequest const& request, std::string_view access_token) const = 0;
  [[nodiscard]] virtual ava::core::Result<ava::http::HttpRequest> build_request(ProviderRequest const& request, ProviderAuthContext const& auth) const;
  [[nodiscard]] virtual ava::core::VoidResult apply_auth_options(ava::http::HttpRequest& request, ProviderAuthContext const& auth) const;
  [[nodiscard]] virtual std::unique_ptr<StreamParser> create_stream_parser() const;
  [[nodiscard]] virtual ava::core::Result<std::vector<StreamEvent>> parse_response(ava::http::HttpResponse const& response, bool stream) const;

  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

[[nodiscard]] std::string to_string(AssistantPhase phase);
[[nodiscard]] std::optional<AssistantPhase> assistant_phase_from_string(std::string_view value);
[[nodiscard]] bool is_known_assistant_phase(AssistantPhase phase) noexcept;
[[nodiscard]] std::string to_string(StreamEventType type);
[[nodiscard]] std::string to_string(ProviderErrorKind kind);
[[nodiscard]] ProviderErrorKind classify_provider_error(ava::http::HttpResponse const& response);
[[nodiscard]] ava::http::ResponseRetryDecision provider_retry_decision(ava::http::HttpResponse const& response);
[[nodiscard]] bool is_context_overflow_error(ava::core::Error const& error);
struct ImageInputPolicy
{
  std::size_t max_attachments_per_request = 16;
  std::size_t max_bytes_per_image = 20 * 1024 * 1024;
  std::size_t max_bytes_per_request = 40 * 1024 * 1024;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ImageInputPolicy image_input_policy_for_api_family(std::string_view api_family) noexcept;
[[nodiscard]] bool is_supported_image_mime_type(std::string_view mime_type);
[[nodiscard]] bool request_has_image_parts(ProviderRequest const& request);
[[nodiscard]] ava::core::VoidResult validate_image_content_parts(ProviderRequest const& request, bool model_supports_images);

}  // namespace ava::provider
