#include "sys.h"
#include "ava/app/display_settings.h"
#include "ava/tui/theme.h"
#include "ava/core/atomic_file.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::app {
namespace {

std::string lower_ascii(std::string_view text)
{
  std::string lowered;
  lowered.reserve(text.size());
  for (char const ch : text) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return lowered;
}

std::string trim_ascii(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
  return std::string(text);
}

bool is_json_whitespace(char ch) noexcept
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

void skip_json_whitespace(std::string_view text, std::size_t& offset) noexcept
{
  while (offset < text.size() && is_json_whitespace(text[offset])) ++offset;
}

ava::core::Error io_error(std::string message, std::filesystem::path const& path, std::error_code const& error)
{
  auto result = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message)).with_context("path", path.string());
  if (error)
    result.with_context("cause", error.message());
  return result;
}

ava::core::Error invalid_display_error(std::string message, std::filesystem::path const& path, std::string_view field = {})
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message)).with_context("path", path.string());
  if (!field.empty())
    error.with_context("field", std::string(field));
  return error;
}

ava::core::Error invalid_theme_error(std::string message, std::filesystem::path const& path)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message)).with_context("path", path.string());
}

bool valid_custom_theme_name(std::string_view name)
{
  return !name.empty() && name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos &&
         name.find_first_of(" \t\r\n") == std::string_view::npos && name != "." && name != "..";
}

int hex_digit(char ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

std::optional<int> parse_hex_byte(std::string_view value, std::size_t offset)
{
  if (offset + 1 >= value.size())
    return std::nullopt;
  auto const high = hex_digit(value[offset]);
  auto const low = hex_digit(value[offset + 1]);
  if (high < 0 || low < 0)
    return std::nullopt;
  return (high * 16) + low;
}

int squared_distance(int left_red, int left_green, int left_blue, int right_red, int right_green, int right_blue)
{
  auto const red = left_red - right_red;
  auto const green = left_green - right_green;
  auto const blue = left_blue - right_blue;
  return (red * red) + (green * green) + (blue * blue);
}

int xterm_level(int index)
{
  return index == 0 ? 0 : 55 + (index * 40);
}

int nearest_xterm_256(int red, int green, int blue)
{
  auto const cube_index = [](int channel) {
    if (channel < 48)
      return 0;
    if (channel < 115)
      return 1;
    return std::clamp((channel - 35) / 40, 1, 5);
  };
  auto const red_index = cube_index(red);
  auto const green_index = cube_index(green);
  auto const blue_index = cube_index(blue);
  auto const cube_red = xterm_level(red_index);
  auto const cube_green = xterm_level(green_index);
  auto const cube_blue = xterm_level(blue_index);
  auto const cube_color = 16 + (36 * red_index) + (6 * green_index) + blue_index;
  auto const cube_distance = squared_distance(red, green, blue, cube_red, cube_green, cube_blue);

  auto const luminance = ((red * 299) + (green * 587) + (blue * 114)) / 1000;
  auto const gray_index = std::clamp((luminance - 8 + 5) / 10, 0, 23);
  auto const gray_level = 8 + (gray_index * 10);
  auto const gray_color = 232 + gray_index;
  auto const gray_distance = squared_distance(red, green, blue, gray_level, gray_level, gray_level);
  return gray_distance < cube_distance ? gray_color : cube_color;
}

std::optional<int> parse_hex_color(std::string_view value)
{
  if (value.size() != 7 || value.front() != '#')
    return std::nullopt;
  auto const red = parse_hex_byte(value, 1);
  auto const green = parse_hex_byte(value, 3);
  auto const blue = parse_hex_byte(value, 5);
  if (!red || !green || !blue)
    return std::nullopt;
  return nearest_xterm_256(*red, *green, *blue);
}

std::string revision_for_text(std::string_view text)
{
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char const ch : text)
  {
    hash ^= ch;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << text.size() << ':' << hash;
  return out.str();
}

ava::core::Result<int> parse_theme_color_field(std::string_view object, std::string_view key, std::string_view vars, std::filesystem::path const& path,
                                               int depth);

ava::core::Result<int> parse_theme_color_string(std::string_view value, std::string_view vars, std::filesystem::path const& path, int depth)
{
  if (value.empty())
    return -1;
  if (auto const hex = parse_hex_color(value))
    return *hex;
  if (depth > 8)
  {
    return std::unexpected(invalid_theme_error("custom TUI theme color variable cycle", path).with_context("variable", std::string(value)));
  }
  if (vars.empty())
  {
    return std::unexpected(invalid_theme_error("custom TUI theme color variable is not defined", path).with_context("variable", std::string(value)));
  }
  auto resolved = parse_theme_color_field(vars, value, vars, path, depth + 1);
  if (!resolved)
  {
    return std::unexpected(invalid_theme_error("custom TUI theme color variable is not defined", path).with_context("variable", std::string(value)));
  }
  return resolved;
}

ava::core::Result<int> parse_theme_color_field(std::string_view object, std::string_view key, std::string_view vars, std::filesystem::path const& path,
                                               int depth)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
  {
    return std::unexpected(invalid_theme_error("custom TUI theme is missing a required color", path).with_context("color", std::string(key)));
  }
  if (*start < object.size() && object[*start] == '"')
  {
    auto value = ava::core::json::string_field(object, key);
    if (!value)
    {
      return std::unexpected(invalid_theme_error("custom TUI theme color must be a string or 0-255 integer", path).with_context("color", std::string(key)));
    }
    return parse_theme_color_string(*value, vars, path, depth);
  }
  auto integer = ava::core::json::integer_field(object, key);
  if (!integer || *integer < 0 || *integer > 255)
  {
    return std::unexpected(invalid_theme_error("custom TUI theme color must be a string or 0-255 integer", path).with_context("color", std::string(key)));
  }
  return static_cast<int>(*integer);
}

ava::core::Result<int> parse_required_theme_color(std::string_view colors, std::string_view key, std::string_view vars, std::filesystem::path const& path)
{
  auto parsed = parse_theme_color_field(colors, key, vars, path, 0);
  if (!parsed)
    return std::unexpected(std::move(parsed.error()));
  return *parsed;
}

ava::core::Result<int> parse_optional_theme_color(std::string_view colors, std::string_view key, std::string_view vars, std::filesystem::path const& path,
                                                  int fallback)
{
  if (!ava::core::json::field_value_start(colors, key))
    return fallback;
  return parse_theme_color_field(colors, key, vars, path, 0);
}

ava::core::Result<ava::tui::TuiThemePalette> parse_custom_theme_palette(std::string_view json, std::filesystem::path const& path)
{
  auto const colors = ava::core::json::object_field(json, "colors");
  if (!colors)
    return std::unexpected(invalid_theme_error("custom TUI theme is missing colors", path));
  auto const vars = ava::core::json::object_field(json, "vars").value_or(std::string{});

  ava::tui::TuiThemePalette palette;
  auto text = parse_required_theme_color(*colors, "text", vars, path);
  auto muted = parse_required_theme_color(*colors, "muted", vars, path);
  auto success = parse_required_theme_color(*colors, "success", vars, path);
  auto warning = parse_required_theme_color(*colors, "warning", vars, path);
  auto error = parse_required_theme_color(*colors, "error", vars, path);
  auto accent = parse_required_theme_color(*colors, "accent", vars, path);
  auto screen_bg = parse_required_theme_color(*colors, "screenBg", vars, path);
  auto composer_bg = parse_required_theme_color(*colors, "composerBg", vars, path);
  if (!text)
    return std::unexpected(std::move(text.error()));
  if (!muted)
    return std::unexpected(std::move(muted.error()));
  if (!success)
    return std::unexpected(std::move(success.error()));
  if (!warning)
    return std::unexpected(std::move(warning.error()));
  if (!error)
    return std::unexpected(std::move(error.error()));
  if (!accent)
    return std::unexpected(std::move(accent.error()));
  if (!screen_bg)
    return std::unexpected(std::move(screen_bg.error()));
  if (!composer_bg)
    return std::unexpected(std::move(composer_bg.error()));
  auto tool_bg = parse_optional_theme_color(*colors, "toolBg", vars, path, *composer_bg);
  if (!tool_bg)
    return std::unexpected(std::move(tool_bg.error()));
  auto question_bg = parse_optional_theme_color(*colors, "questionBg", vars, path, *composer_bg);
  if (!question_bg)
    return std::unexpected(std::move(question_bg.error()));
  palette.text = *text;
  palette.muted = *muted;
  palette.success = *success;
  palette.warning = *warning;
  palette.error = *error;
  palette.accent = *accent;
  palette.screen_bg = *screen_bg;
  palette.composer_bg = *composer_bg;
  palette.tool_bg = *tool_bg;
  palette.question_bg = *question_bg;
  return palette;
}

struct DisplayJsonEntry
{
  std::string key;
  std::string raw_value;
};

std::optional<std::size_t> json_string_literal_end(std::string_view text, std::size_t start)
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

std::optional<std::size_t> json_balanced_value_end(std::string_view text, std::size_t start)
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

std::optional<std::size_t> json_value_end(std::string_view text, std::size_t start)
{
  if (start >= text.size())
    return std::nullopt;
  if (text[start] == '"')
  {
    auto const end = json_string_literal_end(text, start);
    if (!end)
      return std::nullopt;
    return *end + 1;
  }
  if (text[start] == '{' || text[start] == '[')
    return json_balanced_value_end(text, start);

  auto end = start;
  while (end < text.size() && text[end] != ',' && text[end] != '}') ++end;
  while (end > start && is_json_whitespace(text[end - 1])) --end;
  return end > start ? std::optional<std::size_t>(end) : std::nullopt;
}

std::optional<std::vector<DisplayJsonEntry>> top_level_display_entries(std::string_view object)
{
  if (!ava::core::json::is_valid_object(object))
    return std::nullopt;

  std::vector<DisplayJsonEntry> entries;
  std::size_t offset = 0;
  skip_json_whitespace(object, offset);
  if (offset >= object.size() || object[offset] != '{')
    return std::nullopt;
  ++offset;
  skip_json_whitespace(object, offset);
  if (offset < object.size() && object[offset] == '}')
    return entries;

  while (offset < object.size())
  {
    skip_json_whitespace(object, offset);
    auto const key_start = offset;
    auto const key_end = json_string_literal_end(object, key_start);
    if (!key_end)
      return std::nullopt;
    auto raw_key = std::string(object.substr(key_start, *key_end - key_start + 1));
    auto const decoded_key = ava::core::json::string_field("{\"value\":" + raw_key + "}", "value");
    if (!decoded_key)
      return std::nullopt;
    offset = *key_end + 1;
    skip_json_whitespace(object, offset);
    if (offset >= object.size() || object[offset] != ':')
      return std::nullopt;
    ++offset;
    skip_json_whitespace(object, offset);
    auto const value_start = offset;
    auto const value_end = json_value_end(object, value_start);
    if (!value_end)
      return std::nullopt;
    auto raw_value = std::string(object.substr(value_start, *value_end - value_start));
    entries.push_back(DisplayJsonEntry{.key = *decoded_key, .raw_value = std::move(raw_value)});
    offset = *value_end;
    skip_json_whitespace(object, offset);
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

ava::core::Result<std::string> decode_json_string_value(std::string_view raw_value, std::filesystem::path const& path, std::string_view field)
{
  if (raw_value.empty() || raw_value.front() != '"')
    return std::unexpected(invalid_display_error(std::string(field) + " must be a JSON string", path, field));
  auto decoded = ava::core::json::string_field(std::string("{\"value\":") + std::string(raw_value) + "}", "value");
  if (!decoded)
    return std::unexpected(invalid_display_error(std::string(field) + " must be a JSON string", path, field));
  return std::move(*decoded);
}

ava::core::Result<bool> decode_json_bool_value(std::string_view raw_value, std::filesystem::path const& path, std::string_view field)
{
  if (raw_value == "true")
    return true;
  if (raw_value == "false")
    return false;
  return std::unexpected(invalid_display_error(std::string(field) + " must be a JSON boolean", path, field));
}

ava::core::Result<std::vector<std::string>> decode_mermaid_argv(std::string_view raw_value, std::filesystem::path const& path)
{
  if (raw_value.empty() || raw_value.front() != '[')
    return std::unexpected(invalid_display_error("mermaid.argv must be an array of strings", path, "mermaid.argv"));

  std::vector<std::string> argv;
  std::size_t total_bytes = 0;
  std::size_t offset = 1;
  skip_json_whitespace(raw_value, offset);
  if (offset < raw_value.size() && raw_value[offset] == ']')
  {
    ++offset;
    skip_json_whitespace(raw_value, offset);
    if (offset == raw_value.size())
      return argv;
  }

  while (offset < raw_value.size())
  {
    if (argv.size() == kMaxMermaidArgCount || raw_value[offset] != '"')
      return std::unexpected(invalid_display_error("mermaid.argv must contain at most 32 strings", path, "mermaid.argv"));
    auto const literal_end = json_string_literal_end(raw_value, offset);
    if (!literal_end)
      return std::unexpected(invalid_display_error("mermaid.argv must be an array of strings", path, "mermaid.argv"));
    auto decoded = decode_json_string_value(raw_value.substr(offset, *literal_end - offset + 1), path, "mermaid.argv");
    if (!decoded)
      return std::unexpected(std::move(decoded.error()));
    if (decoded->size() > kMaxMermaidArgBytes || decoded->find('\0') != std::string::npos || total_bytes > kMaxMermaidArgvBytes - decoded->size())
    {
      return std::unexpected(invalid_display_error("mermaid.argv exceeds its argument byte limits", path, "mermaid.argv"));
    }
    total_bytes += decoded->size();
    argv.push_back(std::move(*decoded));
    offset = *literal_end + 1;
    skip_json_whitespace(raw_value, offset);
    if (offset < raw_value.size() && raw_value[offset] == ',')
    {
      ++offset;
      skip_json_whitespace(raw_value, offset);
      continue;
    }
    if (offset < raw_value.size() && raw_value[offset] == ']')
    {
      ++offset;
      skip_json_whitespace(raw_value, offset);
      if (offset == raw_value.size())
        break;
    }
    return std::unexpected(invalid_display_error("mermaid.argv must be an array of strings", path, "mermaid.argv"));
  }

  if (!argv.empty() && !std::filesystem::path(argv.front()).is_absolute())
    return std::unexpected(invalid_display_error("mermaid.argv[0] must be an absolute path", path, "mermaid.argv"));
  return argv;
}

ava::core::Result<MermaidDisplaySettings> decode_mermaid_settings(std::string_view raw_value, std::filesystem::path const& path)
{
  auto entries = top_level_display_entries(raw_value);
  if (!entries)
    return std::unexpected(invalid_display_error("mermaid must be a JSON object", path, "mermaid"));

  MermaidDisplaySettings settings;
  for (auto const& entry : *entries)
  {
    if (entry.key == "enabled")
    {
      if (settings.enabled_configured)
        return std::unexpected(invalid_display_error("duplicate mermaid.enabled field", path, "mermaid.enabled"));
      auto enabled = decode_json_bool_value(entry.raw_value, path, "mermaid.enabled");
      if (!enabled)
        return std::unexpected(std::move(enabled.error()));
      settings.enabled = *enabled;
      settings.enabled_configured = true;
      continue;
    }
    if (entry.key == "argv")
    {
      if (settings.argv_configured)
        return std::unexpected(invalid_display_error("duplicate mermaid.argv field", path, "mermaid.argv"));
      auto argv = decode_mermaid_argv(entry.raw_value, path);
      if (!argv)
        return std::unexpected(std::move(argv.error()));
      settings.argv = std::move(*argv);
      settings.argv_configured = true;
      continue;
    }
    settings.unknown_fields.emplace_back(entry.key, entry.raw_value);
  }
  if (settings.enabled && settings.argv.empty())
    return std::unexpected(invalid_display_error("enabled mermaid rendering requires nonempty argv", path, "mermaid.argv"));
  return settings;
}

ava::core::Result<std::size_t> decode_image_width_value(std::string_view raw_value, std::filesystem::path const& path)
{
  if (raw_value.empty() || raw_value.front() == '"' || raw_value.front() == '{' || raw_value.front() == '[' || raw_value == "true" || raw_value == "false" ||
      raw_value == "null")
  {
    return std::unexpected(invalid_display_error("image_width_cells must be an integer between 8 and 160", path, "image_width_cells"));
  }

  std::size_t index = 0;
  if (raw_value[index] == '+')
    ++index;
  if (index < raw_value.size() && raw_value[index] == '-')
  {
    return std::unexpected(invalid_display_error("image_width_cells must be an integer between 8 and 160", path, "image_width_cells"));
  }
  if (index >= raw_value.size() || std::isdigit(static_cast<unsigned char>(raw_value[index])) == 0)
  {
    return std::unexpected(invalid_display_error("image_width_cells must be an integer between 8 and 160", path, "image_width_cells"));
  }
  unsigned long long value = 0;
  bool saw_digit = false;
  while (index < raw_value.size() && std::isdigit(static_cast<unsigned char>(raw_value[index])) != 0)
  {
    saw_digit = true;
    auto const digit = static_cast<unsigned long long>(raw_value[index] - '0');
    if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10ULL)
    {
      return std::unexpected(invalid_display_error("image_width_cells must be an integer between 8 and 160", path, "image_width_cells"));
    }
    value = (value * 10ULL) + digit;
    ++index;
  }
  if (!saw_digit || index != raw_value.size() || raw_value.find('.') != std::string_view::npos || raw_value.find('e') != std::string_view::npos ||
      raw_value.find('E') != std::string_view::npos)
  {
    return std::unexpected(invalid_display_error("image_width_cells must be an integer between 8 and 160", path, "image_width_cells"));
  }
  if (value < kMinTuiImageWidthCells || value > kMaxTuiImageWidthCells)
  {
    return std::unexpected(invalid_display_error("image_width_cells must be an integer between 8 and 160", path, "image_width_cells")
                               .with_context("value", std::to_string(value)));
  }
  return static_cast<std::size_t>(value);
}

ava::core::Result<DisplaySettingsDocument> parse_display_settings_document(std::string_view json, std::filesystem::path path)
{
  if (!ava::core::json::is_valid_object(json))
    return std::unexpected(invalid_display_error("invalid TUI display settings JSON", path));

  auto entries = top_level_display_entries(json);
  if (!entries)
    return std::unexpected(invalid_display_error("invalid TUI display settings JSON object", path));

  DisplaySettingsDocument document{.theme = std::nullopt,
                                   .show_images = std::nullopt,
                                   .image_width_cells = std::nullopt,
                                   .cursor_style = std::nullopt,
                                   .cursor_blink = std::nullopt,
                                   .mermaid = std::nullopt,
                                   .unknown_fields = {},
                                   .path = std::move(path)};
  bool saw_theme = false;
  bool saw_show_images = false;
  bool saw_image_width = false;
  bool saw_cursor_style = false;
  bool saw_cursor_blink = false;
  bool saw_mermaid = false;
  for (auto const& entry : *entries)
  {
    if (entry.key == "theme")
    {
      if (saw_theme)
        return std::unexpected(invalid_display_error("duplicate theme field", document.path, "theme"));
      saw_theme = true;
      auto decoded = decode_json_string_value(entry.raw_value, document.path, "theme");
      if (!decoded)
        return std::unexpected(std::move(decoded.error()));
      if (!decoded->empty())
        document.theme = std::move(*decoded);
      continue;
    }
    if (entry.key == "show_images")
    {
      if (saw_show_images)
        return std::unexpected(invalid_display_error("duplicate show_images field", document.path, "show_images"));
      saw_show_images = true;
      auto decoded = decode_json_bool_value(entry.raw_value, document.path, "show_images");
      if (!decoded)
        return std::unexpected(std::move(decoded.error()));
      document.show_images = *decoded;
      continue;
    }
    if (entry.key == "image_width_cells")
    {
      if (saw_image_width)
        return std::unexpected(invalid_display_error("duplicate image_width_cells field", document.path, "image_width_cells"));
      saw_image_width = true;
      auto decoded = decode_image_width_value(entry.raw_value, document.path);
      if (!decoded)
        return std::unexpected(std::move(decoded.error()));
      document.image_width_cells = *decoded;
      continue;
    }
    if (entry.key == "cursor_style")
    {
      if (saw_cursor_style)
        return std::unexpected(invalid_display_error("duplicate cursor_style field", document.path, "cursor_style"));
      saw_cursor_style = true;
      auto decoded = decode_json_string_value(entry.raw_value, document.path, "cursor_style");
      if (!decoded)
        return std::unexpected(std::move(decoded.error()));
      auto style = normalize_tui_cursor_style_setting(*decoded);
      if (!style || tui_cursor_style_name(*style) != *decoded)
        return std::unexpected(invalid_display_error("cursor_style must be default, block, underline, or bar", document.path, "cursor_style"));
      document.cursor_style = *style;
      continue;
    }
    if (entry.key == "cursor_blink")
    {
      if (saw_cursor_blink)
        return std::unexpected(invalid_display_error("duplicate cursor_blink field", document.path, "cursor_blink"));
      saw_cursor_blink = true;
      auto decoded = decode_json_bool_value(entry.raw_value, document.path, "cursor_blink");
      if (!decoded)
        return std::unexpected(std::move(decoded.error()));
      document.cursor_blink = *decoded;
      continue;
    }
    if (entry.key == "mermaid")
    {
      if (saw_mermaid)
        return std::unexpected(invalid_display_error("duplicate mermaid field", document.path, "mermaid"));
      saw_mermaid = true;
      auto decoded = decode_mermaid_settings(entry.raw_value, document.path);
      if (!decoded)
        return std::unexpected(std::move(decoded.error()));
      document.mermaid = std::move(*decoded);
      continue;
    }
    document.unknown_fields.emplace_back(entry.key, entry.raw_value);
  }
  return document;
}

std::string serialize_mermaid_settings(MermaidDisplaySettings const& settings)
{
  std::vector<DisplayJsonEntry> entries;
  entries.reserve(2 + settings.unknown_fields.size());
  if (settings.enabled_configured)
    entries.push_back(DisplayJsonEntry{.key = "enabled", .raw_value = settings.enabled ? "true" : "false"});
  if (settings.argv_configured)
  {
    std::string argv = "[";
    for (std::size_t index = 0; index < settings.argv.size(); ++index)
    {
      if (index != 0)
        argv += ", ";
      argv += '"';
      argv += ava::core::json::escape(settings.argv[index]);
      argv += '"';
    }
    argv += ']';
    entries.push_back(DisplayJsonEntry{.key = "argv", .raw_value = std::move(argv)});
  }
  for (auto const& unknown : settings.unknown_fields) entries.push_back(DisplayJsonEntry{.key = unknown.first, .raw_value = unknown.second});

  std::string out = "{";
  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    if (index != 0)
      out += ", ";
    out += '"';
    out += ava::core::json::escape(entries[index].key);
    out += "\": ";
    out += entries[index].raw_value;
  }
  out += '}';
  return out;
}

std::string serialize_display_settings_document(DisplaySettingsDocument const& document)
{
  std::vector<DisplayJsonEntry> entries;
  entries.reserve(6 + document.unknown_fields.size());
  if (document.theme)
    entries.push_back(DisplayJsonEntry{.key = "theme", .raw_value = std::string("\"") + ava::core::json::escape(*document.theme) + "\""});
  if (document.show_images)
    entries.push_back(DisplayJsonEntry{.key = "show_images", .raw_value = *document.show_images ? "true" : "false"});
  if (document.image_width_cells)
    entries.push_back(DisplayJsonEntry{.key = "image_width_cells", .raw_value = std::to_string(*document.image_width_cells)});
  if (document.cursor_style)
    entries.push_back(
        DisplayJsonEntry{.key = "cursor_style", .raw_value = std::string("\"") + std::string(tui_cursor_style_name(*document.cursor_style)) + "\""});
  if (document.cursor_blink)
    entries.push_back(DisplayJsonEntry{.key = "cursor_blink", .raw_value = *document.cursor_blink ? "true" : "false"});
  if (document.mermaid)
    entries.push_back(DisplayJsonEntry{.key = "mermaid", .raw_value = serialize_mermaid_settings(*document.mermaid)});
  for (auto const& unknown : document.unknown_fields) entries.push_back(DisplayJsonEntry{.key = unknown.first, .raw_value = unknown.second});

  if (entries.empty())
    return "{\n}\n";
  std::string out = "{\n";
  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    out += "  \"";
    out += ava::core::json::escape(entries[index].key);
    out += "\": ";
    out += entries[index].raw_value;
    if (index + 1 < entries.size())
      out += ',';
    out += '\n';
  }
  out += "}\n";
  return out;
}

ava::core::Result<std::string> read_display_settings_text(std::filesystem::path const& path)
{
  std::error_code status_error;
  if (!std::filesystem::is_regular_file(path, status_error))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "TUI display settings is not a regular file").with_context("path", path.string());
    if (status_error)
      error.with_context("cause", status_error.message());
    return std::unexpected(std::move(error));
  }
  std::error_code size_error;
  auto const size = std::filesystem::file_size(path, size_error);
  if (size_error || size > kMaxTuiDisplaySettingsBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "TUI display settings is too large")
                     .with_context("path", path.string())
                     .with_context("max_bytes", std::to_string(kMaxTuiDisplaySettingsBytes));
    if (size_error)
      error.with_context("cause", size_error.message());
    return std::unexpected(std::move(error));
  }

  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read TUI display settings").with_context("path", path.string()));

  std::string content;
  content.reserve(static_cast<std::size_t>(size));
  std::array<char, 4096> buffer{};
  while (input)
  {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (input.gcount() > 0)
      content.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
    if (content.size() > kMaxTuiDisplaySettingsBytes)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "TUI display settings is too large")
                                 .with_context("path", path.string())
                                 .with_context("max_bytes", std::to_string(kMaxTuiDisplaySettingsBytes)));
    }
  }
  if (!input.eof() && input.fail())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed while reading TUI display settings").with_context("path", path.string()));
  return content;
}

ava::core::Result<DisplaySettingsDocument> load_or_default_document(ava::config::XdgPaths const& paths)
{
  auto const path = tui_display_settings_file(paths);
  std::error_code exists_error;
  auto const exists = std::filesystem::exists(path, exists_error);
  if (exists_error)
    return std::unexpected(io_error("failed to inspect TUI display settings", path, exists_error));
  if (!exists)
    return DisplaySettingsDocument{.theme = std::nullopt,
                                   .show_images = std::nullopt,
                                   .image_width_cells = std::nullopt,
                                   .cursor_style = std::nullopt,
                                   .cursor_blink = std::nullopt,
                                   .mermaid = std::nullopt,
                                   .unknown_fields = {},
                                   .path = path};

  auto content = read_display_settings_text(path);
  if (!content)
    return std::unexpected(std::move(content.error()));
  return parse_display_settings_document(*content, path);
}

// Image/document fields only. Theme name resolution is applied by the callers that own discovery.
TuiDisplaySettings display_settings_from_document_fields(DisplaySettingsDocument const& document)
{
  TuiDisplaySettings settings{.theme = std::nullopt,
                              .custom_theme = std::nullopt,
                              .show_images = true,
                              .image_width_cells = kDefaultTuiImageWidthCells,
                              .show_images_configured = false,
                              .image_width_configured = false,
                              .cursor = ava::tui::terminal::CursorSettings{ava::tui::terminal::CursorStyle::Default},
                              .cursor_style_configured = false,
                              .cursor_blink_configured = false,
                              .mermaid = document.mermaid.value_or(MermaidDisplaySettings{}),
                              .path = document.path};
  settings.show_images = document.show_images.value_or(true);
  settings.image_width_cells = document.image_width_cells.value_or(kDefaultTuiImageWidthCells);
  settings.show_images_configured = document.show_images.has_value();
  settings.image_width_configured = document.image_width_cells.has_value();
  settings.cursor =
      ava::tui::terminal::CursorSettings{document.cursor_style.value_or(ava::tui::terminal::CursorStyle::Default), document.cursor_blink.value_or(true)};
  settings.cursor_style_configured = document.cursor_style.has_value();
  settings.cursor_blink_configured = document.cursor_blink.has_value();
  return settings;
}

ava::core::Error invalid_display_theme_error(DisplaySettingsDocument const& document)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid TUI display theme")
      .with_context("path", document.path.string())
      .with_context("theme", document.theme.value_or(std::string{}))
      .with_context("supported", "dark, light, plain, or a valid theme name under themes/*.json");
}

ava::core::VoidResult store_display_settings_document(DisplaySettingsDocument const& document)
{
  return ava::core::write_text_file_atomic(document.path, serialize_display_settings_document(document), "TUI display settings");
}

ava::core::Result<std::optional<std::string>> resolve_theme_name_for_store(ava::config::XdgPaths const& paths, std::optional<std::string> theme,
                                                                           std::filesystem::path const& path)
{
  if (!theme)
    return std::optional<std::string>{};
  if (auto normalized = normalize_tui_theme_setting(*theme))
    return normalized;
  auto custom = load_tui_custom_theme(paths, *theme);
  if (!custom)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid TUI display theme")
                               .with_context("path", path.string())
                               .with_context("theme", *theme)
                               .with_context("supported", "dark, light, plain, or a valid theme name under themes/*.json"));
  }
  return custom->name;
}

}  // namespace

std::filesystem::path tui_display_settings_file(ava::config::XdgPaths const& paths)
{
  return paths.ava_config_dir / "display.json";
}

std::filesystem::path tui_theme_directory(ava::config::XdgPaths const& paths)
{
  return paths.ava_config_dir / "themes";
}

std::optional<std::string> normalize_tui_theme_setting(std::string_view value)
{
  auto const normalized = lower_ascii(trim_ascii(value));
  if (normalized == "dark" || normalized == "ava-dark")
    return "dark";
  if (normalized == "light" || normalized == "ava-light")
    return "light";
  if (normalized == "plain" || normalized == "none" || normalized == "no-color" || normalized == "no_color")
    return "plain";
  return std::nullopt;
}

bool is_tui_theme_reset_value(std::string_view value)
{
  auto const normalized = lower_ascii(trim_ascii(value));
  return normalized == "reset" || normalized == "default" || normalized == "auto";
}

std::string tui_theme_setting_usage()
{
  return "usage: /theme [dark|light|plain|custom-name|reset]";
}

std::optional<bool> normalize_tui_show_images_setting(std::string_view value)
{
  auto const normalized = lower_ascii(trim_ascii(value));
  if (normalized == "on" || normalized == "true" || normalized == "yes" || normalized == "enable" || normalized == "enabled" || normalized == "1")
    return true;
  if (normalized == "off" || normalized == "false" || normalized == "no" || normalized == "disable" || normalized == "disabled" || normalized == "0")
    return false;
  return std::nullopt;
}

bool is_tui_show_images_reset_value(std::string_view value)
{
  auto const normalized = lower_ascii(trim_ascii(value));
  return normalized == "reset" || normalized == "default" || normalized == "auto";
}

std::string tui_show_images_setting_usage()
{
  return "usage: /images [on|off|reset]";
}

std::optional<std::size_t> normalize_tui_image_width_setting(std::string_view value)
{
  auto const trimmed = trim_ascii(value);
  if (trimmed.empty())
    return std::nullopt;
  std::size_t index = 0;
  if (trimmed[index] == '+')
    ++index;
  if (index >= trimmed.size() || std::isdigit(static_cast<unsigned char>(trimmed[index])) == 0)
    return std::nullopt;
  unsigned long long width = 0;
  while (index < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[index])) != 0)
  {
    auto const digit = static_cast<unsigned long long>(trimmed[index] - '0');
    if (width > (std::numeric_limits<unsigned long long>::max() - digit) / 10ULL)
      return std::nullopt;
    width = (width * 10ULL) + digit;
    ++index;
  }
  if (index != trimmed.size() || width < kMinTuiImageWidthCells || width > kMaxTuiImageWidthCells)
    return std::nullopt;
  return static_cast<std::size_t>(width);
}

bool is_tui_image_width_reset_value(std::string_view value)
{
  auto const normalized = lower_ascii(trim_ascii(value));
  return normalized == "reset" || normalized == "default" || normalized == "auto";
}

std::string tui_image_width_setting_usage()
{
  return "usage: /image-width <8..160>|reset";
}

std::optional<ava::tui::terminal::CursorStyle> normalize_tui_cursor_style_setting(std::string_view value)
{
  auto const normalized = lower_ascii(trim_ascii(value));
  if (normalized == "default")
    return ava::tui::terminal::CursorStyle::Default;
  if (normalized == "block")
    return ava::tui::terminal::CursorStyle::Block;
  if (normalized == "underline")
    return ava::tui::terminal::CursorStyle::Underline;
  if (normalized == "bar")
    return ava::tui::terminal::CursorStyle::Bar;
  return std::nullopt;
}

std::optional<bool> normalize_tui_cursor_blink_setting(std::string_view value)
{
  auto const normalized = lower_ascii(trim_ascii(value));
  if (normalized == "blink")
    return true;
  if (normalized == "steady")
    return false;
  return std::nullopt;
}

std::string_view tui_cursor_style_name(ava::tui::terminal::CursorStyle style) noexcept
{
  switch (style)
  {
    case ava::tui::terminal::CursorStyle::Default:
      return "default";
    case ava::tui::terminal::CursorStyle::Block:
      return "block";
    case ava::tui::terminal::CursorStyle::Underline:
      return "underline";
    case ava::tui::terminal::CursorStyle::Bar:
      return "bar";
  }
  return "default";
}

std::string tui_cursor_setting_usage()
{
  return "usage: /cursor default|block|underline|bar [blink|steady]";
}

std::string active_tui_theme_summary()
{
  auto const active = ava::tui::active_tui_theme();
  return active.name + " (" + active.badge + ")";
}

namespace {

class ScopedThemeFd
{
 public:
  explicit ScopedThemeFd(int fd) noexcept : fd_(fd) { }
  ScopedThemeFd(ScopedThemeFd const&) = delete;
  ScopedThemeFd& operator=(ScopedThemeFd const&) = delete;
  ScopedThemeFd(ScopedThemeFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }
  ScopedThemeFd& operator=(ScopedThemeFd&& other) noexcept
  {
    if (this != &other)
    {
      close_if_open();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~ScopedThemeFd() { close_if_open(); }

  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  void close_if_open() noexcept
  {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
  }

  int fd_ = -1;
};

std::string errno_message()
{
  return std::generic_category().message(errno);
}

// Lexicographic path key used for deterministic discovery/watch ordering.
std::string normalized_theme_path_key(std::filesystem::path const& path)
{
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error)
    absolute = path;
  return absolute.lexically_normal().generic_string();
}

bool theme_filename_is_json_candidate(std::filesystem::path const& path)
{
  return path.extension() == ".json" && !path.filename().empty() && path.filename() != "." && path.filename() != "..";
}

// One-descriptor open+fstat+bounded-read path. Caller supplies the effective read budget
// (min of per-file cap and remaining aggregate). Never reads more than max_bytes. Distinguishes
// an exact-cap complete file from truncated/oversized content via pre- and post-read fstat only
// (no post-budget probe byte). bytes_read is always the physical byte count actually pulled.
struct BoundedThemeTextRead
{
  enum class Status : std::uint8_t
  {
    Complete,
    Truncated,
  };

  std::string content;
  std::size_t bytes_read = 0;
  Status status = Status::Complete;
};

ava::core::Result<BoundedThemeTextRead> read_custom_theme_text_bounded(std::filesystem::path const& path, std::size_t max_bytes)
{
  if (max_bytes == 0)
    return BoundedThemeTextRead{.content = {}, .bytes_read = 0, .status = BoundedThemeTextRead::Status::Truncated};

  int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  ScopedThemeFd fd(::open(path.c_str(), flags));
  if (fd.get() < 0)
  {
    auto category = (errno == EACCES || errno == EPERM || errno == ELOOP) ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
    return std::unexpected(
        ava::core::Error(category, "failed to open TUI custom theme").with_context("path", path.string()).with_context("cause", errno_message()));
  }

  struct stat pre_status{};
  if (::fstat(fd.get(), &pre_status) != 0)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect TUI custom theme descriptor")
                               .with_context("path", path.string())
                               .with_context("cause", errno_message()));
  }
  if (!S_ISREG(pre_status.st_mode))
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "TUI custom theme must be a regular file").with_context("path", path.string()));
  }
  if (pre_status.st_size < 0)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect TUI custom theme descriptor")
                               .with_context("path", path.string())
                               .with_context("cause", "negative size"));
  }
  auto const pre_size = static_cast<std::uintmax_t>(pre_status.st_size);
  // Known oversized from pre-read metadata: reject without pulling content. bytes_read stays 0.
  if (pre_size > static_cast<std::uintmax_t>(max_bytes))
    return BoundedThemeTextRead{.content = {}, .bytes_read = 0, .status = BoundedThemeTextRead::Status::Truncated};

#ifndef O_NOFOLLOW
  struct stat path_status{};
  if (::lstat(path.c_str(), &path_status) != 0)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect TUI custom theme")
                               .with_context("path", path.string())
                               .with_context("cause", errno_message()));
  }
  if (S_ISLNK(path_status.st_mode) || path_status.st_dev != pre_status.st_dev || path_status.st_ino != pre_status.st_ino)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "TUI custom theme must not be a symlink").with_context("path", path.string()));
  }
#endif

  BoundedThemeTextRead result;
  result.content.reserve(static_cast<std::size_t>(pre_size));
  std::array<char, 4096> buffer{};
  for (;;)
  {
    auto const remaining_budget = max_bytes - result.content.size();
    if (remaining_budget == 0)
      break;
    auto const want = static_cast<std::size_t>(std::min<std::size_t>(buffer.size(), remaining_budget));
    auto const count = ::read(fd.get(), buffer.data(), want);
    if (count == 0)
      break;
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed while reading TUI custom theme")
                                 .with_context("path", path.string())
                                 .with_context("cause", errno_message()));
    }
    result.content.append(buffer.data(), static_cast<std::size_t>(count));
  }
  // Physical work actually performed — retained even when the candidate is later classified truncated.
  result.bytes_read = result.content.size();

  struct stat post_status{};
  if (::fstat(fd.get(), &post_status) != 0)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect TUI custom theme descriptor")
                               .with_context("path", path.string())
                               .with_context("cause", errno_message()));
  }
  if (!S_ISREG(post_status.st_mode) || post_status.st_size < 0)
  {
    result.content.clear();
    result.status = BoundedThemeTextRead::Status::Truncated;
    return result;
  }
  auto const post_size = static_cast<std::uintmax_t>(post_status.st_size);
  auto const read_size = static_cast<std::uintmax_t>(result.bytes_read);

  // Classify complete vs truncated from descriptor metadata only — never read past max_bytes.
  // Growth/shrink/inconsistent descriptor metadata is handled conservatively: any disagreement
  // among pre_size, post_size, and bytes_read fails closed as Truncated while still reporting
  // the bytes physically read.
  if (result.bytes_read < max_bytes)
  {
    // Hit EOF before filling the budget. Complete only when pre/post/read sizes all agree.
    bool const stable_below_cap = pre_size == post_size && post_size == read_size;
    result.status = stable_below_cap ? BoundedThemeTextRead::Status::Complete : BoundedThemeTextRead::Status::Truncated;
  }
  else
  {
    // Filled the budget. Exact-cap complete requires stable pre/post metadata equal to max_bytes.
    bool const exact_stable_cap = pre_size == static_cast<std::uintmax_t>(max_bytes) && post_size == static_cast<std::uintmax_t>(max_bytes);
    result.status = exact_stable_cap ? BoundedThemeTextRead::Status::Complete : BoundedThemeTextRead::Status::Truncated;
  }

  if (result.status == BoundedThemeTextRead::Status::Truncated)
    result.content.clear();
  return result;
}

ava::core::Result<ava::tui::TuiCustomTheme> parse_custom_theme_text(std::string json, std::filesystem::path const& path)
{
  if (!ava::core::json::is_valid_object(json))
    return std::unexpected(invalid_theme_error("invalid TUI custom theme JSON", path));

  auto name = ava::core::json::string_field(json, "name");
  if (!name || !valid_custom_theme_name(*name))
  {
    return std::unexpected(invalid_theme_error("custom TUI theme name must be non-empty and cannot contain whitespace or path separators", path));
  }
  if (normalize_tui_theme_setting(*name) || is_tui_theme_reset_value(*name))
  {
    return std::unexpected(invalid_theme_error("custom TUI theme name conflicts with a built-in theme", path).with_context("theme", *name));
  }

  auto palette = parse_custom_theme_palette(json, path);
  if (!palette)
    return std::unexpected(std::move(palette.error()));
  return ava::tui::TuiCustomTheme{.name = std::move(*name), .path = path, .palette = std::move(*palette), .revision = revision_for_text(json)};
}

struct ThemeCandidatePath
{
  std::filesystem::path path;
  std::string order_key;
};

struct ThemeCandidateCollection
{
  std::vector<ThemeCandidatePath> candidates;
  // True when more regular .json candidates existed than the bounded read/result cap.
  bool exceeded_cap = false;
};

// Collect non-symlink .json candidates under themes/, ordered by normalized path.
// Symlinks/special files are skipped without opening. Cap is applied after sort; exceeded_cap
// records that uniqueness-sensitive lookup must fail closed because later files were not read.
ThemeCandidateCollection collect_custom_theme_candidate_paths(std::filesystem::path const& dir)
{
  ThemeCandidateCollection collection;
  std::error_code exists_error;
  if (!std::filesystem::exists(dir, exists_error) || exists_error)
    return collection;

  std::error_code iter_error;
  auto const options = std::filesystem::directory_options::skip_permission_denied;
  for (std::filesystem::directory_iterator it(dir, options, iter_error), end; !iter_error && it != end; it.increment(iter_error))
  {
    auto const entry_path = it->path();
    if (!theme_filename_is_json_candidate(entry_path))
      continue;

    std::error_code symlink_error;
    if (it->is_symlink(symlink_error) || symlink_error)
      continue;

    // Prefer symlink_status so special files/FIFOs are rejected without following.
    std::error_code status_error;
    auto const status = it->symlink_status(status_error);
    if (status_error || !std::filesystem::is_regular_file(status))
      continue;

    collection.candidates.push_back(ThemeCandidatePath{.path = entry_path, .order_key = normalized_theme_path_key(entry_path)});
  }

  std::ranges::sort(collection.candidates, [](ThemeCandidatePath const& left, ThemeCandidatePath const& right) {
    if (left.order_key != right.order_key)
      return left.order_key < right.order_key;
    return left.path.generic_string() < right.path.generic_string();
  });
  if (collection.candidates.size() > kMaxTuiCustomThemeCandidates)
  {
    collection.exceeded_cap = true;
    collection.candidates.resize(kMaxTuiCustomThemeCandidates);
  }
  return collection;
}

struct DiscoveredCustomThemes
{
  std::vector<TuiCustomThemeSummary> themes;
  // Valid themes encountered while scanning, including duplicates, in path order.
  std::vector<ava::tui::TuiCustomTheme> valid_in_path_order;
  bool complete = true;
  TuiCustomThemeDiscoveryIncompleteReason incomplete_reason = TuiCustomThemeDiscoveryIncompleteReason::None;
  std::size_t aggregate_bytes_read = 0;
};

void mark_discovery_incomplete(DiscoveredCustomThemes& discovered, TuiCustomThemeDiscoveryIncompleteReason reason)
{
  discovered.complete = false;
  discovered.incomplete_reason = reason;
}

char const* discovery_incomplete_reason_token(TuiCustomThemeDiscoveryIncompleteReason reason)
{
  switch (reason)
  {
    case TuiCustomThemeDiscoveryIncompleteReason::CandidateCap:
      return "candidate_cap";
    case TuiCustomThemeDiscoveryIncompleteReason::AggregateBudget:
      return "aggregate_budget";
    case TuiCustomThemeDiscoveryIncompleteReason::None:
      return "none";
  }
  return "truncated";
}

// Bounded deterministic discovery used by listing, watch catalog, and named lookup.
// Policy:
// - candidate files ordered by normalized absolute path
// - each open reads at most min(per-file cap, remaining aggregate budget)
// - skip symlinks/special/unreadable/oversized/invalid files for unconfigured catalog listing
// - first valid file per theme name wins for available/catalog results
// - later same-name valid files remain visible to named lookup so configured load can fail closed
// - incomplete scans (candidate cap or aggregate budget) are recorded; listing keeps the prefix
DiscoveredCustomThemes discover_custom_themes_bounded(ava::config::XdgPaths const& paths)
{
  DiscoveredCustomThemes discovered;
  auto const collection = collect_custom_theme_candidate_paths(tui_theme_directory(paths));
  std::size_t aggregate_bytes = 0;

  for (auto const& candidate : collection.candidates)
  {
    if (aggregate_bytes >= kMaxTuiCustomThemeCatalogAggregateBytes)
    {
      mark_discovery_incomplete(discovered, TuiCustomThemeDiscoveryIncompleteReason::AggregateBudget);
      break;
    }

    auto const remaining = kMaxTuiCustomThemeCatalogAggregateBytes - aggregate_bytes;
    auto const max_read = std::min(kMaxTuiCustomThemeFileBytes, remaining);
    auto text = read_custom_theme_text_bounded(candidate.path, max_read);
    if (!text)
      continue;

    // Charge physical work before any skip/incomplete decision so accounting never under-reports.
    // bytes_read is already clamped to max_read <= remaining, so this cannot exceed the aggregate cap.
    aggregate_bytes += text->bytes_read;

    if (text->status == BoundedThemeTextRead::Status::Truncated)
    {
      if (max_read < kMaxTuiCustomThemeFileBytes)
      {
        // Could not finish within the remaining aggregate budget; stop without parsing a prefix.
        mark_discovery_incomplete(discovered, TuiCustomThemeDiscoveryIncompleteReason::AggregateBudget);
        break;
      }
      // Per-file oversized/unstable relative to the file cap: skip and keep scanning within budget.
      continue;
    }

    auto theme = parse_custom_theme_text(std::move(text->content), candidate.path);
    if (!theme)
      continue;

    discovered.valid_in_path_order.push_back(*theme);
    bool const duplicate_name = std::ranges::any_of(discovered.themes, [&](TuiCustomThemeSummary const& existing) { return existing.name == theme->name; });
    if (duplicate_name)
      continue;

    discovered.themes.push_back(TuiCustomThemeSummary{.name = theme->name, .path = theme->path, .palette = theme->palette, .revision = theme->revision});
  }

  discovered.aggregate_bytes_read = aggregate_bytes;
  if (discovered.complete && collection.exceeded_cap)
    mark_discovery_incomplete(discovered, TuiCustomThemeDiscoveryIncompleteReason::CandidateCap);

  // Stable result order by theme name (path order already resolved duplicate winners).
  std::ranges::sort(discovered.themes, {}, &TuiCustomThemeSummary::name);
  return discovered;
}

TuiCustomThemeDiscoveryResult to_public_discovery_result(DiscoveredCustomThemes discovered)
{
  return TuiCustomThemeDiscoveryResult{
      .themes = std::move(discovered.themes),
      .complete = discovered.complete,
      .incomplete_reason = discovered.incomplete_reason,
      .aggregate_bytes_read = discovered.aggregate_bytes_read,
  };
}

ava::core::Result<ava::tui::TuiCustomTheme> resolve_named_custom_theme(std::string_view name, DiscoveredCustomThemes const& discovered,
                                                                       std::filesystem::path const& directory)
{
  std::optional<ava::tui::TuiCustomTheme> match;
  for (auto const& theme : discovered.valid_in_path_order)
  {
    if (theme.name != name)
      continue;
    if (match)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "duplicate TUI custom theme name")
                                 .with_context("theme", std::string(name))
                                 .with_context("path", match->path.string())
                                 .with_context("path", theme.path.string()));
    }
    match = theme;
  }

  // Uniqueness-sensitive lookup must fail closed when the scan stopped early: a late duplicate or
  // sole match may lie beyond the candidate/byte boundary even if one file already matched.
  if (!discovered.complete)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "TUI custom theme discovery is incomplete")
                     .with_context("theme", std::string(name))
                     .with_context("reason", discovery_incomplete_reason_token(discovered.incomplete_reason))
                     .with_context("directory", directory.string());
    if (discovered.incomplete_reason == TuiCustomThemeDiscoveryIncompleteReason::CandidateCap)
      error.with_context("max_candidates", std::to_string(kMaxTuiCustomThemeCandidates));
    if (discovered.incomplete_reason == TuiCustomThemeDiscoveryIncompleteReason::AggregateBudget)
      error.with_context("max_aggregate_bytes", std::to_string(kMaxTuiCustomThemeCatalogAggregateBytes));
    return std::unexpected(std::move(error));
  }

  if (!match)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "TUI custom theme was not found")
                               .with_context("theme", std::string(name))
                               .with_context("directory", directory.string()));
  }
  return std::move(*match);
}

ava::core::Result<TuiDisplaySettings> resolve_theme_into_settings(ava::config::XdgPaths const& paths, DisplaySettingsDocument document,
                                                                  DiscoveredCustomThemes const* discovered)
{
  auto settings = display_settings_from_document_fields(document);
  if (!document.theme)
    return settings;

  if (auto normalized = normalize_tui_theme_setting(*document.theme))
  {
    settings.theme = std::move(normalized);
    return settings;
  }

  ava::core::Result<ava::tui::TuiCustomTheme> custom = std::unexpected(invalid_display_theme_error(document));
  if (discovered != nullptr)
    custom = resolve_named_custom_theme(*document.theme, *discovered, tui_theme_directory(paths));
  else
    custom = load_tui_custom_theme(paths, *document.theme);
  if (!custom)
    return std::unexpected(invalid_display_theme_error(document));
  settings.theme = custom->name;
  settings.custom_theme = std::move(*custom);
  return settings;
}

ava::core::Result<TuiDisplaySettings> resolve_theme_into_settings(ava::config::XdgPaths const& paths, DisplaySettingsDocument document)
{
  return resolve_theme_into_settings(paths, std::move(document), nullptr);
}

}  // namespace

ava::core::Result<ava::tui::TuiCustomTheme> load_tui_custom_theme_file(std::filesystem::path const& path)
{
  auto text = read_custom_theme_text_bounded(path, kMaxTuiCustomThemeFileBytes);
  if (!text)
    return std::unexpected(std::move(text.error()));
  if (text->status == BoundedThemeTextRead::Status::Truncated)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "TUI custom theme is too large")
                               .with_context("path", path.string())
                               .with_context("max_bytes", std::to_string(kMaxTuiCustomThemeFileBytes)));
  }
  return parse_custom_theme_text(std::move(text->content), path);
}

ava::core::Result<ava::tui::TuiCustomTheme> load_tui_custom_theme(ava::config::XdgPaths const& paths, std::string_view name)
{
  if (!valid_custom_theme_name(name))
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid TUI custom theme name").with_context("theme", std::string(name)));
  }

  auto const dir = tui_theme_directory(paths);
  std::error_code exists_error;
  if (!std::filesystem::exists(dir, exists_error))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "TUI custom theme was not found")
                               .with_context("theme", std::string(name))
                               .with_context("directory", dir.string()));
  }

  // Named/configured lookup uses the same bounded deterministic scan. Duplicate valid names and
  // incomplete scans fail closed with an actionable local error (no raw theme content).
  auto const discovered = discover_custom_themes_bounded(paths);
  return resolve_named_custom_theme(name, discovered, dir);
}

TuiCustomThemeDiscoveryResult discover_tui_custom_themes(ava::config::XdgPaths const& paths)
{
  return to_public_discovery_result(discover_custom_themes_bounded(paths));
}

std::vector<TuiCustomThemeSummary> available_tui_custom_themes(ava::config::XdgPaths const& paths)
{
  return discover_tui_custom_themes(paths).themes;
}

ava::core::Result<DisplaySettingsDocument> load_display_settings_document(ava::config::XdgPaths const& paths)
{
  return load_or_default_document(paths);
}

ava::core::Result<TuiDisplaySettings> load_tui_display_settings(ava::config::XdgPaths const& paths)
{
  auto document = load_or_default_document(paths);
  if (!document)
    return std::unexpected(std::move(document.error()));
  return resolve_theme_into_settings(paths, std::move(*document));
}

ava::core::Result<TuiDisplaySettings> apply_tui_display_settings(ava::config::XdgPaths const& paths)
{
  auto settings = load_tui_display_settings(paths);
  if (!settings)
    return std::unexpected(std::move(settings.error()));
  ava::tui::set_tui_config_theme(settings->theme, settings->custom_theme);
  return settings;
}

ava::core::Result<TuiDisplaySettingsWatchState> load_tui_display_settings_watch_state(ava::config::XdgPaths const& paths)
{
  auto const path = tui_display_settings_file(paths);
  std::string display_revision = "missing";
  std::error_code exists_error;
  auto const exists = std::filesystem::exists(path, exists_error);
  if (exists_error)
    return std::unexpected(io_error("failed to inspect TUI display settings", path, exists_error));
  if (exists)
  {
    auto content = read_display_settings_text(path);
    if (!content)
      return std::unexpected(std::move(content.error()));
    display_revision = revision_for_text(*content);
  }

  auto document = load_or_default_document(paths);
  if (!document)
    return std::unexpected(std::move(document.error()));

  // One bounded discovery feeds both configured custom resolution and the watch catalog fingerprint.
  auto const discovered = discover_custom_themes_bounded(paths);
  auto settings = resolve_theme_into_settings(paths, std::move(*document), &discovered);
  if (!settings)
    return std::unexpected(std::move(settings.error()));

  TuiDisplaySettingsWatchState state;
  state.display_revision = std::move(display_revision);
  state.theme = settings->theme;
  state.show_images = settings->show_images;
  state.image_width_cells = settings->image_width_cells;
  state.cursor = settings->cursor;
  state.mermaid = settings->mermaid;
  if (settings->custom_theme)
  {
    state.custom_theme_path = settings->custom_theme->path;
    state.custom_theme_revision = settings->custom_theme->revision;
  }
  for (auto const& theme : discovered.themes) state.custom_theme_catalog.push_back(TuiCustomThemeCatalogEntry{.name = theme.name, .revision = theme.revision});
  return state;
}

bool tui_display_settings_watch_state_changed(TuiDisplaySettingsWatchState const& previous, TuiDisplaySettingsWatchState const& current)
{
  return previous.display_revision != current.display_revision || previous.theme != current.theme || previous.custom_theme_path != current.custom_theme_path ||
         previous.custom_theme_revision != current.custom_theme_revision || previous.show_images != current.show_images ||
         previous.image_width_cells != current.image_width_cells || previous.cursor.style() != current.cursor.style() ||
         previous.cursor.blink() != current.cursor.blink() || previous.mermaid.enabled != current.mermaid.enabled ||
         previous.mermaid.argv != current.mermaid.argv || previous.custom_theme_catalog.size() != current.custom_theme_catalog.size() ||
         !std::equal(previous.custom_theme_catalog.begin(), previous.custom_theme_catalog.end(), current.custom_theme_catalog.begin(),
                     [](TuiCustomThemeCatalogEntry const& left, TuiCustomThemeCatalogEntry const& right) {
                       return left.name == right.name && left.revision == right.revision;
                     });
}

ava::core::VoidResult store_tui_theme_setting(ava::config::XdgPaths const& paths, std::optional<std::string> theme)
{
  auto document = load_or_default_document(paths);
  if (!document)
    return std::unexpected(std::move(document.error()));

  auto resolved = resolve_theme_name_for_store(paths, std::move(theme), document->path);
  if (!resolved)
    return std::unexpected(std::move(resolved.error()));
  document->theme = std::move(*resolved);
  return store_display_settings_document(*document);
}

ava::core::VoidResult store_tui_show_images_setting(ava::config::XdgPaths const& paths, std::optional<bool> show_images)
{
  auto document = load_or_default_document(paths);
  if (!document)
    return std::unexpected(std::move(document.error()));
  document->show_images = show_images;
  return store_display_settings_document(*document);
}

ava::core::VoidResult store_tui_image_width_setting(ava::config::XdgPaths const& paths, std::optional<std::size_t> image_width_cells)
{
  if (image_width_cells && (*image_width_cells < kMinTuiImageWidthCells || *image_width_cells > kMaxTuiImageWidthCells))
  {
    return std::unexpected(
        invalid_display_error("image_width_cells must be an integer between 8 and 160", tui_display_settings_file(paths), "image_width_cells")
            .with_context("value", std::to_string(*image_width_cells)));
  }
  auto document = load_or_default_document(paths);
  if (!document)
    return std::unexpected(std::move(document.error()));
  document->image_width_cells = image_width_cells;
  return store_display_settings_document(*document);
}

ava::core::VoidResult store_tui_cursor_setting(ava::config::XdgPaths const& paths, ava::tui::terminal::CursorStyle style, std::optional<bool> blink)
{
  auto document = load_or_default_document(paths);
  if (!document)
    return std::unexpected(std::move(document.error()));
  document->cursor_style = style;
  if (blink)
    document->cursor_blink = *blink;
  return store_display_settings_document(*document);
}

}  // namespace ava::app
