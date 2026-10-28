#include "sys.h"
#include "ava/app/acp/codec.h"
#include "ava/app/acp/codec_json_internal.h"
#include "ava/app/acp/envelope_intent.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>

namespace ava::app::acp {
namespace {

using Json = nlohmann::json;

JsonRpcError codec_error(int code, std::string message, std::optional<JsonRpcId> id = std::nullopt, EnvelopeIntent intent = EnvelopeIntent::Unknown)
{
  return JsonRpcError{.code = code,
                      .message = std::move(message),
                      .data_json = std::nullopt,
                      .id = std::move(id),
                      .intent = intent,
                      .suppress_response = intent == EnvelopeIntent::Notification || intent == EnvelopeIntent::Response};
}

EnvelopeIntent classify_envelope(Json const& root)
{
  if (!root.is_object())
    return EnvelopeIntent::Unknown;
  if (root.contains("method"))
    return root.contains("id") ? EnvelopeIntent::Request : EnvelopeIntent::Notification;
  if (root.contains("id") || root.contains("result") || root.contains("error"))
    return EnvelopeIntent::Response;
  return EnvelopeIntent::Unknown;
}

bool value_within_limits(Json const& value, std::size_t depth = 1)
{
  if (depth > kMaxNestingDepth)
    return false;
  if (value.is_string())
    return value.get_ref<std::string const&>().size() <= kMaxStringBytes;
  if (value.is_array())
  {
    if (value.size() > kMaxCollectionItems)
      return false;
    return std::ranges::all_of(value, [depth](Json const& item) { return value_within_limits(item, depth + 1); });
  }
  if (value.is_object())
  {
    if (value.size() > kMaxCollectionItems)
      return false;
    for (auto const& [key, item] : value.items())
    {
      if (key.size() > kMaxStringBytes || !value_within_limits(item, depth + 1))
        return false;
    }
  }
  return true;
}

std::optional<std::int64_t> parse_signed_integer(Json const& value)
{
  if (value.is_number_unsigned())
  {
    auto const number = value.get<std::uint64_t>();
    if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return static_cast<std::int64_t>(number);
    return std::nullopt;
  }
  if (value.is_number_integer())
    return value.get<std::int64_t>();
  return std::nullopt;
}

std::optional<JsonRpcId> parse_id(Json const& value)
{
  if (value.is_null())
    return JsonRpcId(NullJsonRpcId{});
  if (value.is_string())
  {
    auto const& text = value.get_ref<std::string const&>();
    if (text.size() <= kMaxIdStringBytes)
      return JsonRpcId(text);
    return std::nullopt;
  }
  auto integer = parse_signed_integer(value);
  if (integer)
    return JsonRpcId(*integer);
  return std::nullopt;
}

bool id_within_limit(JsonRpcId const& id)
{
  auto const* text = std::get_if<std::string>(&id);
  return text == nullptr || text->size() <= kMaxIdStringBytes;
}

Json id_json(JsonRpcId const& id)
{
  if (std::holds_alternative<NullJsonRpcId>(id))
    return nullptr;
  if (auto const* number = std::get_if<std::int64_t>(&id))
    return *number;
  return std::get<std::string>(id);
}

std::string safe_dump(Json const& value)
{
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

ava::core::Result<Json> parse_embedded(std::string_view text, std::string_view field)
{
  if (text.size() > kMaxRecordBytes || ava::core::validate_strict_json(text, kMaxNestingDepth) != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(protocol_error(std::string(field) + " exceeds ACP JSON limits or contains duplicate member names"));
  Json value = Json::parse(text.begin(), text.end(), nullptr, false, true);
  if (value.is_discarded() || !value_within_limits(value))
    return std::unexpected(protocol_error(std::string(field) + " is not bounded valid JSON"));
  return value;
}

void decode_optional_bool(Json const& object, char const* name, bool& target)
{
  auto const iterator = object.find(name);
  if (iterator != object.end() && iterator->is_boolean())
    target = iterator->get<bool>();
}

void decode_optional_meta(Json const& object, std::optional<std::string>& target)
{
  auto const iterator = object.find("_meta");
  if (iterator != object.end() && iterator->is_object())
    target = safe_dump(*iterator);
}

std::optional<ImplementationInfo> decode_implementation(Json const& value)
{
  if (!value.is_object())
    return std::nullopt;
  auto const name = value.find("name");
  auto const version = value.find("version");
  if (name == value.end() || !name->is_string() || version == value.end() || !version->is_string())
    return std::nullopt;
  ImplementationInfo info{.name = name->get<std::string>(), .version = version->get<std::string>(), .title = std::nullopt, .meta_json = std::nullopt};
  if (auto const title = value.find("title"); title != value.end() && title->is_string())
    info.title = title->get<std::string>();
  decode_optional_meta(value, info.meta_json);
  return info;
}

ClientCapabilities decode_client_capabilities(Json const& value)
{
  ClientCapabilities capabilities;
  if (!value.is_object())
    return capabilities;
  decode_optional_meta(value, capabilities.meta_json);

  if (auto const fs = value.find("fs"); fs != value.end() && fs->is_object())
  {
    decode_optional_meta(*fs, capabilities.fs_meta_json);
    decode_optional_bool(*fs, "readTextFile", capabilities.read_text_file);
    decode_optional_bool(*fs, "writeTextFile", capabilities.write_text_file);
  }
  decode_optional_bool(value, "terminal", capabilities.terminal);

  if (auto const session = value.find("session"); session != value.end() && session->is_object())
  {
    decode_optional_meta(*session, capabilities.session_meta_json);
    if (auto const config = session->find("configOptions"); config != session->end() && config->is_object())
    {
      decode_optional_meta(*config, capabilities.config_options_meta_json);
      if (auto const boolean = config->find("boolean"); boolean != config->end() && boolean->is_object())
      {
        decode_optional_meta(*boolean, capabilities.boolean_config_meta_json);
        capabilities.boolean_config_options = true;
      }
    }
  }
  return capabilities;
}

Json base_envelope()
{
  return Json{{"jsonrpc", "2.0"}};
}

ava::core::Result<std::string> dump_record(Json const& value)
{
  std::string text = safe_dump(value);
  if (text.size() + 1 > kMaxRecordBytes)
    return std::unexpected(protocol_error("encoded ACP record exceeds byte limit"));
  text.push_back('\n');
  return text;
}

}  // namespace

std::expected<nlohmann::json, JsonRpcError> decode_json_value(std::string_view text)
{
  auto value = parse_embedded(text, "ACP JSON value");
  if (!value)
    return std::unexpected(codec_error(-32602, value.error().message()));
  return std::move(*value);
}

DecodeResult decode_message(std::string_view record)
{
  auto const envelope_scan = scan_envelope_intent(record);
  auto intent = envelope_scan.intent;
  if (record.size() > kMaxRecordBytes)
  {
    intent = loop_safe_oversized_intent(envelope_scan);
    return std::unexpected(codec_error(-32700, "ACP record exceeds byte limit", std::nullopt, intent));
  }

  auto const strict_status = ava::core::validate_strict_json(record, kMaxNestingDepth);
  if (strict_status == ava::core::StrictJsonStatus::NestingTooDeep)
    return std::unexpected(codec_error(-32700, "ACP JSON nesting exceeds limit", std::nullopt, intent));
  if (strict_status == ava::core::StrictJsonStatus::Invalid)
    return std::unexpected(codec_error(-32700, "Parse error", std::nullopt, intent));

  Json root = Json::parse(record.begin(), record.end(), nullptr, false, true);
  if (root.is_discarded())
    return std::unexpected(codec_error(-32700, "Parse error", std::nullopt, intent));
  if (intent == EnvelopeIntent::Unknown)
    intent = classify_envelope(root);
  if (strict_status == ava::core::StrictJsonStatus::DuplicateObjectKey)
    return std::unexpected(codec_error(-32600, "JSON object contains duplicate member names", std::nullopt, intent));
  if (!value_within_limits(root))
    return std::unexpected(codec_error(-32700, "ACP JSON value exceeds limits", std::nullopt, intent));
  if (root.is_array())
    return std::unexpected(codec_error(-32600, "JSON-RPC batch requests are not supported"));
  if (!root.is_object())
    return std::unexpected(codec_error(-32600, "Invalid Request"));

  std::optional<JsonRpcId> valid_id;
  bool const has_id = root.contains("id");
  if (has_id)
  {
    valid_id = parse_id(root.at("id"));
    if (!valid_id)
      return std::unexpected(codec_error(-32600, "JSON-RPC id must be null, a bounded string, or a signed integer", std::nullopt, intent));
  }
  auto invalid = [&](std::string message) -> DecodeResult { return std::unexpected(codec_error(-32600, std::move(message), valid_id, intent)); };

  auto const jsonrpc = root.find("jsonrpc");
  if (jsonrpc == root.end() || !jsonrpc->is_string() || jsonrpc->get_ref<std::string const&>() != "2.0")
    return invalid("jsonrpc must be exactly \"2.0\"");

  bool const has_method = root.contains("method");
  bool const has_result = root.contains("result");
  bool const has_error = root.contains("error");
  if (has_method)
  {
    if (has_result || has_error)
      return invalid("request and response members are mutually exclusive");
    auto const& method_value = root.at("method");
    if (!method_value.is_string())
      return invalid("method must be a string");
    auto method = method_value.get<std::string>();
    if (method.empty() || method.size() > kMaxMethodBytes)
      return invalid("method length is invalid");
    std::optional<std::string> params;
    if (auto const iterator = root.find("params"); iterator != root.end() && !iterator->is_null())
    {
      if (!iterator->is_object() && !iterator->is_array())
        return invalid("params must be an object, array, or null");
      params = safe_dump(*iterator);
    }
    if (valid_id)
      return Message(Request{.id = std::move(*valid_id), .method = std::move(method), .params_json = std::move(params)});
    return Message(Notification{.method = std::move(method), .params_json = std::move(params)});
  }

  if (!has_id || has_result == has_error)
    return invalid("response must contain exactly one of result or error and an id");
  if (has_result)
    return Message(Response{.id = std::move(*valid_id), .result_json = safe_dump(root.at("result"))});

  auto const& error_value = root.at("error");
  if (!error_value.is_object())
    return invalid("error must be an object");
  auto const code = error_value.find("code");
  auto const message = error_value.find("message");
  auto const error_number = code == error_value.end() ? std::optional<std::int64_t>{} : parse_signed_integer(*code);
  if (!error_number || message == error_value.end() || !message->is_string())
    return invalid("error requires integer code and string message");
  if (*error_number < std::numeric_limits<int>::min() || *error_number > std::numeric_limits<int>::max())
    return invalid("error code is outside int32 range");
  JsonRpcError error{.code = static_cast<int>(*error_number),
                     .message = message->get<std::string>(),
                     .data_json = std::nullopt,
                     .id = std::nullopt,
                     .suppress_response = false};
  if (auto const data = error_value.find("data"); data != error_value.end())
    error.data_json = safe_dump(*data);
  return Message(ErrorResponse{.id = std::move(*valid_id), .error = std::move(error)});
}

InitializeDecodeResult decode_initialize_params(Request const& request)
{
  if (request.method != "initialize")
    return std::unexpected(codec_error(-32602, "request is not initialize", request.id));
  if (!request.params_json)
    return std::unexpected(codec_error(-32602, "initialize requires params", request.id));
  auto parsed = parse_embedded(*request.params_json, "initialize params");
  if (!parsed || !parsed->is_object())
    return std::unexpected(codec_error(-32602, "initialize params must be an object", request.id));

  auto const version = parsed->find("protocolVersion");
  auto const number = version == parsed->end() ? std::optional<std::int64_t>{} : parse_signed_integer(*version);
  if (!number)
    return std::unexpected(codec_error(-32602, "protocolVersion must be an integer", request.id));
  if (*number < 0 || *number > std::numeric_limits<std::uint16_t>::max())
    return std::unexpected(codec_error(-32602, "protocolVersion is outside uint16 range", request.id));

  InitializeRequest initialize{
      .protocol_version = static_cast<std::uint16_t>(*number), .client_capabilities = {}, .client_info = std::nullopt, .meta_json = std::nullopt};
  if (auto const capabilities = parsed->find("clientCapabilities"); capabilities != parsed->end())
    initialize.client_capabilities = decode_client_capabilities(*capabilities);
  if (auto const info = parsed->find("clientInfo"); info != parsed->end())
    initialize.client_info = decode_implementation(*info);
  decode_optional_meta(*parsed, initialize.meta_json);
  return initialize;
}

std::expected<JsonRpcId, JsonRpcError> decode_cancel_request_params(Notification const& notification)
{
  if (notification.method != "$/cancel_request" || !notification.params_json)
    return std::unexpected(codec_error(-32602, "$/cancel_request requires params"));
  auto parsed = parse_embedded(*notification.params_json, "cancel params");
  if (!parsed || !parsed->is_object())
    return std::unexpected(codec_error(-32602, "$/cancel_request params must be an object"));
  auto const id = parsed->find("requestId");
  if (id == parsed->end())
    return std::unexpected(codec_error(-32602, "$/cancel_request requires requestId"));
  auto decoded = parse_id(*id);
  if (!decoded)
    return std::unexpected(codec_error(-32602, "requestId must be null, a bounded string, or a signed integer"));
  if (auto const meta = parsed->find("_meta"); meta != parsed->end() && !meta->is_object() && !meta->is_null())
    return std::unexpected(codec_error(-32602, "_meta must be an object or null"));
  return *decoded;
}

ava::core::Result<std::string> encode_success(JsonRpcId const& id, std::string_view result_json)
{
  if (!id_within_limit(id))
    return std::unexpected(protocol_error("response id exceeds ACP limit"));
  auto result = parse_embedded(result_json, "result");
  if (!result)
    return std::unexpected(std::move(result.error()));
  Json envelope = base_envelope();
  envelope["id"] = id_json(id);
  envelope["result"] = std::move(*result);
  return dump_record(envelope);
}

ava::core::Result<std::string> encode_error(std::optional<JsonRpcId> const& id, int code, std::string_view message, std::optional<std::string_view> data_json)
{
  if (id && !id_within_limit(*id))
    return std::unexpected(protocol_error("error response id exceeds ACP limit"));
  Json error{{"code", code}, {"message", std::string(message)}};
  if (data_json)
  {
    auto data = parse_embedded(*data_json, "error data");
    if (!data)
      return std::unexpected(std::move(data.error()));
    error["data"] = std::move(*data);
  }
  Json envelope = base_envelope();
  envelope["id"] = id ? id_json(*id) : Json(nullptr);
  envelope["error"] = std::move(error);
  return dump_record(envelope);
}

ava::core::Result<std::string> encode_notification(std::string_view method, std::optional<std::string_view> params_json)
{
  if (method.empty() || method.size() > kMaxMethodBytes)
    return std::unexpected(protocol_error("notification method length is invalid"));
  Json envelope = base_envelope();
  envelope["method"] = std::string(method);
  if (params_json)
  {
    auto params = parse_embedded(*params_json, "params");
    if (!params || (!params->is_object() && !params->is_array() && !params->is_null()))
      return std::unexpected(protocol_error("notification params must be a bounded object, array, or null"));
    if (!params->is_null())
      envelope["params"] = std::move(*params);
  }
  return dump_record(envelope);
}

ava::core::Result<std::string> encode_request(JsonRpcId const& id, std::string_view method, std::optional<std::string_view> params_json)
{
  if (!id_within_limit(id))
    return std::unexpected(protocol_error("request id exceeds ACP limit"));
  auto notification = encode_notification(method, params_json);
  if (!notification)
    return notification;
  Json envelope = Json::parse(notification->begin(), std::prev(notification->end()), nullptr, false, true);
  envelope["id"] = id_json(id);
  return dump_record(envelope);
}

ava::core::Result<std::string> cancel_request_params_json(JsonRpcId const& id)
{
  if (!id_within_limit(id))
    return std::unexpected(protocol_error("cancel request id exceeds ACP limit"));
  return safe_dump(Json{{"requestId", id_json(id)}});
}

ava::core::Result<std::string> encode_message(Message const& message)
{
  return std::visit(
      [](auto const& value) -> ava::core::Result<std::string> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Request>)
          return encode_request(value.id, value.method, value.params_json ? std::optional<std::string_view>(*value.params_json) : std::nullopt);
        else if constexpr (std::is_same_v<T, Notification>)
          return encode_notification(value.method, value.params_json ? std::optional<std::string_view>(*value.params_json) : std::nullopt);
        else if constexpr (std::is_same_v<T, Response>)
          return encode_success(value.id, value.result_json);
        else
          return encode_error(value.id, value.error.code, value.error.message,
                              value.error.data_json ? std::optional<std::string_view>(*value.error.data_json) : std::nullopt);
      },
      message);
}

ava::core::Result<std::string> initialize_result_json(std::string_view agent_version, bool image_prompt_capability)
{
  Json capabilities{{"loadSession", false},
                    {"promptCapabilities", Json{{"image", image_prompt_capability}, {"audio", false}, {"embeddedContext", false}}},
                    {"mcpCapabilities", Json{{"http", false}, {"sse", false}}},
                    {"sessionCapabilities", Json{{"list", Json::object()}, {"resume", Json::object()}, {"close", Json::object()}}},
                    {"auth", Json::object()}};
  Json result{{"protocolVersion", kProtocolVersion},
              {"agentCapabilities", std::move(capabilities)},
              {"authMethods", Json::array()},
              {"agentInfo", Json{{"name", "ava"}, {"title", "AVA"}, {"version", std::string(agent_version)}}}};
  auto text = safe_dump(result);
  if (text.size() > kMaxRecordBytes)
    return std::unexpected(protocol_error("initialize result exceeds ACP record limit"));
  return text;
}

ava::core::Result<std::string> encode_initialize_result(JsonRpcId const& id, std::string_view agent_version, bool image_prompt_capability)
{
  auto result = initialize_result_json(agent_version, image_prompt_capability);
  if (!result)
    return std::unexpected(std::move(result.error()));
  return encode_success(id, *result);
}

}  // namespace ava::app::acp
