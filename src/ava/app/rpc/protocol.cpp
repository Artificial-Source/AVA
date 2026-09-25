#include "sys.h"
#include "protocol.h"
#include "serialization_json.h"
#include "ava/agent/job_control.h"
#include "ava/session/session_store.h"
#include "ava/core/json.h"
#include "ava/core/strict_json.h"
#include "ava/core/string_utils.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <istream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "debug.h"

namespace ava::app::rpc {
namespace {

std::string supported_session_entry_versions_json()
{
  std::string versions = "[";
  for (long long version = 0; version <= ava::session::kCurrentSessionEntryVersion; ++version)
  {
    if (version != 0)
      versions += ',';
    versions += std::to_string(version);
  }
  versions += ']';
  return versions;
}

bool is_json_object_line(std::string_view line)
{
  line = core::trim_view(line);
  if (line.size() < 2 || line.front() != '{')
    return false;
  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  std::size_t object_end = std::string_view::npos;
  for (std::size_t index = 0; index < line.size(); ++index)
  {
    char const ch = line[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\' && in_string)
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
    {
      in_string = !in_string;
      continue;
    }
    if (in_string)
      continue;
    if (ch == '{')
    {
      ++depth;
    }
    else if (ch == '}')
    {
      --depth;
      if (depth == 0)
      {
        object_end = index;
        break;
      }
      if (depth < 0)
        return false;
    }
  }
  if (in_string || depth != 0 || object_end == std::string_view::npos)
    return false;
  return core::trim(line.substr(object_end + 1)).empty();
}

ava::core::Result<std::optional<long long>> exact_optional_integer_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::optional<long long>{};
  auto const field_name = std::string(key);
  std::size_t end = *start;
  if (end < object.size() && object[end] == '-')
    ++end;
  auto const digits_start = end;
  while (end < object.size() && std::isdigit(static_cast<unsigned char>(object[end])) != 0) ++end;
  if (end == digits_start)
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an integer"));
  bool const negative = object[*start] == '-';
  auto const unsigned_start = negative ? *start + 1 : *start;
  if (end - unsigned_start > 1 && object[unsigned_start] == '0')
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an integer"));
  }
  while (end < object.size() && std::isspace(static_cast<unsigned char>(object[end])) != 0) ++end;
  if (end < object.size() && object[end] != ',' && object[end] != '}')
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an integer"));
  }
  try
  {
    return std::optional<long long>{std::stoll(std::string(object.substr(*start, end - *start)))};
  }
  catch (...)
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " is out of range"));
  }
}

ava::core::Result<std::optional<bool>> exact_optional_bool_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::optional<bool>{};
  auto const tail = object.substr(*start);
  auto token_ends = [&](std::size_t end) {
    while (end < tail.size() && std::isspace(static_cast<unsigned char>(tail[end])) != 0) ++end;
    return end == tail.size() || tail[end] == ',' || tail[end] == '}';
  };
  if (tail.starts_with("true") && token_ends(4))
    return std::optional<bool>{true};
  if (tail.starts_with("false") && token_ends(5))
    return std::optional<bool>{false};
  return std::unexpected(invalid_rpc("RPC " + std::string(key) + " must be a boolean"));
}

ava::core::Result<std::optional<std::string>> exact_optional_string_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::optional<std::string>{};
  if (*start >= object.size() || object[*start] != '"')
  {
    return std::unexpected(invalid_rpc("RPC " + std::string(key) + " must be a string"));
  }
  auto value = ava::core::json::string_field(object, key);
  if (!value)
  {
    return std::unexpected(invalid_rpc("RPC " + std::string(key) + " must be a string"));
  }
  return std::optional<std::string>{std::move(*value)};
}

ava::core::Result<std::optional<std::string>> plugin_command_arguments_field(std::string_view object, bool validate)
{
  auto const start = ava::core::json::field_value_start(object, "arguments");
  if (!start)
    return std::optional<std::string>{};
  if (*start >= object.size())
  {
    return std::unexpected(invalid_rpc("RPC arguments must be an object"));
  }

  if (object[*start] == '{')
  {
    auto arguments = ava::core::json::object_field(object, "arguments");
    if (!arguments && validate)
    {
      return std::unexpected(invalid_rpc("RPC arguments must be an object"));
    }
    return arguments;
  }

  if (object[*start] == '"')
  {
    auto arguments = ava::core::json::string_field(object, "arguments");
    if (!arguments && validate)
    {
      return std::unexpected(invalid_rpc("RPC arguments must be an object"));
    }
    if (validate && (!arguments || ava::core::validate_strict_json(*arguments, kMaxRpcNestingDepth) != ava::core::StrictJsonStatus::Valid ||
                     !is_json_object_line(*arguments)))
    {
      return std::unexpected(invalid_rpc("RPC string arguments must contain a JSON object"));
    }
    return arguments;
  }

  if (validate)
  {
    return std::unexpected(invalid_rpc("RPC arguments must be an object"));
  }
  return std::optional<std::string>{};
}

bool is_rpc_identifier_metacharacter(char ch)
{
  switch (ch)
  {
    case '"':
    case '\'':
    case '\\':
    case '`':
    case '$':
    case '&':
    case '|':
    case ';':
    case '<':
    case '>':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
      return true;
    default:
      return false;
  }
}

ava::core::VoidResult validate_rpc_identifier(std::string_view value, std::string_view field_name)
{
  if (value.size() > kMaxRpcIdentifierBytes)
  {
    auto error = invalid_rpc("RPC identifier is too long");
    error.with_context("field", std::string(field_name));
    error.with_context("max_bytes", std::to_string(kMaxRpcIdentifierBytes));
    return std::unexpected(std::move(error));
  }

  for (char const ch : value)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F || ch == ' ' || is_rpc_identifier_metacharacter(ch))
    {
      auto error = invalid_rpc("RPC identifier contains invalid character");
      error.with_context("field", std::string(field_name));
      return std::unexpected(std::move(error));
    }
  }

  return {};
}

ava::core::VoidResult validate_optional_rpc_identifier(std::optional<std::string> const& value, std::string_view field_name)
{
  if (!value)
    return {};
  return validate_rpc_identifier(*value, field_name);
}

ava::core::VoidResult validate_optional_rpc_text(std::optional<std::string> const& value, std::string_view field_name, std::size_t max_bytes)
{
  if (!value)
    return {};
  if (value->size() > max_bytes)
  {
    auto error = invalid_rpc("RPC text field is too long");
    error.with_context("field", std::string(field_name));
    error.with_context("max_bytes", std::to_string(max_bytes));
    return std::unexpected(std::move(error));
  }

  for (char const ch : *value)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
    {
      auto error = invalid_rpc("RPC text field contains invalid character");
      error.with_context("field", std::string(field_name));
      return std::unexpected(std::move(error));
    }
  }

  return {};
}

ava::core::VoidResult validate_optional_rpc_multiline_text(std::optional<std::string> const& value, std::string_view field_name, std::size_t max_bytes)
{
  if (!value)
    return {};
  if (value->size() > max_bytes)
  {
    auto error = invalid_rpc("RPC text field is too long");
    error.with_context("field", std::string(field_name));
    error.with_context("max_bytes", std::to_string(max_bytes));
    return std::unexpected(std::move(error));
  }

  for (char const ch : *value)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if ((byte < 0x20 && ch != '\n' && ch != '\t') || byte == 0x7F)
    {
      auto error = invalid_rpc("RPC text field contains invalid character");
      error.with_context("field", std::string(field_name));
      return std::unexpected(std::move(error));
    }
  }

  return {};
}

void skip_json_ws(std::string_view text, std::size_t& index)
{
  while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) ++index;
}

std::optional<std::size_t> balanced_json_value_end(std::string_view text, std::size_t start, char open, char close)
{
  if (start >= text.size() || text[start] != open)
    return std::nullopt;
  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (std::size_t index = start; index < text.size(); ++index)
  {
    char const ch = text[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\' && in_string)
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
    {
      in_string = !in_string;
      continue;
    }
    if (in_string)
      continue;
    if (ch == open)
    {
      ++depth;
      continue;
    }
    if (ch == close)
    {
      --depth;
      if (depth == 0)
        return index + 1;
      if (depth < 0)
        return std::nullopt;
    }
  }
  return std::nullopt;
}

ava::core::Result<std::optional<std::vector<std::string>>> optional_string_array_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::optional<std::vector<std::string>>{};
  auto const field_name = std::string(key);
  if (*start >= object.size() || object[*start] != '[')
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of strings"));
  }

  std::size_t index = *start + 1;
  std::size_t string_count = 0;
  skip_json_ws(object, index);
  if (index < object.size() && object[index] != ']')
  {
    while (index < object.size())
    {
      if (object[index] != '"')
      {
        return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of strings"));
      }
      bool escaped = false;
      bool closed = false;
      for (++index; index < object.size(); ++index)
      {
        char const ch = object[index];
        if (escaped)
        {
          escaped = false;
          continue;
        }
        if (ch == '\\')
        {
          escaped = true;
          continue;
        }
        if (ch == '"')
        {
          closed = true;
          ++index;
          break;
        }
      }
      if (!closed)
        return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of strings"));
      ++string_count;

      skip_json_ws(object, index);
      if (index >= object.size())
      {
        return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of strings"));
      }
      if (object[index] == ',')
      {
        ++index;
        skip_json_ws(object, index);
        continue;
      }
      if (object[index] == ']')
        break;
      return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of strings"));
    }
  }

  if (index >= object.size() || object[index] != ']')
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of strings"));
  }
  ++index;
  skip_json_ws(object, index);
  if (index < object.size() && object[index] != ',' && object[index] != '}')
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of strings"));
  }

  auto values = ava::core::json::strings_in_array_field(object, key);
  if (values.size() != string_count)
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of strings"));
  }
  return std::optional<std::vector<std::string>>{std::move(values)};
}

ava::core::Result<RpcImageUpload> parse_rpc_image_upload(std::string_view image_object)
{
  auto type = exact_optional_string_field(image_object, "type");
  if (!type)
    return std::unexpected(std::move(type.error()));
  if (!*type || (*type)->empty())
    return std::unexpected(invalid_rpc("RPC images entries require type"));
  if (**type != "image")
    return std::unexpected(invalid_rpc("RPC images entries must have type image"));
  if (auto valid = validate_optional_rpc_text(*type, "images.type", 16); !valid)
    return std::unexpected(std::move(valid.error()));

  auto data = exact_optional_string_field(image_object, "data");
  if (!data)
    return std::unexpected(std::move(data.error()));
  if (!*data || (*data)->empty())
    return std::unexpected(invalid_rpc("RPC images entries require data"));
  if (auto valid = validate_optional_rpc_text(*data, "images.data", kMaxRpcPromptImageDataBase64Bytes); !valid)
    return std::unexpected(std::move(valid.error()));

  auto camel_mime_type = exact_optional_string_field(image_object, "mimeType");
  if (!camel_mime_type)
    return std::unexpected(std::move(camel_mime_type.error()));
  auto snake_mime_type = exact_optional_string_field(image_object, "mime_type");
  if (!snake_mime_type)
    return std::unexpected(std::move(snake_mime_type.error()));
  if (!*camel_mime_type && !*snake_mime_type)
    return std::unexpected(invalid_rpc("RPC images entries require mimeType"));
  if (*camel_mime_type && *snake_mime_type && **camel_mime_type != **snake_mime_type)
    return std::unexpected(invalid_rpc("RPC images mimeType aliases must match"));
  auto mime_type = *camel_mime_type ? std::move(**camel_mime_type) : std::move(**snake_mime_type);
  if (mime_type.empty())
    return std::unexpected(invalid_rpc("RPC images entries require mimeType"));
  auto optional_mime_type = std::optional<std::string>{mime_type};
  if (auto valid = validate_optional_rpc_text(optional_mime_type, "images.mimeType", kMaxRpcPromptImageMimeTypeBytes); !valid)
    return std::unexpected(std::move(valid.error()));

  return RpcImageUpload{.type = std::move(**type), .data_base64 = std::move(**data), .mime_type = std::move(mime_type)};
}

ava::core::Result<std::optional<std::vector<RpcImageUpload>>> optional_image_upload_array_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::optional<std::vector<RpcImageUpload>>{};
  auto const field_name = std::string(key);
  if (*start >= object.size() || object[*start] != '[')
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of image objects"));
  }
  auto const end = balanced_json_value_end(object, *start, '[', ']');
  if (!end)
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of image objects"));
  }

  std::size_t index = *start + 1;
  std::vector<RpcImageUpload> values;
  skip_json_ws(object, index);
  if (index < object.size() && index + 1 != *end)
  {
    while (index < object.size())
    {
      if (index + 1 >= *end || object[index] != '{')
      {
        return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of image objects"));
      }
      auto const item_end = balanced_json_value_end(object, index, '{', '}');
      if (!item_end || *item_end > *end - 1)
      {
        return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of image objects"));
      }
      auto item = parse_rpc_image_upload(object.substr(index, *item_end - index));
      if (!item)
        return std::unexpected(std::move(item.error()));
      values.push_back(std::move(*item));

      index = *item_end;
      skip_json_ws(object, index);
      if (index >= object.size())
      {
        return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of image objects"));
      }
      if (object[index] == ',')
      {
        ++index;
        skip_json_ws(object, index);
        continue;
      }
      if (object[index] == ']')
        break;
      return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of image objects"));
    }
  }

  index = *end;
  skip_json_ws(object, index);
  if (index < object.size() && object[index] != ',' && object[index] != '}')
  {
    return std::unexpected(invalid_rpc("RPC " + field_name + " must be an array of image objects"));
  }
  return std::optional<std::vector<RpcImageUpload>>{std::move(values)};
}

ava::core::VoidResult validate_optional_rpc_text_array(std::optional<std::vector<std::string>> const& values, std::string_view field_name,
                                                       std::size_t max_entries, std::size_t max_bytes)
{
  if (!values)
    return {};
  if (values->size() > max_entries)
  {
    auto error = invalid_rpc("RPC text array field has too many entries");
    error.with_context("field", std::string(field_name));
    error.with_context("max_entries", std::to_string(max_entries));
    return std::unexpected(std::move(error));
  }
  for (auto const& value : *values)
  {
    auto wrapped = std::optional<std::string>{value};
    if (auto valid = validate_optional_rpc_text(wrapped, field_name, max_bytes); !valid)
    {
      return valid;
    }
  }
  return {};
}

bool command_type_is(std::string_view type, std::initializer_list<std::string_view> candidates)
{
  return std::ranges::any_of(candidates, [type](std::string_view candidate) { return type == candidate; });
}

bool is_runtime_cancellation_error(ava::core::Error const& error)
{
  return error.code() == ava::core::ErrorCode::Canceled;
}

std::string rpc_error_code(ava::core::Error const& error)
{
  for (auto context = error.context().rbegin(); context != error.context().rend(); ++context)
  {
    if (context->key == "rpc_error_code" && !context->value.empty())
      return std::string(stable_rpc_error_code(context->value));
  }
  if (is_runtime_cancellation_error(error))
    return "canceled";
  switch (error.category())
  {
    case ava::core::ErrorCategory::InvalidArgument:
      return "invalid_request";
    case ava::core::ErrorCategory::Io:
      return "io_error";
    case ava::core::ErrorCategory::NotFound:
      return "not_found";
    case ava::core::ErrorCategory::PermissionDenied:
      return "permission_denied";
    case ava::core::ErrorCategory::Configuration:
      return "configuration_error";
    case ava::core::ErrorCategory::Provider:
      return "provider_error";
    case ava::core::ErrorCategory::Session:
      return "session_error";
    case ava::core::ErrorCategory::Tool:
      return "tool_error";
    case ava::core::ErrorCategory::Unknown:
      return "internal_error";
  }
  return std::string(stable_rpc_error_code("internal_error"));
}

}  // namespace

ava::core::Error invalid_rpc(std::string message)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  error.with_context("rpc_error_code", "invalid_request");
  return error;
}

ava::core::VoidResult validate_protocol_version(RpcCommand const& command)
{
  if (!command.protocol_version)
    return {};
  if (*command.protocol_version == kRpcProtocolVersion)
    return {};

  auto error = invalid_rpc("unsupported RPC protocol version");
  error.with_context("protocol_version", std::to_string(*command.protocol_version));
  error.with_context("supported_protocol_version", std::to_string(kRpcProtocolVersion));
  return std::unexpected(std::move(error));
}

std::string rpc_protocol_result_json()
{
  return "{\"protocol_version\":" + std::to_string(kRpcProtocolVersions.protocol) + ",\"supported_protocol_versions\":[" +
         std::to_string(kRpcProtocolVersions.protocol) + "],\"event_schema_version\":" + std::to_string(kRpcProtocolVersions.event_schema) +
         ",\"supported_event_schema_versions\":[" + std::to_string(kRpcProtocolVersions.event_schema) +
         "],\"session_entry_version\":" + std::to_string(ava::session::kCurrentSessionEntryVersion) +
         ",\"supported_session_entry_versions\":" + supported_session_entry_versions_json() +
         ",\"capabilities\":[\"direct_bash_rpc\",\"job_controls\"],\"direct_command_types\":[\"run_bash\",\"run_command\",\"list_jobs\","
         "\"get_job\",\"wait_job\",\"get_job_result\",\"cancel_job\",\"promote_job\"]}";
}

std::string parse_error_response_id(std::string_view line)
{
  if (!ava::core::json::is_valid_utf8(line) || !is_json_object_line(line))
    return "";
  auto id = ava::core::json::string_field(line, "id");
  if (!id || id->empty())
    return "";
  if (auto valid = validate_rpc_identifier(*id, "id"); !valid)
    return "";
  return *id;
}

ava::core::Result<bool> read_rpc_line_bounded(std::istream& in, std::string& line)
{
  DoutEntering(dc::rpc, "read_rpc_line_bounded(input=" << static_cast<void*>(&in) << ", buffer_capacity=" << line.capacity() << ")");

  line.clear();
  bool oversized = false;
  while (true)
  {
    auto const next = in.get();
    if (next == std::istream::traits_type::eof())
    {
      if (oversized)
        return std::unexpected(invalid_rpc("RPC request line is too large"));
      if (in.eof())
        return !line.empty() || oversized;
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read RPC stdin"));
    }
    char const ch = static_cast<char>(next);
    if (ch == '\n')
      break;
    if (line.size() >= kMaxRpcLineBytes)
    {
      oversized = true;
      continue;
    }
    if (!oversized)
      line.push_back(ch);
  }
  if (oversized)
    return std::unexpected(invalid_rpc("RPC request line is too large"));

  Dout(dc::rpc, "Read RPC record: bytes=" << line.size());
  return true;
}

}  // namespace ava::app::rpc

namespace ava::app {

ava::core::Result<RpcCommand> parse_rpc_command_line(std::string_view line)
{
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1);
  if (!ava::core::json::is_valid_utf8(line))
    return std::unexpected(rpc::invalid_rpc("RPC request is not valid UTF-8"));
  if (line.size() > rpc::kMaxRpcLineBytes)
    return std::unexpected(rpc::invalid_rpc("RPC request line is too large"));
  auto const strict_status = ava::core::validate_strict_json(line, rpc::kMaxRpcNestingDepth);
  if (strict_status == ava::core::StrictJsonStatus::NestingTooDeep)
    return std::unexpected(rpc::invalid_rpc("RPC JSON nesting exceeds limit"));
  if (strict_status == ava::core::StrictJsonStatus::DuplicateObjectKey)
    return std::unexpected(rpc::invalid_rpc("RPC JSON object contains duplicate member names"));
  if (strict_status != ava::core::StrictJsonStatus::Valid || !rpc::is_json_object_line(line))
    return std::unexpected(rpc::invalid_rpc("malformed RPC JSON object"));

  auto id = ava::core::json::string_field(line, "id");
  if (!id || id->empty())
    return std::unexpected(rpc::invalid_rpc("RPC request requires a non-empty string id"));
  if (auto valid = rpc::validate_rpc_identifier(*id, "id"); !valid)
    return std::unexpected(std::move(valid.error()));
  auto type = ava::core::json::string_field(line, "type");
  if (!type || type->empty())
    return std::unexpected(rpc::invalid_rpc("RPC request requires a non-empty string type"));
  auto protocol_version = rpc::exact_optional_integer_field(line, "protocol_version");
  if (!protocol_version)
    return std::unexpected(std::move(protocol_version.error()));
  auto selected_string = [&](std::string_view key, std::initializer_list<std::string_view> command_types) -> ava::core::Result<std::optional<std::string>> {
    if (!rpc::command_type_is(*type, command_types))
      return ava::core::json::string_field(line, key);
    return rpc::exact_optional_string_field(line, key);
  };

  auto message = selected_string("message", {"prompt", "steer", "follow_up"});
  if (!message)
    return std::unexpected(std::move(message.error()));
  auto instructions = selected_string("instructions", {"compact"});
  if (!instructions)
    return std::unexpected(std::move(instructions.error()));
  auto request_id = selected_string("request_id", {"permission_reply", "question_reply"});
  if (!request_id)
    return std::unexpected(std::move(request_id.error()));
  if (rpc::command_type_is(*type, {"permission_reply", "question_reply"}))
    if (auto valid = rpc::validate_optional_rpc_identifier(*request_id, "request_id"); !valid)
      return std::unexpected(std::move(valid.error()));
  auto correlation_id = selected_string("correlation_id", {"permission_reply", "question_reply"});
  if (!correlation_id)
    return std::unexpected(std::move(correlation_id.error()));
  if (rpc::command_type_is(*type, {"permission_reply", "question_reply"}))
    if (auto valid = rpc::validate_optional_rpc_identifier(*correlation_id, "correlation_id"); !valid)
      return std::unexpected(std::move(valid.error()));
  auto provider = selected_string("provider", {"set_model", "summarize_branch"});
  if (!provider)
    return std::unexpected(std::move(provider.error()));
  if (rpc::command_type_is(*type, {"set_model", "summarize_branch"}))
    if (auto valid = rpc::validate_optional_rpc_identifier(*provider, "provider"); !valid)
      return std::unexpected(std::move(valid.error()));
  auto model = selected_string("model", {"set_model", "summarize_branch"});
  if (!model)
    return std::unexpected(std::move(model.error()));
  if (rpc::command_type_is(*type, {"set_model", "summarize_branch"}))
    if (auto valid = rpc::validate_optional_rpc_identifier(*model, "model"); !valid)
      return std::unexpected(std::move(valid.error()));

  auto plugin_id = selected_string("plugin_id", {"inspect_plugin", "remove_plugin", "enable_plugin", "disable_plugin", "list_plugin_prompts",
                                                 "get_plugin_prompt", "list_plugin_skills", "get_plugin_skill", "run_plugin_command"});
  if (!plugin_id)
    return std::unexpected(std::move(plugin_id.error()));
  if (rpc::command_type_is(*type, {"inspect_plugin", "remove_plugin", "enable_plugin", "disable_plugin", "list_plugin_prompts", "get_plugin_prompt",
                                   "list_plugin_skills", "get_plugin_skill", "run_plugin_command"}))
    if (auto valid = rpc::validate_optional_rpc_identifier(*plugin_id, "plugin_id"); !valid)
      return std::unexpected(std::move(valid.error()));
  auto name = selected_string("name", {"invoke_command", "get_plugin_prompt", "get_plugin_skill", "run_plugin_command"});
  if (!name)
    return std::unexpected(std::move(name.error()));
  if (rpc::command_type_is(*type, {"invoke_command", "get_plugin_prompt", "get_plugin_skill", "run_plugin_command"}))
    if (auto valid = rpc::validate_optional_rpc_identifier(*name, "name"); !valid)
      return std::unexpected(std::move(valid.error()));
  auto const is_plugin_command = *type == "run_plugin_command";
  auto arguments = rpc::plugin_command_arguments_field(line, is_plugin_command);
  if (!arguments)
    return std::unexpected(std::move(arguments.error()));
  auto server_id = selected_string("server_id", {"inspect_mcp_server", "list_mcp_tools", "restart_mcp_server"});
  if (!server_id)
    return std::unexpected(std::move(server_id.error()));
  if (rpc::command_type_is(*type, {"inspect_mcp_server", "list_mcp_tools", "restart_mcp_server"}))
    if (auto valid = rpc::validate_optional_rpc_identifier(*server_id, "server_id"); !valid)
      return std::unexpected(std::move(valid.error()));

  auto reasoning_level = selected_string("reasoning_level", {"set_reasoning"});
  if (!reasoning_level)
    return std::unexpected(std::move(reasoning_level.error()));
  auto reasoning_display = selected_string("reasoning_display", {"set_reasoning"});
  if (!reasoning_display)
    return std::unexpected(std::move(reasoning_display.error()));
  ava::core::Result<std::optional<long long>> reasoning_budget_tokens = std::optional<long long>{};
  if (*type == "set_reasoning")
    reasoning_budget_tokens = rpc::exact_optional_integer_field(line, "reasoning_budget_tokens");
  if (!reasoning_budget_tokens)
    return std::unexpected(std::move(reasoning_budget_tokens.error()));

  auto decision = selected_string("decision", {"permission_reply"});
  if (!decision)
    return std::unexpected(std::move(decision.error()));
  auto command_arguments = selected_string("command_arguments", {"invoke_command"});
  if (!command_arguments)
    return std::unexpected(std::move(command_arguments.error()));
  if (*type == "invoke_command")
    if (auto valid = rpc::validate_optional_rpc_text(*command_arguments, "command_arguments", rpc::kMaxRpcLineBytes / 2); !valid)
      return std::unexpected(std::move(valid.error()));

  auto output_path = std::optional<std::string>{};
  if (*type == "export_html")
  {
    auto parsed_camel_output_path = rpc::exact_optional_string_field(line, "outputPath");
    if (!parsed_camel_output_path)
      return std::unexpected(std::move(parsed_camel_output_path.error()));
    auto parsed_snake_output_path = rpc::exact_optional_string_field(line, "output_path");
    if (!parsed_snake_output_path)
      return std::unexpected(std::move(parsed_snake_output_path.error()));
    if (*parsed_camel_output_path && *parsed_snake_output_path && **parsed_camel_output_path != **parsed_snake_output_path)
      return std::unexpected(rpc::invalid_rpc("RPC export_html outputPath aliases must match"));
    if (*parsed_camel_output_path)
      output_path = std::move(**parsed_camel_output_path);
    else if (*parsed_snake_output_path)
      output_path = std::move(**parsed_snake_output_path);
    if (output_path && output_path->empty())
      return std::unexpected(rpc::invalid_rpc("RPC export_html outputPath must be non-empty when provided"));
    if (auto valid = rpc::validate_optional_rpc_text(output_path, "outputPath", rpc::kMaxRpcExportPathBytes); !valid)
      return std::unexpected(std::move(valid.error()));
  }

  auto rule_id = std::optional<std::string>{};
  if (rpc::command_type_is(*type, {"permission_rule_remove"}))
  {
    auto parsed_rule_id = rpc::exact_optional_string_field(line, "rule_id");
    if (!parsed_rule_id)
      return std::unexpected(std::move(parsed_rule_id.error()));
    rule_id = std::move(*parsed_rule_id);
    if (auto valid = rpc::validate_optional_rpc_identifier(rule_id, "rule_id"); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
  }

  auto grant_id = std::optional<std::string>{};
  if (*type == "permission_grant_revoke")
  {
    auto parsed_grant_id = rpc::exact_optional_string_field(line, "grant_id");
    if (!parsed_grant_id)
      return std::unexpected(std::move(parsed_grant_id.error()));
    grant_id = std::move(*parsed_grant_id);
    if (auto valid = rpc::validate_optional_rpc_identifier(grant_id, "grant_id"); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
  }

  auto action = std::optional<std::string>{};
  auto operation = std::optional<std::string>{};
  auto scope = std::optional<std::string>{};
  auto mode = std::optional<std::string>{};
  auto target_path = std::optional<std::string>{};
  auto command_text = std::optional<std::string>{};
  auto tool_name = std::optional<std::string>{};
  auto command_recipe_key = std::optional<std::string>{};
  auto recipe_display = std::optional<std::string>{};
  auto critical_acknowledged = std::optional<bool>{};
  if (rpc::command_type_is(*type, {"run_command", "run_bash"}))
  {
    auto parsed_command = rpc::exact_optional_string_field(line, "command");
    if (!parsed_command)
      return std::unexpected(std::move(parsed_command.error()));
    command_text = std::move(*parsed_command);
    if (auto valid = rpc::validate_optional_rpc_text(command_text, "command", rpc::kMaxRpcRuleCommandBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
  }
  if (rpc::command_type_is(*type, {"permission_rule_add"}))
  {
    auto parsed_action = rpc::exact_optional_string_field(line, "action");
    if (!parsed_action)
      return std::unexpected(std::move(parsed_action.error()));
    action = std::move(*parsed_action);
    if (auto valid = rpc::validate_optional_rpc_identifier(action, "action"); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_operation = rpc::exact_optional_string_field(line, "operation");
    if (!parsed_operation)
      return std::unexpected(std::move(parsed_operation.error()));
    operation = std::move(*parsed_operation);
    if (auto valid = rpc::validate_optional_rpc_identifier(operation, "operation"); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_scope = rpc::exact_optional_string_field(line, "scope");
    if (!parsed_scope)
      return std::unexpected(std::move(parsed_scope.error()));
    scope = std::move(*parsed_scope);
    if (auto valid = rpc::validate_optional_rpc_identifier(scope, "scope"); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_mode = rpc::exact_optional_string_field(line, "mode");
    if (!parsed_mode)
      return std::unexpected(std::move(parsed_mode.error()));
    mode = std::move(*parsed_mode);
    if (auto valid = rpc::validate_optional_rpc_identifier(mode, "mode"); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_tool_name = rpc::exact_optional_string_field(line, "tool_name");
    if (!parsed_tool_name)
      return std::unexpected(std::move(parsed_tool_name.error()));
    tool_name = std::move(*parsed_tool_name);
    if (auto valid = rpc::validate_optional_rpc_identifier(tool_name, "tool_name"); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_target_path = rpc::exact_optional_string_field(line, "target_path");
    if (!parsed_target_path)
      return std::unexpected(std::move(parsed_target_path.error()));
    target_path = std::move(*parsed_target_path);
    if (auto valid = rpc::validate_optional_rpc_text(target_path, "target_path", rpc::kMaxRpcRulePathBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto path_alias = rpc::exact_optional_string_field(line, "path");
    if (!path_alias)
      return std::unexpected(std::move(path_alias.error()));
    if (auto valid = rpc::validate_optional_rpc_text(*path_alias, "path", rpc::kMaxRpcRulePathBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_command = rpc::exact_optional_string_field(line, "command");
    if (!parsed_command)
      return std::unexpected(std::move(parsed_command.error()));
    command_text = std::move(*parsed_command);
    if (auto valid = rpc::validate_optional_rpc_text(command_text, "command", rpc::kMaxRpcRuleCommandBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_recipe_key = rpc::exact_optional_string_field(line, "command_recipe_key");
    if (!parsed_recipe_key)
      return std::unexpected(std::move(parsed_recipe_key.error()));
    command_recipe_key = std::move(*parsed_recipe_key);
    if (auto valid = rpc::validate_optional_rpc_text(command_recipe_key, "command_recipe_key", 128); !valid)
      return std::unexpected(std::move(valid.error()));
    auto parsed_recipe_display = rpc::exact_optional_string_field(line, "recipe_display");
    if (!parsed_recipe_display)
      return std::unexpected(std::move(parsed_recipe_display.error()));
    recipe_display = std::move(*parsed_recipe_display);
    if (auto valid = rpc::validate_optional_rpc_text(recipe_display, "recipe_display", 1024); !valid)
      return std::unexpected(std::move(valid.error()));
    auto parsed_critical_acknowledged = rpc::exact_optional_bool_field(line, "critical_acknowledged");
    if (!parsed_critical_acknowledged)
      return std::unexpected(std::move(parsed_critical_acknowledged.error()));
    critical_acknowledged = std::move(*parsed_critical_acknowledged);
  }

  auto reason = std::optional<std::string>{};
  if (rpc::command_type_is(*type, {"permission_reply", "permission_rule_add", "summarize_branch"}))
  {
    auto parsed_reason = rpc::exact_optional_string_field(line, "reason");
    if (!parsed_reason)
      return std::unexpected(std::move(parsed_reason.error()));
    reason = std::move(*parsed_reason);
    if (auto valid = rpc::validate_optional_rpc_text(reason, "reason", rpc::kMaxRpcReasonBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
  }

  auto session_name = std::optional<std::string>{};
  auto labels = std::optional<std::vector<std::string>>{};
  if (rpc::command_type_is(*type, {"set_session_name", "fork_session", "clone_session"}))
  {
    auto parsed_session_name = rpc::exact_optional_string_field(line, "session_name");
    if (!parsed_session_name)
      return std::unexpected(std::move(parsed_session_name.error()));
    session_name = std::move(*parsed_session_name);
    if (auto valid = rpc::validate_optional_rpc_text(session_name, "session_name", rpc::kMaxRpcSessionNameBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
  }
  if (rpc::command_type_is(*type, {"set_session_labels", "fork_session", "clone_session"}))
  {
    auto parsed_labels = rpc::optional_string_array_field(line, "labels");
    if (!parsed_labels)
      return std::unexpected(std::move(parsed_labels.error()));
    if (auto valid = rpc::validate_optional_rpc_text_array(*parsed_labels, "labels", rpc::kMaxRpcSessionLabels, rpc::kMaxRpcSessionLabelBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    labels = std::move(*parsed_labels);
  }

  auto branch_from_entry_id = std::optional<std::string>{};
  if (*type == "clone_session" && ava::core::json::field_value_start(line, "branch_from_entry_id"))
  {
    return std::unexpected(rpc::invalid_rpc("clone_session does not support branch_from_entry_id"));
  }
  if (*type == "fork_session")
  {
    auto parsed_branch_from_entry_id = rpc::exact_optional_string_field(line, "branch_from_entry_id");
    if (!parsed_branch_from_entry_id)
      return std::unexpected(std::move(parsed_branch_from_entry_id.error()));
    branch_from_entry_id = std::move(*parsed_branch_from_entry_id);
    if (auto valid = rpc::validate_optional_rpc_text(branch_from_entry_id, "branch_from_entry_id", rpc::kMaxRpcEntryIdBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    if (branch_from_entry_id && branch_from_entry_id->empty())
    {
      return std::unexpected(rpc::invalid_rpc("fork_session branch_from_entry_id must be non-empty when provided"));
    }
  }

  auto branch_root_entry_id = std::optional<std::string>{};
  auto branch_tip_entry_id = std::optional<std::string>{};
  auto summary = std::optional<std::string>{};
  if (*type == "summarize_branch")
  {
    auto parsed_branch_root_entry_id = rpc::exact_optional_string_field(line, "branch_root_entry_id");
    if (!parsed_branch_root_entry_id)
      return std::unexpected(std::move(parsed_branch_root_entry_id.error()));
    branch_root_entry_id = std::move(*parsed_branch_root_entry_id);
    if (auto valid = rpc::validate_optional_rpc_text(branch_root_entry_id, "branch_root_entry_id", rpc::kMaxRpcEntryIdBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_branch_tip_entry_id = rpc::exact_optional_string_field(line, "branch_tip_entry_id");
    if (!parsed_branch_tip_entry_id)
      return std::unexpected(std::move(parsed_branch_tip_entry_id.error()));
    branch_tip_entry_id = std::move(*parsed_branch_tip_entry_id);
    if (auto valid = rpc::validate_optional_rpc_text(branch_tip_entry_id, "branch_tip_entry_id", rpc::kMaxRpcEntryIdBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_summary = rpc::exact_optional_string_field(line, "summary");
    if (!parsed_summary)
      return std::unexpected(std::move(parsed_summary.error()));
    summary = std::move(*parsed_summary);
    if (auto valid = rpc::validate_optional_rpc_multiline_text(summary, "summary", rpc::kMaxRpcBranchSummaryBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
  }

  auto job_id = std::optional<std::string>{};
  auto timeout_ms = std::optional<long long>{};
  if (rpc::command_type_is(*type, {"list_jobs", "get_job", "wait_job", "get_job_result", "cancel_job", "promote_job"}))
  {
    auto parsed_job_id = rpc::exact_optional_string_field(line, "job_id");
    if (!parsed_job_id)
      return std::unexpected(std::move(parsed_job_id.error()));
    job_id = std::move(*parsed_job_id);
    if (job_id)
    {
      if (job_id->empty())
        return std::unexpected(rpc::invalid_rpc("RPC job_id must be non-empty when provided"));
      if (job_id->size() > ava::agent::kMaxPublicJobIdBytes)
        return std::unexpected(rpc::invalid_rpc("RPC job_id is too long"));
      if (auto valid = rpc::validate_optional_rpc_identifier(job_id, "job_id"); !valid)
        return std::unexpected(std::move(valid.error()));
    }
    if (*type == "wait_job")
    {
      auto parsed_timeout = rpc::exact_optional_integer_field(line, "timeout_ms");
      if (!parsed_timeout)
        return std::unexpected(std::move(parsed_timeout.error()));
      timeout_ms = std::move(*parsed_timeout);
      if (timeout_ms && *timeout_ms <= 0)
        return std::unexpected(rpc::invalid_rpc("RPC timeout_ms must be a positive integer"));
    }
  }

  auto session_id = ava::core::json::string_field(line, "session_id");
  if (rpc::command_type_is(*type, {"fork_session", "clone_session", "summarize_branch", "open_session", "switch_session"}))
  {
    auto parsed_session_id = rpc::exact_optional_string_field(line, "session_id");
    if (!parsed_session_id)
      return std::unexpected(std::move(parsed_session_id.error()));
    session_id = std::move(*parsed_session_id);
    if (session_id && session_id->empty())
    {
      return std::unexpected(rpc::invalid_rpc(*type + " session_id must be non-empty when provided"));
    }
  }

  auto selected_options = std::optional<std::vector<std::string>>{};
  auto answer = std::optional<std::string>{};
  auto selected = std::optional<std::string>{};
  if (*type == "question_reply")
  {
    auto parsed_selected_options = rpc::optional_string_array_field(line, "selected_options");
    if (!parsed_selected_options)
      return std::unexpected(std::move(parsed_selected_options.error()));
    if (auto valid = rpc::validate_optional_rpc_text_array(*parsed_selected_options, "selected_options", rpc::kMaxRpcQuestionSelectedOptions,
                                                           rpc::kMaxRpcQuestionAnswerBytes);
        !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    selected_options = std::move(*parsed_selected_options);
    auto parsed_answer = rpc::exact_optional_string_field(line, "answer");
    if (!parsed_answer)
      return std::unexpected(std::move(parsed_answer.error()));
    answer = std::move(*parsed_answer);
    if (auto valid = rpc::validate_optional_rpc_text(answer, "answer", rpc::kMaxRpcQuestionAnswerBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    auto parsed_selected = rpc::exact_optional_string_field(line, "selected");
    if (!parsed_selected)
      return std::unexpected(std::move(parsed_selected.error()));
    selected = std::move(*parsed_selected);
    if (auto valid = rpc::validate_optional_rpc_text(selected, "selected", rpc::kMaxRpcQuestionAnswerBytes); !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
  }

  auto attachments = std::optional<std::vector<std::string>>{};
  auto images = std::optional<std::vector<RpcImageUpload>>{};
  if (*type == "prompt")
  {
    auto parsed_attachments = rpc::optional_string_array_field(line, "attachments");
    if (!parsed_attachments)
      return std::unexpected(std::move(parsed_attachments.error()));
    if (auto valid =
            rpc::validate_optional_rpc_text_array(*parsed_attachments, "attachments", rpc::kMaxRpcPromptAttachments, rpc::kMaxRpcPromptAttachmentPathBytes);
        !valid)
    {
      return std::unexpected(std::move(valid.error()));
    }
    if (*parsed_attachments && std::ranges::any_of(**parsed_attachments, [](std::string const& path) { return path.empty(); }))
    {
      return std::unexpected(rpc::invalid_rpc("RPC attachments entries must be non-empty"));
    }
    attachments = std::move(*parsed_attachments);

    auto parsed_images = rpc::optional_image_upload_array_field(line, "images");
    if (!parsed_images)
      return std::unexpected(std::move(parsed_images.error()));
    images = std::move(*parsed_images);

    auto const attachment_count = attachments ? attachments->size() : 0U;
    auto const upload_count = images ? images->size() : 0U;
    if (attachment_count + upload_count > rpc::kMaxRpcPromptAttachments)
    {
      auto error = rpc::invalid_rpc("RPC prompt image inputs have too many entries");
      error.with_context("max_entries", std::to_string(rpc::kMaxRpcPromptAttachments));
      return std::unexpected(std::move(error));
    }
  }

  ava::core::Result<std::optional<std::string>> path = std::optional<std::string>{};
  if (rpc::command_type_is(*type, {"validate_plugin", "install_plugin", "permission_rule_add"}))
    path = rpc::exact_optional_string_field(line, "path");
  if (!path)
    return std::unexpected(std::move(path.error()));
  if (auto valid = rpc::validate_optional_rpc_text(*path, "path", rpc::kMaxRpcRulePathBytes); !valid)
    return std::unexpected(std::move(valid.error()));

  return RpcCommand{.id = std::move(*id),
                    .type = std::move(*type),
                    .protocol_version = std::move(*protocol_version),
                    .message = std::move(*message),
                    .session_id = std::move(session_id),
                    .job_id = std::move(job_id),
                    .timeout_ms = std::move(timeout_ms),
                    .provider = std::move(*provider),
                    .model = std::move(*model),
                    .instructions = std::move(*instructions),
                    .reasoning_level = std::move(*reasoning_level),
                    .reasoning_budget_tokens = std::move(*reasoning_budget_tokens),
                    .reasoning_display = std::move(*reasoning_display),
                    .request_id = std::move(*request_id),
                    .correlation_id = std::move(*correlation_id),
                    .grant_id = std::move(grant_id),
                    .rule_id = std::move(rule_id),
                    .decision = std::move(*decision),
                    .action = std::move(action),
                    .operation = std::move(operation),
                    .scope = std::move(scope),
                    .mode = std::move(mode),
                    .reason = std::move(reason),
                    .session_name = std::move(session_name),
                    .labels = std::move(labels),
                    .branch_from_entry_id = std::move(branch_from_entry_id),
                    .branch_root_entry_id = std::move(branch_root_entry_id),
                    .branch_tip_entry_id = std::move(branch_tip_entry_id),
                    .summary = std::move(summary),
                    .answer = std::move(answer),
                    .selected = std::move(selected),
                    .selected_options = std::move(selected_options),
                    .attachments = std::move(attachments),
                    .images = std::move(images),
                    .plugin_id = std::move(*plugin_id),
                    .name = std::move(*name),
                    .arguments = std::move(*arguments),
                    .command_arguments = std::move(*command_arguments),
                    .server_id = std::move(*server_id),
                    .output_path = std::move(output_path),
                    .path = std::move(*path),
                    .target_path = std::move(target_path),
                    .command = std::move(command_text),
                    .tool_name = std::move(tool_name),
                    .command_recipe_key = std::move(command_recipe_key),
                    .recipe_display = std::move(recipe_display),
                    .critical_acknowledged = std::move(critical_acknowledged)};
}

std::string serialize_rpc_success_jsonl(std::string_view id, std::string_view result_json)
{
  std::string json = "{\"id\":\"" + ava::core::json::escape(id) + "\",\"type\":\"response\",\"success\":true";
  if (!result_json.empty())
  {
    json += ",\"result\":";
    json += result_json;
  }
  json += "}\n";
  return json;
}

std::string serialize_rpc_error_jsonl(std::string_view id, ava::core::Error const& error)
{
  std::string json = "{\"id\":\"" + ava::core::json::escape(id) + "\",\"type\":\"response\",\"success\":false,\"error\":{";
  json += rpc::string_field_json("category", ava::core::to_string(error.category()));
  json += ',';
  json += rpc::string_field_json("code", rpc::rpc_error_code(error));
  json += ',';
  json += rpc::string_field_json("message", error.message());
  json += ',';
  json += rpc::string_field_json("details", error.format());
  json += "}}\n";
  return json;
}

}  // namespace ava::app
