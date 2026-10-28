#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace ava::app::acp {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxRecordBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxNestingDepth = 64;
inline constexpr std::size_t kMaxStringBytes = 256U * 1024U;
inline constexpr std::size_t kMaxCollectionItems = 4096;
inline constexpr std::size_t kMaxMethodBytes = 256;
inline constexpr std::size_t kMaxIdStringBytes = 256;
inline constexpr std::size_t kMaxOutboundRecords = 256;
inline constexpr std::size_t kMaxOutboundBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxPendingCalls = 64;
inline constexpr std::size_t kMaxInflightRequests = 8;
inline constexpr std::size_t kMaxWorkerQueue = 32;
// Enough workers for every bounded inbound request plus cancellation and
// lifecycle notifications; independent sessions must not serialize prompts.
inline constexpr std::size_t kWorkerCount = kMaxInflightRequests + 2;
inline constexpr auto kDefaultCallTimeout = std::chrono::seconds(30);
inline constexpr auto kWriteStallTimeout = std::chrono::seconds(2);
inline constexpr auto kShutdownGrace = std::chrono::seconds(2);
inline constexpr int kShutdownEscalationExitCode = 70;

struct NullJsonRpcId
{
  friend bool operator==(NullJsonRpcId const&, NullJsonRpcId const&) = default;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using JsonRpcId = std::variant<NullJsonRpcId, std::int64_t, std::string>;

enum class EnvelopeIntent
{
  Unknown,
  Request,
  Notification,
  Response,
};

struct JsonRpcError
{
  int code = -32603;
  std::string message;
  std::optional<std::string> data_json;
  std::optional<JsonRpcId> id;
  EnvelopeIntent intent = EnvelopeIntent::Unknown;
  bool suppress_response = false;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct Request
{
  JsonRpcId id;
  std::string method;
  std::optional<std::string> params_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct Notification
{
  std::string method;
  std::optional<std::string> params_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct Response
{
  JsonRpcId id;
  std::string result_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ErrorResponse
{
  JsonRpcId id;
  JsonRpcError error;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using Message = std::variant<Request, Notification, Response, ErrorResponse>;

struct ImplementationInfo
{
  std::string name;
  std::string version;
  std::optional<std::string> title;
  std::optional<std::string> meta_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ClientCapabilities
{
  bool read_text_file = false;
  bool write_text_file = false;
  bool terminal = false;
  bool boolean_config_options = false;
  std::optional<std::string> meta_json;
  std::optional<std::string> fs_meta_json;
  std::optional<std::string> session_meta_json;
  std::optional<std::string> config_options_meta_json;
  std::optional<std::string> boolean_config_meta_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct InitializeRequest
{
  std::uint16_t protocol_version = 0;
  ClientCapabilities client_capabilities;
  std::optional<ImplementationInfo> client_info;
  std::optional<std::string> meta_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] bool ids_equal(JsonRpcId const& lhs, JsonRpcId const& rhs) noexcept;
[[nodiscard]] std::string id_debug_string(JsonRpcId const& id);
// Type-tagged identity for connection-local maps; unlike debug text it cannot
// alias a string id with an integer or null id.
[[nodiscard]] std::string id_key(JsonRpcId const& id);
[[nodiscard]] ava::core::Error protocol_error(std::string message);

}  // namespace ava::app::acp
