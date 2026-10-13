#include "sys.h"
#include "ava/app/settings_json.h"
#include "ava/core/json.h"

#include <utility>
#include <vector>

namespace ava::app {
namespace {

std::optional<std::size_t> balanced_settings_json_value_end(std::string_view text, std::size_t start)
{
  if (start >= text.size() || (text[start] != '{' && text[start] != '['))
    return std::nullopt;
  std::vector<char> expected_closers;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t index = start; index < text.size(); ++index)
  {
    auto const ch = text[index];
    if (in_string)
    {
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
        in_string = false;
      continue;
    }
    if (ch == '"')
    {
      in_string = true;
      continue;
    }
    if (ch == '{')
    {
      expected_closers.push_back('}');
      continue;
    }
    if (ch == '[')
    {
      expected_closers.push_back(']');
      continue;
    }
    if (ch == '}' || ch == ']')
    {
      if (expected_closers.empty() || expected_closers.back() != ch)
        return std::nullopt;
      expected_closers.pop_back();
      if (expected_closers.empty())
        return index + 1;
    }
  }
  return std::nullopt;
}

}  // namespace

bool is_settings_json_whitespace(char ch) noexcept
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

void skip_settings_json_whitespace(std::string_view text, std::size_t& offset) noexcept
{
  while (offset < text.size() && is_settings_json_whitespace(text[offset]))
    ++offset;
}

std::optional<std::size_t> settings_json_string_literal_end(std::string_view text, std::size_t start) noexcept
{
  if (start >= text.size() || text[start] != '"')
    return std::nullopt;
  bool escaped = false;
  for (std::size_t index = start + 1; index < text.size(); ++index)
  {
    auto const ch = text[index];
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
      return index;
  }
  return std::nullopt;
}

std::optional<std::size_t> settings_json_value_end(std::string_view text, std::size_t start)
{
  if (start >= text.size())
    return std::nullopt;
  if (text[start] == '"')
  {
    auto const end = settings_json_string_literal_end(text, start);
    if (!end)
      return std::nullopt;
    return *end + 1;
  }
  if (text[start] == '{' || text[start] == '[')
    return balanced_settings_json_value_end(text, start);

  auto end = start;
  while (end < text.size() && text[end] != ',' && text[end] != '}')
    ++end;
  while (end > start && is_settings_json_whitespace(text[end - 1]))
    --end;
  return end > start ? std::optional<std::size_t>(end) : std::nullopt;
}

std::optional<std::vector<SettingsJsonEntry>> parse_settings_json_object_entries(std::string_view object)
{
  if (!ava::core::json::is_valid_object(object))
    return std::nullopt;

  std::vector<SettingsJsonEntry> entries;
  std::size_t offset = 0;
  skip_settings_json_whitespace(object, offset);
  if (offset >= object.size() || object[offset] != '{')
    return std::nullopt;
  ++offset;
  skip_settings_json_whitespace(object, offset);
  if (offset < object.size() && object[offset] == '}')
    return entries;

  while (offset < object.size())
  {
    skip_settings_json_whitespace(object, offset);
    auto const key_start = offset;
    auto const key_end = settings_json_string_literal_end(object, key_start);
    if (!key_end)
      return std::nullopt;
    auto raw_key = std::string(object.substr(key_start, *key_end - key_start + 1));
    auto const decoded_key = ava::core::json::string_field("{\"value\":" + raw_key + "}", "value");
    if (!decoded_key)
      return std::nullopt;
    offset = *key_end + 1;
    skip_settings_json_whitespace(object, offset);
    if (offset >= object.size() || object[offset] != ':')
      return std::nullopt;
    ++offset;
    skip_settings_json_whitespace(object, offset);
    auto const value_start = offset;
    auto const value_end = settings_json_value_end(object, value_start);
    if (!value_end)
      return std::nullopt;
    auto raw_value = std::string(object.substr(value_start, *value_end - value_start));
    entries.push_back(SettingsJsonEntry{.key = *decoded_key, .raw_key = std::move(raw_key), .raw_value = std::move(raw_value)});
    offset = *value_end;
    skip_settings_json_whitespace(object, offset);
    if (offset < object.size() && object[offset] == ',')
    {
      ++offset;
      continue;
    }
    if (offset < object.size() && object[offset] == '}')
      return entries;
    return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace ava::app
