#include "sys.h"
#include "ava/plugin/manifest.h"
#include "ava/plugin/ui_protocol.h"
#include "ava/core/hex.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ava::plugin {
namespace {

constexpr std::size_t kPluginProtocolRecordMaxBytes = 64 * 1024;
constexpr std::size_t kPluginProtocolMaxJsonDepth = 128;
constexpr std::size_t kMaxUiObjectFields = 8;

ava::core::Error ui_protocol_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
}

void skip_ws(std::string_view text, std::size_t& offset)
{
  while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) != 0) ++offset;
}

std::optional<std::size_t> string_end(std::string_view text, std::size_t start)
{
  if (start >= text.size() || text[start] != '"')
    return std::nullopt;
  bool escaped = false;
  for (std::size_t index = start + 1; index < text.size(); ++index)
  {
    char const ch = text[index];
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
      return index + 1;
  }
  return std::nullopt;
}

std::optional<std::size_t> value_end(std::string_view text, std::size_t start)
{
  if (start >= text.size())
    return std::nullopt;
  if (text[start] == '"')
    return string_end(text, start);
  if (text[start] != '{' && text[start] != '[')
  {
    auto end = start;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != ']') ++end;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return end == start ? std::nullopt : std::optional<std::size_t>(end);
  }

  std::size_t object_depth = 0;
  std::size_t array_depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t index = start; index < text.size(); ++index)
  {
    char const ch = text[index];
    if (in_string)
    {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        in_string = false;
      continue;
    }
    if (ch == '"')
    {
      in_string = true;
      continue;
    }
    if (ch == '{')
      ++object_depth;
    else if (ch == '[')
      ++array_depth;
    else if (ch == '}')
    {
      if (object_depth == 0)
        return std::nullopt;
      --object_depth;
    }
    else if (ch == ']')
    {
      if (array_depth == 0)
        return std::nullopt;
      --array_depth;
    }
    if (object_depth == 0 && array_depth == 0)
      return index + 1;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> hex_code_unit(std::string_view text, std::size_t start)
{
  if (start + 4 > text.size())
    return std::nullopt;
  std::uint32_t result = 0;
  for (std::size_t index = start; index < start + 4; ++index)
  {
    auto const value = ava::core::hex_digit_value(text[index]);
    if (value < 0)
      return std::nullopt;
    result = (result << 4U) | static_cast<std::uint32_t>(value);
  }
  return result;
}

std::size_t utf8_size(std::uint32_t codepoint)
{
  if (codepoint <= 0x7FU)
    return 1;
  if (codepoint <= 0x7FFU)
    return 2;
  if (codepoint <= 0xFFFFU)
    return 3;
  return 4;
}

void append_utf8(std::string& output, std::uint32_t codepoint)
{
  if (codepoint <= 0x7FU)
  {
    output.push_back(static_cast<char>(codepoint));
  }
  else if (codepoint <= 0x7FFU)
  {
    output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
  else if (codepoint <= 0xFFFFU)
  {
    output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
  else
  {
    output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
}

std::optional<std::pair<std::uint32_t, std::size_t>> raw_utf8_codepoint(std::string_view text, std::size_t offset, std::size_t end)
{
  if (offset >= end)
    return std::nullopt;
  auto const first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80U)
    return std::pair<std::uint32_t, std::size_t>{first, 1};

  std::size_t length = 0;
  std::uint32_t codepoint = 0;
  if ((first & 0xE0U) == 0xC0U)
  {
    length = 2;
    codepoint = first & 0x1FU;
  }
  else if ((first & 0xF0U) == 0xE0U)
  {
    length = 3;
    codepoint = first & 0x0FU;
  }
  else if ((first & 0xF8U) == 0xF0U)
  {
    length = 4;
    codepoint = first & 0x07U;
  }
  else
  {
    return std::nullopt;
  }
  if (offset + length > end)
    return std::nullopt;
  for (std::size_t index = 1; index < length; ++index)
  {
    auto const byte = static_cast<unsigned char>(text[offset + index]);
    if ((byte & 0xC0U) != 0x80U)
      return std::nullopt;
    codepoint = (codepoint << 6U) | (byte & 0x3FU);
  }
  if ((length == 2 && codepoint < 0x80U) || (length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U) || codepoint > 0x10FFFFU ||
      (codepoint >= 0xD800U && codepoint <= 0xDFFFU))
  {
    return std::nullopt;
  }
  return std::pair<std::uint32_t, std::size_t>{codepoint, length};
}

bool forbidden_terminal_codepoint(std::uint32_t codepoint)
{
  return codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint <= 0x1FU || (codepoint >= 0x7FU && codepoint <= 0x9FU) ||
         codepoint == 0x061CU || codepoint == 0x200EU || codepoint == 0x200FU || codepoint == 0x2028U || codepoint == 0x2029U ||
         (codepoint >= 0x202AU && codepoint <= 0x202EU) || (codepoint >= 0x2066U && codepoint <= 0x206FU);
}

bool safe_decoded_text(std::string_view text, std::size_t max_bytes, bool require_nonempty)
{
  if ((require_nonempty && text.empty()) || text.size() > max_bytes)
    return false;
  std::size_t offset = 0;
  while (offset < text.size())
  {
    auto decoded = raw_utf8_codepoint(text, offset, text.size());
    if (!decoded || forbidden_terminal_codepoint(decoded->first))
      return false;
    offset += decoded->second;
  }
  return true;
}

template <typename Consumer>
bool walk_json_string(std::string_view literal, Consumer&& consume)
{
  if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"')
    return false;
  std::size_t offset = 1;
  auto const end = literal.size() - 1;
  while (offset < end)
  {
    if (literal[offset] != '\\')
    {
      auto decoded = raw_utf8_codepoint(literal, offset, end);
      if (!decoded || !consume(decoded->first))
        return false;
      offset += decoded->second;
      continue;
    }

    if (++offset >= end)
      return false;
    char const escaped = literal[offset++];
    std::uint32_t codepoint = 0;
    switch (escaped)
    {
      case '"':
      case '\\':
      case '/':
        codepoint = static_cast<unsigned char>(escaped);
        break;
      case 'b':
        codepoint = 0x08U;
        break;
      case 'f':
        codepoint = 0x0CU;
        break;
      case 'n':
        codepoint = 0x0AU;
        break;
      case 'r':
        codepoint = 0x0DU;
        break;
      case 't':
        codepoint = 0x09U;
        break;
      case 'u': {
        auto first = hex_code_unit(literal, offset);
        if (!first)
          return false;
        offset += 4;
        if (*first >= 0xD800U && *first <= 0xDBFFU)
        {
          if (offset + 6 > end || literal[offset] != '\\' || literal[offset + 1] != 'u')
            return false;
          auto second = hex_code_unit(literal, offset + 2);
          if (!second || *second < 0xDC00U || *second > 0xDFFFU)
            return false;
          offset += 6;
          codepoint = 0x10000U + ((*first - 0xD800U) << 10U) + (*second - 0xDC00U);
        }
        else
        {
          if (*first >= 0xDC00U && *first <= 0xDFFFU)
            return false;
          codepoint = *first;
        }
        break;
      }
      default:
        return false;
    }
    if (!consume(codepoint))
      return false;
  }
  return offset == end;
}

std::optional<std::size_t> safe_string_size(std::string_view literal, std::size_t max_bytes, bool require_nonempty)
{
  std::size_t bytes = 0;
  bool const valid = walk_json_string(literal, [&](std::uint32_t codepoint) {
    if (forbidden_terminal_codepoint(codepoint))
      return false;
    auto const next = utf8_size(codepoint);
    if (bytes > max_bytes - std::min(max_bytes, next))
      return false;
    bytes += next;
    return bytes <= max_bytes;
  });
  if (!valid || (require_nonempty && bytes == 0))
    return std::nullopt;
  return bytes;
}

std::optional<std::string> decode_safe_string(std::string_view literal, std::size_t max_bytes, bool require_nonempty)
{
  auto const bytes = safe_string_size(literal, max_bytes, require_nonempty);
  if (!bytes)
    return std::nullopt;
  std::string output;
  output.reserve(*bytes);
  if (!walk_json_string(literal, [&](std::uint32_t codepoint) {
        append_utf8(output, codepoint);
        return true;
      }))
  {
    return std::nullopt;
  }
  if (output.size() != *bytes || !ava::core::json::is_valid_utf8(output))
    return std::nullopt;
  return output;
}

struct ObjectField
{
  std::string name;
  std::string_view value;
};

struct ObjectFields
{
  std::array<ObjectField, kMaxUiObjectFields> values{};
  std::size_t count = 0;
};

std::optional<ObjectFields> parse_object_fields(std::string_view object)
{
  if (!ava::core::json::is_valid_object_with_max_depth(object, kPluginProtocolMaxJsonDepth))
    return std::nullopt;
  std::size_t offset = 0;
  skip_ws(object, offset);
  if (offset >= object.size() || object[offset++] != '{')
    return std::nullopt;

  ObjectFields result;
  skip_ws(object, offset);
  if (offset < object.size() && object[offset] == '}')
    return result;
  while (offset < object.size())
  {
    if (result.count == result.values.size())
      return std::nullopt;
    auto const key_start = offset;
    auto const key_end = string_end(object, key_start);
    if (!key_end)
      return std::nullopt;
    auto name = decode_safe_string(object.substr(key_start, *key_end - key_start), 48, true);
    if (!name || !std::ranges::all_of(*name, [](unsigned char ch) { return ch < 0x80U; }))
      return std::nullopt;
    for (std::size_t index = 0; index < result.count; ++index)
    {
      if (result.values[index].name == *name)
        return std::nullopt;
    }
    offset = *key_end;
    skip_ws(object, offset);
    if (offset >= object.size() || object[offset++] != ':')
      return std::nullopt;
    skip_ws(object, offset);
    auto const field_start = offset;
    auto const field_end = value_end(object, field_start);
    if (!field_end)
      return std::nullopt;
    result.values[result.count++] = ObjectField{.name = std::move(*name), .value = object.substr(field_start, *field_end - field_start)};
    offset = *field_end;
    skip_ws(object, offset);
    if (offset >= object.size())
      return std::nullopt;
    if (object[offset] == '}')
    {
      ++offset;
      skip_ws(object, offset);
      return offset == object.size() ? std::optional<ObjectFields>(std::move(result)) : std::nullopt;
    }
    if (object[offset++] != ',')
      return std::nullopt;
    skip_ws(object, offset);
  }
  return std::nullopt;
}

ObjectField const* field(ObjectFields const& fields, std::string_view name)
{
  for (std::size_t index = 0; index < fields.count; ++index)
  {
    if (fields.values[index].name == name)
      return &fields.values[index];
  }
  return nullptr;
}

bool exact_fields(ObjectFields const& fields, std::initializer_list<std::string_view> required, std::initializer_list<std::string_view> optional = {})
{
  if (fields.count < required.size() || fields.count > required.size() + optional.size())
    return false;
  auto is_listed = [&](std::string_view name) {
    return std::ranges::find(required, name) != required.end() || std::ranges::find(optional, name) != optional.end();
  };
  for (std::size_t index = 0; index < fields.count; ++index)
  {
    if (!is_listed(fields.values[index].name))
      return false;
  }
  return std::ranges::all_of(required, [&](std::string_view name) { return field(fields, name) != nullptr; });
}

template <std::size_t MaxItems>
struct ArrayItems
{
  std::array<std::string_view, MaxItems> values{};
  std::size_t count = 0;
};

template <std::size_t MaxItems>
std::optional<ArrayItems<MaxItems>> parse_array_items(std::string_view array, char expected_first)
{
  std::size_t offset = 0;
  skip_ws(array, offset);
  if (offset >= array.size() || array[offset++] != '[')
    return std::nullopt;
  ArrayItems<MaxItems> result;
  skip_ws(array, offset);
  if (offset < array.size() && array[offset] == ']')
  {
    ++offset;
    skip_ws(array, offset);
    return offset == array.size() ? std::optional<ArrayItems<MaxItems>>(result) : std::nullopt;
  }
  while (offset < array.size())
  {
    if (result.count == MaxItems || array[offset] != expected_first)
      return std::nullopt;
    auto const start = offset;
    auto const end = value_end(array, start);
    if (!end)
      return std::nullopt;
    result.values[result.count++] = array.substr(start, *end - start);
    offset = *end;
    skip_ws(array, offset);
    if (offset >= array.size())
      return std::nullopt;
    if (array[offset] == ']')
    {
      ++offset;
      skip_ws(array, offset);
      return offset == array.size() ? std::optional<ArrayItems<MaxItems>>(result) : std::nullopt;
    }
    if (array[offset++] != ',')
      return std::nullopt;
    skip_ws(array, offset);
  }
  return std::nullopt;
}

bool json_depth_within_limit(std::string_view value)
{
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (char const ch : value)
  {
    if (in_string)
    {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        in_string = false;
      continue;
    }
    if (ch == '"')
      in_string = true;
    else if (ch == '{' || ch == '[')
    {
      if (++depth > kPluginProtocolMaxJsonDepth)
        return false;
    }
    else if (ch == '}' || ch == ']')
    {
      if (depth == 0)
        return false;
      --depth;
    }
  }
  return !in_string && depth == 0;
}

std::optional<std::string> decoded_id(std::string_view literal)
{
  auto decoded = decode_safe_string(literal, kPluginUiIdMaxBytes, true);
  if (!decoded || !is_valid_plugin_ui_id(*decoded))
    return std::nullopt;
  return decoded;
}

std::optional<std::string> decoded_component(std::string_view literal, bool require_nonempty = false)
{
  return decode_safe_string(literal, kPluginUiTextComponentMaxBytes, require_nonempty);
}

bool add_payload_bytes(std::size_t& total, std::optional<std::size_t> const& value)
{
  if (!value || *value > kPluginUiModalPayloadMaxBytes - std::min(kPluginUiModalPayloadMaxBytes, total))
    return false;
  total += *value;
  return total <= kPluginUiModalPayloadMaxBytes;
}

ava::core::Result<PluginUiStatusRequest> parse_status(ObjectFields const& fields)
{
  if (!exact_fields(fields, {"id", "type", "text"}))
    return std::unexpected(ui_protocol_error("plugin UI status request is malformed"));
  auto const id_size = safe_string_size(field(fields, "id")->value, kPluginUiIdMaxBytes, true);
  auto const text_size = safe_string_size(field(fields, "text")->value, kPluginUiStatusTextMaxBytes, false);
  if (!id_size || !text_size)
    return std::unexpected(ui_protocol_error("plugin UI status request is malformed"));
  auto id = decoded_id(field(fields, "id")->value);
  auto text = decode_safe_string(field(fields, "text")->value, kPluginUiStatusTextMaxBytes, false);
  if (!id || !text)
    return std::unexpected(ui_protocol_error("plugin UI status request is malformed"));
  return PluginUiStatusRequest{.id = std::move(*id), .text = std::move(*text)};
}

ava::core::Result<PluginUiWidgetRequest> parse_widget(ObjectFields const& fields)
{
  if (!exact_fields(fields, {"id", "type", "title", "lines"}))
    return std::unexpected(ui_protocol_error("plugin UI widget request is malformed"));
  auto const lines = parse_array_items<kPluginUiWidgetMaxLines>(field(fields, "lines")->value, '"');
  auto const id_size = safe_string_size(field(fields, "id")->value, kPluginUiIdMaxBytes, true);
  auto const title_size = safe_string_size(field(fields, "title")->value, kPluginUiTextComponentMaxBytes, true);
  if (!lines || lines->count == 0 || !id_size || !title_size)
    return std::unexpected(ui_protocol_error("plugin UI widget request is malformed"));
  std::size_t widget_text_bytes = *title_size;
  for (std::size_t index = 0; index < lines->count; ++index)
  {
    auto const size = safe_string_size(lines->values[index], kPluginUiTextComponentMaxBytes, false);
    if (!size || *size > kPluginUiWidgetTextMaxBytes - std::min(kPluginUiWidgetTextMaxBytes, widget_text_bytes))
      return std::unexpected(ui_protocol_error("plugin UI widget request is malformed"));
    widget_text_bytes += *size;
  }
  if (widget_text_bytes > kPluginUiWidgetTextMaxBytes)
    return std::unexpected(ui_protocol_error("plugin UI widget request is malformed"));

  auto id = decoded_id(field(fields, "id")->value);
  auto title = decoded_component(field(fields, "title")->value, true);
  if (!id || !title)
    return std::unexpected(ui_protocol_error("plugin UI widget request is malformed"));
  PluginUiWidgetRequest request{.id = std::move(*id), .title = std::move(*title), .lines = {}};
  request.lines.reserve(lines->count);
  for (std::size_t index = 0; index < lines->count; ++index)
  {
    auto line = decoded_component(lines->values[index]);
    if (!line)
      return std::unexpected(ui_protocol_error("plugin UI widget request is malformed"));
    request.lines.push_back(std::move(*line));
  }
  return request;
}

struct ParsedChoiceFields
{
  ObjectFields fields;
};

ava::core::Result<PluginUiSelectRequest> parse_select(ObjectFields const& fields)
{
  if (!exact_fields(fields, {"id", "type", "title", "description", "choices"}))
    return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));
  auto const choices = parse_array_items<kPluginUiChoiceMaxCount>(field(fields, "choices")->value, '{');
  if (!choices || choices->count == 0)
    return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));

  std::size_t payload_bytes = 0;
  auto const id_size = safe_string_size(field(fields, "id")->value, kPluginUiIdMaxBytes, true);
  auto const title_size = safe_string_size(field(fields, "title")->value, kPluginUiTextComponentMaxBytes, true);
  auto const description_size = safe_string_size(field(fields, "description")->value, kPluginUiTextComponentMaxBytes, false);
  if (!add_payload_bytes(payload_bytes, id_size) || !add_payload_bytes(payload_bytes, title_size) || !add_payload_bytes(payload_bytes, description_size))
    return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));

  std::array<ParsedChoiceFields, kPluginUiChoiceMaxCount> parsed_choices{};
  for (std::size_t index = 0; index < choices->count; ++index)
  {
    auto parsed = parse_object_fields(choices->values[index]);
    if (!parsed || !exact_fields(*parsed, {"id", "label"}, {"description"}))
      return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));
    auto const choice_id_size = safe_string_size(field(*parsed, "id")->value, kPluginUiIdMaxBytes, true);
    auto const label_size = safe_string_size(field(*parsed, "label")->value, kPluginUiTextComponentMaxBytes, true);
    auto const* description_field = field(*parsed, "description");
    auto const choice_description_size =
        description_field ? safe_string_size(description_field->value, kPluginUiTextComponentMaxBytes, false) : std::optional<std::size_t>(0);
    if (!add_payload_bytes(payload_bytes, choice_id_size) || !add_payload_bytes(payload_bytes, label_size) ||
        !add_payload_bytes(payload_bytes, choice_description_size))
    {
      return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));
    }
    parsed_choices[index] = ParsedChoiceFields{.fields = std::move(*parsed)};
  }

  std::array<std::string, kPluginUiChoiceMaxCount> option_ids;
  for (std::size_t index = 0; index < choices->count; ++index)
  {
    auto option_id = decoded_id(field(parsed_choices[index].fields, "id")->value);
    bool duplicate = false;
    if (option_id)
    {
      for (std::size_t previous = 0; previous < index; ++previous) duplicate = duplicate || option_ids[previous] == *option_id;
    }
    if (!option_id || duplicate)
      return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));
    option_ids[index] = std::move(*option_id);
  }

  auto id = decoded_id(field(fields, "id")->value);
  auto title = decoded_component(field(fields, "title")->value, true);
  auto description = decoded_component(field(fields, "description")->value);
  if (!id || !title || !description)
    return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));
  PluginUiSelectRequest request{.id = std::move(*id), .title = std::move(*title), .description = std::move(*description), .choices = {}};
  request.choices.reserve(choices->count);
  for (std::size_t index = 0; index < choices->count; ++index)
  {
    auto& parsed = parsed_choices[index];
    auto label = decoded_component(field(parsed.fields, "label")->value, true);
    auto const* description_field = field(parsed.fields, "description");
    std::optional<std::string> choice_description;
    if (description_field)
    {
      auto decoded = decoded_component(description_field->value);
      if (!decoded)
        return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));
      choice_description = std::move(*decoded);
    }
    if (!label)
      return std::unexpected(ui_protocol_error("plugin UI select request is malformed"));
    request.choices.push_back(PluginUiChoice{.id = std::move(option_ids[index]), .label = std::move(*label), .description = std::move(choice_description)});
  }
  return request;
}

ava::core::Result<PluginUiConfirmRequest> parse_confirm(ObjectFields const& fields)
{
  if (!exact_fields(fields, {"id", "type", "title", "description"}))
    return std::unexpected(ui_protocol_error("plugin UI confirm request is malformed"));
  std::size_t payload_bytes = 0;
  auto const id_size = safe_string_size(field(fields, "id")->value, kPluginUiIdMaxBytes, true);
  auto const title_size = safe_string_size(field(fields, "title")->value, kPluginUiTextComponentMaxBytes, true);
  auto const description_size = safe_string_size(field(fields, "description")->value, kPluginUiTextComponentMaxBytes, false);
  if (!add_payload_bytes(payload_bytes, id_size) || !add_payload_bytes(payload_bytes, title_size) || !add_payload_bytes(payload_bytes, description_size))
    return std::unexpected(ui_protocol_error("plugin UI confirm request is malformed"));
  auto id = decoded_id(field(fields, "id")->value);
  auto title = decoded_component(field(fields, "title")->value, true);
  auto description = decoded_component(field(fields, "description")->value);
  if (!id || !title || !description)
    return std::unexpected(ui_protocol_error("plugin UI confirm request is malformed"));
  return PluginUiConfirmRequest{.id = std::move(*id), .title = std::move(*title), .description = std::move(*description)};
}

std::string_view action_name(PluginUiActionKind action) noexcept
{
  switch (action)
  {
    case PluginUiActionKind::Ack:
      return "ack";
    case PluginUiActionKind::Select:
      return "select";
    case PluginUiActionKind::Confirm:
      return "confirm";
    case PluginUiActionKind::Cancel:
      return "cancel";
  }
  return "cancel";
}

}  // namespace

bool is_plugin_ui_record_type(std::string_view type) noexcept
{
  return type == "ui.status" || type == "ui.widget" || type == "ui.select" || type == "ui.confirm";
}

bool is_valid_plugin_ui_id(std::string_view id) noexcept
{
  if (id.empty() || id.size() > kPluginUiIdMaxBytes)
    return false;
  auto const first = static_cast<unsigned char>(id.front());
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9')))
    return false;
  return std::ranges::all_of(id, [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
  });
}

std::string_view plugin_ui_request_type(PluginUiRequest const& request) noexcept
{
  return std::visit(
      [](auto const& value) -> std::string_view {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, PluginUiStatusRequest>)
          return "ui.status";
        if constexpr (std::is_same_v<T, PluginUiWidgetRequest>)
          return "ui.widget";
        if constexpr (std::is_same_v<T, PluginUiSelectRequest>)
          return "ui.select";
        return "ui.confirm";
      },
      request);
}

std::string_view plugin_ui_request_id(PluginUiRequest const& request) noexcept
{
  return std::visit([](auto const& value) -> std::string_view { return value.id; }, request);
}

std::string_view plugin_ui_request_capability(PluginUiRequest const& request) noexcept
{
  return std::visit(
      [](auto const& value) -> std::string_view {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, PluginUiStatusRequest>)
          return kPluginUiStatusCapability;
        if constexpr (std::is_same_v<T, PluginUiWidgetRequest>)
          return kPluginUiWidgetCapability;
        if constexpr (std::is_same_v<T, PluginUiSelectRequest>)
          return kPluginUiSelectCapability;
        return kPluginUiConfirmCapability;
      },
      request);
}

ava::core::Result<PluginUiRequest> parse_plugin_ui_request(std::string_view record, PluginUiProtocolState& state)
{
  if (state.record_count_ >= kPluginUiRecordMaxCount || record.size() > kPluginProtocolRecordMaxBytes || !json_depth_within_limit(record))
    return std::unexpected(ui_protocol_error("plugin UI request is malformed"));
  auto fields = parse_object_fields(record);
  if (!fields)
    return std::unexpected(ui_protocol_error("plugin UI request is malformed"));
  auto const* type_field = field(*fields, "type");
  auto const* id_field = field(*fields, "id");
  if (!type_field || !id_field)
    return std::unexpected(ui_protocol_error("plugin UI request is malformed"));
  auto request_id = decoded_id(id_field->value);
  if (!request_id || state.request_ids_.contains(*request_id))
    return std::unexpected(ui_protocol_error("plugin UI request is malformed"));
  auto type = decode_safe_string(type_field->value, 32, true);
  if (!type || !is_plugin_ui_record_type(*type))
    return std::unexpected(ui_protocol_error("plugin UI request is malformed"));
  if (*type == "ui.status" && state.status_count_ >= 1)
    return std::unexpected(ui_protocol_error("plugin UI request limit exceeded"));
  if (*type == "ui.widget" && state.widget_count_ >= kPluginUiWidgetMaxCount)
    return std::unexpected(ui_protocol_error("plugin UI request limit exceeded"));
  if ((*type == "ui.select" || *type == "ui.confirm") && state.modal_count_ >= kPluginUiModalMaxCount)
    return std::unexpected(ui_protocol_error("plugin UI request limit exceeded"));

  ava::core::Result<PluginUiRequest> parsed = [&]() -> ava::core::Result<PluginUiRequest> {
    if (*type == "ui.status")
    {
      auto request = parse_status(*fields);
      if (!request)
        return std::unexpected(std::move(request.error()));
      return PluginUiRequest(std::move(*request));
    }
    if (*type == "ui.widget")
    {
      auto request = parse_widget(*fields);
      if (!request)
        return std::unexpected(std::move(request.error()));
      return PluginUiRequest(std::move(*request));
    }
    if (*type == "ui.select")
    {
      auto request = parse_select(*fields);
      if (!request)
        return std::unexpected(std::move(request.error()));
      return PluginUiRequest(std::move(*request));
    }
    auto request = parse_confirm(*fields);
    if (!request)
      return std::unexpected(std::move(request.error()));
    return PluginUiRequest(std::move(*request));
  }();
  if (!parsed)
    return std::unexpected(std::move(parsed.error()));
  if (auto valid = validate_plugin_ui_request(*parsed); !valid)
    return std::unexpected(std::move(valid.error()));

  if (plugin_ui_request_id(*parsed) != *request_id || !state.request_ids_.insert(std::move(*request_id)).second)
    return std::unexpected(ui_protocol_error("plugin UI request is malformed"));
  ++state.record_count_;
  if (*type == "ui.status")
    ++state.status_count_;
  if (*type == "ui.widget")
    ++state.widget_count_;
  if (*type == "ui.select" || *type == "ui.confirm")
    ++state.modal_count_;
  return parsed;
}

ava::core::VoidResult validate_plugin_ui_request(PluginUiRequest const& request)
{
  bool const valid = std::visit(
      [](auto const& value) {
        using T = std::decay_t<decltype(value)>;
        if (!is_valid_plugin_ui_id(value.id))
          return false;
        if constexpr (std::is_same_v<T, PluginUiStatusRequest>)
        {
          return safe_decoded_text(value.text, kPluginUiStatusTextMaxBytes, false);
        }
        else if constexpr (std::is_same_v<T, PluginUiWidgetRequest>)
        {
          if (!safe_decoded_text(value.title, kPluginUiTextComponentMaxBytes, true) || value.lines.empty() || value.lines.size() > kPluginUiWidgetMaxLines)
          {
            return false;
          }
          std::size_t widget_text_bytes = value.title.size();
          for (auto const& line : value.lines)
          {
            if (!safe_decoded_text(line, kPluginUiTextComponentMaxBytes, false) || line.size() > kPluginUiWidgetTextMaxBytes - widget_text_bytes)
              return false;
            widget_text_bytes += line.size();
          }
          return true;
        }
        else if constexpr (std::is_same_v<T, PluginUiSelectRequest>)
        {
          if (!safe_decoded_text(value.title, kPluginUiTextComponentMaxBytes, true) ||
              !safe_decoded_text(value.description, kPluginUiTextComponentMaxBytes, false) || value.choices.empty() ||
              value.choices.size() > kPluginUiChoiceMaxCount)
          {
            return false;
          }
          std::size_t payload_bytes = value.id.size() + value.title.size() + value.description.size();
          if (payload_bytes > kPluginUiModalPayloadMaxBytes)
            return false;
          for (std::size_t index = 0; index < value.choices.size(); ++index)
          {
            auto const& choice = value.choices[index];
            if (!is_valid_plugin_ui_id(choice.id) || !safe_decoded_text(choice.label, kPluginUiTextComponentMaxBytes, true) ||
                (choice.description && !safe_decoded_text(*choice.description, kPluginUiTextComponentMaxBytes, false)))
            {
              return false;
            }
            for (std::size_t previous = 0; previous < index; ++previous)
            {
              if (value.choices[previous].id == choice.id)
                return false;
            }
            auto const choice_bytes = choice.id.size() + choice.label.size() + (choice.description ? choice.description->size() : 0);
            if (choice_bytes > kPluginUiModalPayloadMaxBytes - payload_bytes)
              return false;
            payload_bytes += choice_bytes;
          }
          return true;
        }
        else
        {
          if (!safe_decoded_text(value.title, kPluginUiTextComponentMaxBytes, true) ||
              !safe_decoded_text(value.description, kPluginUiTextComponentMaxBytes, false))
          {
            return false;
          }
          std::size_t const payload_bytes = value.id.size() + value.title.size() + value.description.size();
          return payload_bytes <= kPluginUiModalPayloadMaxBytes;
        }
      },
      request);
  if (!valid)
    return std::unexpected(ui_protocol_error("plugin UI request is malformed"));
  return {};
}

ava::core::VoidResult validate_plugin_ui_action(PluginUiRequest const& request, PluginUiAction const& action)
{
  if (auto valid = validate_plugin_ui_request(request); !valid)
    return std::unexpected(ui_protocol_error("plugin UI action does not match request"));
  if (std::holds_alternative<PluginUiStatusRequest>(request) || std::holds_alternative<PluginUiWidgetRequest>(request))
  {
    if (action.action != PluginUiActionKind::Ack || !action.option_id.empty())
      return std::unexpected(ui_protocol_error("plugin UI action does not match request"));
    return {};
  }
  if (auto const* select = std::get_if<PluginUiSelectRequest>(&request))
  {
    if (action.action == PluginUiActionKind::Cancel && action.option_id.empty())
      return {};
    if (action.action != PluginUiActionKind::Select || !is_valid_plugin_ui_id(action.option_id) ||
        std::ranges::none_of(select->choices, [&](PluginUiChoice const& choice) { return choice.id == action.option_id; }))
    {
      return std::unexpected(ui_protocol_error("plugin UI action does not match request"));
    }
    return {};
  }
  if ((action.action != PluginUiActionKind::Confirm && action.action != PluginUiActionKind::Cancel) || !action.option_id.empty())
    return std::unexpected(ui_protocol_error("plugin UI action does not match request"));
  return {};
}

ava::core::Result<std::string> serialize_plugin_ui_action(std::string_view request_id, PluginUiAction const& action)
{
  if (!is_valid_plugin_ui_id(request_id) || (action.action != PluginUiActionKind::Ack && action.action != PluginUiActionKind::Select &&
                                             action.action != PluginUiActionKind::Confirm && action.action != PluginUiActionKind::Cancel))
    return std::unexpected(ui_protocol_error("plugin UI action is malformed"));
  if ((action.action == PluginUiActionKind::Select && !is_valid_plugin_ui_id(action.option_id)) ||
      (action.action != PluginUiActionKind::Select && !action.option_id.empty()))
  {
    return std::unexpected(ui_protocol_error("plugin UI action is malformed"));
  }
  std::string record =
      "{\"id\":\"" + ava::core::json::escape(request_id) + "\",\"type\":\"ui.action\",\"action\":\"" + std::string(action_name(action.action)) + '"';
  if (action.action == PluginUiActionKind::Select)
    record += ",\"option_id\":\"" + ava::core::json::escape(action.option_id) + '"';
  record += '}';
  return record;
}

}  // namespace ava::plugin
