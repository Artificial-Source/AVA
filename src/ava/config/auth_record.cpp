#include "sys.h"
#include "ava/config/auth_record.h"
#include "ava/core/json.h"

#include <cctype>
#include <optional>
#include <utility>

namespace ava::config {
namespace {

ava::core::Error auth_record_error(ava::core::ErrorCategory category, std::string message, std::filesystem::path const& path)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("path", path.string());
  return error;
}

bool is_json_ws(char ch)
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

void skip_json_ws(std::string_view text, std::size_t& index)
{
  while (index < text.size() && is_json_ws(text[index])) ++index;
}

bool is_hex_digit(char ch)
{
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

ava::core::Result<std::string> parse_auth_member_key(std::string_view text, std::size_t& index, std::filesystem::path const& path)
{
  if (index >= text.size() || text[index] != '"')
  {
    return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file member key must be a JSON string", path));
  }
  ++index;
  std::string key;
  bool escaped = false;
  for (; index < text.size(); ++index)
  {
    char const ch = text[index];
    if (escaped)
    {
      switch (ch)
      {
        case '"':
        case '\\':
        case '/':
          key.push_back(ch);
          break;
        case 'b':
          key.push_back('\b');
          break;
        case 'f':
          key.push_back('\f');
          break;
        case 'n':
          key.push_back('\n');
          break;
        case 'r':
          key.push_back('\r');
          break;
        case 't':
          key.push_back('\t');
          break;
        case 'u':
          if (index + 4 >= text.size() || !is_hex_digit(text[index + 1]) || !is_hex_digit(text[index + 2]) || !is_hex_digit(text[index + 3]) ||
              !is_hex_digit(text[index + 4]))
          {
            return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file member key has an invalid unicode escape", path));
          }
          key.push_back('?');
          index += 4;
          break;
        default:
          return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file member key has an invalid escape", path));
      }
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
      ++index;
      return key;
    }
    if (static_cast<unsigned char>(ch) < 0x20)
    {
      return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file member key contains a control character", path));
    }
    key.push_back(ch);
  }
  return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file member key is unterminated", path));
}

std::optional<std::size_t> raw_json_value_end(std::string_view text, std::size_t start)
{
  if (start >= text.size())
    return std::nullopt;
  char const first = text[start];
  bool const valid_start =
      first == '"' || first == '{' || first == '[' || first == '-' || first == 't' || first == 'f' || first == 'n' || (first >= '0' && first <= '9');
  if (!valid_start)
    return std::nullopt;
  bool in_string = false;
  bool escaped = false;
  int object_depth = 0;
  int array_depth = 0;
  std::size_t last_non_ws = start;
  for (std::size_t index = start; index < text.size(); ++index)
  {
    char const ch = text[index];
    if (in_string)
    {
      if (escaped)
      {
        escaped = false;
      }
      else if (ch == '\\')
      {
        escaped = true;
      }
      else if (ch == '"')
      {
        in_string = false;
      }
      last_non_ws = index + 1;
      continue;
    }

    if (ch == '"')
    {
      in_string = true;
      last_non_ws = index + 1;
      continue;
    }
    if (ch == '{')
    {
      ++object_depth;
      last_non_ws = index + 1;
      continue;
    }
    if (ch == '[')
    {
      ++array_depth;
      last_non_ws = index + 1;
      continue;
    }
    if (ch == '}')
    {
      if (object_depth == 0 && array_depth == 0)
        return last_non_ws;
      --object_depth;
      last_non_ws = index + 1;
      if (object_depth == 0 && array_depth == 0)
        return index + 1;
      continue;
    }
    if (ch == ']')
    {
      if (array_depth == 0)
        return std::nullopt;
      --array_depth;
      last_non_ws = index + 1;
      if (object_depth == 0 && array_depth == 0)
        return index + 1;
      continue;
    }
    if (ch == ',' && object_depth == 0 && array_depth == 0)
      return last_non_ws;
    if (!is_json_ws(ch))
      last_non_ws = index + 1;
  }
  if (in_string || object_depth != 0 || array_depth != 0 || last_non_ws == start)
    return std::nullopt;
  return last_non_ws;
}

}  // namespace

bool is_valid_provider_id(std::string_view provider_id)
{
  if (provider_id.empty() || provider_id.size() > 128)
    return false;
  for (char const ch : provider_id)
  {
    auto const uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) != 0 || ch == '-' || ch == '_')
      continue;
    return false;
  }
  return true;
}

ava::core::Result<std::string> provider_credential_object_json(ProviderCredential const& credential)
{
  if (!is_valid_provider_id(credential.provider_id))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "provider id is invalid");
    error.with_context("provider", credential.provider_id);
    return std::unexpected(std::move(error));
  }
  if (credential.access_token.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "provider credential token is empty");
    error.with_context("provider", credential.provider_id);
    return std::unexpected(std::move(error));
  }
  if (credential.credential_type == "api_key")
  {
    return "{\n    \"type\": \"api_key\",\n    \"api_key\": \"" + ava::core::json::escape(credential.access_token) + "\"\n  }";
  }
  if (credential.provider_id == "anthropic" && credential.credential_type == "oauth") {
    std::string body = "{\n    \"type\": \"oauth\",\n    \"access_token\": \"" +
                       ava::core::json::escape(credential.access_token) + "\"";
    if (!credential.refresh_token.empty()) {
      body += ",\n    \"refresh_token\": \"" + ava::core::json::escape(credential.refresh_token) + "\"";
    }
    if (credential.expires_at > 0) {
      body += ",\n    \"expires_at\": " + std::to_string(credential.expires_at);
    }
    if (!credential.account_id.empty()) {
      body += ",\n    \"account_id\": \"" + ava::core::json::escape(credential.account_id) + "\"";
    }
    if (!credential.source_metadata.empty()) {
      body += ",\n    \"source\": \"" + ava::core::json::escape(credential.source_metadata) + "\"";
    }
    body += "\n  }";
    return body;
  }
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "provider credential type is unsupported");
  error.with_context("provider", credential.provider_id);
  error.with_context("credential_type", credential.credential_type);
  return std::unexpected(std::move(error));
}

ava::core::Result<std::vector<AuthRecordMember>> parse_auth_record_members(std::string_view text, std::filesystem::path const& path)
{
  std::vector<AuthRecordMember> members;
  std::size_t index = 0;
  skip_json_ws(text, index);
  if (index >= text.size())
    return members;
  if (text[index] != '{')
  {
    return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file must be a JSON object", path));
  }
  ++index;
  skip_json_ws(text, index);
  if (index < text.size() && text[index] == '}')
  {
    ++index;
    skip_json_ws(text, index);
    if (index == text.size())
      return members;
    return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file has trailing content after JSON object", path));
  }
  while (index < text.size())
  {
    auto key = parse_auth_member_key(text, index, path);
    if (!key)
      return std::unexpected(std::move(key.error()));
    skip_json_ws(text, index);
    if (index >= text.size() || text[index] != ':')
    {
      return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file member is missing a colon", path));
    }
    ++index;
    skip_json_ws(text, index);
    auto const value_start = index;
    auto const value_end = raw_json_value_end(text, value_start);
    if (!value_end || *value_end <= value_start)
    {
      return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file member has an invalid value", path));
    }
    members.push_back(AuthRecordMember{.key = std::move(*key), .raw_value = std::string(text.substr(value_start, *value_end - value_start))});
    index = *value_end;
    skip_json_ws(text, index);
    if (index >= text.size())
      break;
    if (text[index] == ',')
    {
      ++index;
      skip_json_ws(text, index);
      continue;
    }
    if (text[index] == '}')
    {
      ++index;
      skip_json_ws(text, index);
      if (index == text.size() && !ava::core::json::is_valid_object(text))
        return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file member has an invalid value", path));
      if (index == text.size())
        return members;
      return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file has trailing content after JSON object", path));
    }
    return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file object has an invalid separator", path));
  }
  return std::unexpected(auth_record_error(ava::core::ErrorCategory::InvalidArgument, "auth file JSON object is unterminated", path));
}

std::string serialize_auth_record_members(std::vector<AuthRecordMember> const& members)
{
  std::string body = "{\n";
  for (std::size_t index = 0; index < members.size(); ++index)
  {
    body += "  \"" + ava::core::json::escape(members[index].key) + "\": " + members[index].raw_value;
    body += index + 1 == members.size() ? "\n" : ",\n";
  }
  body += "}\n";
  return body;
}

}  // namespace ava::config
