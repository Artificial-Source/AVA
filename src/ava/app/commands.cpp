#include "sys.h"
#include "ava/app/command_connect.h"
#include "ava/app/command_format.h"
#include "ava/app/command_help.h"
#include "ava/app/command_jobs.h"
#include "ava/app/command_mcp.h"
#include "ava/app/command_models.h"
#include "ava/app/command_permissions.h"
#include "ava/app/command_plugins.h"
#include "ava/app/command_registry.h"
#include "ava/app/command_reload.h"
#include "ava/app/command_sessions.h"
#include "ava/app/command_tools.h"
#include "ava/app/command_trust.h"
#include "ava/app/commands.h"
#include "ava/app/display_settings.h"
#include "ava/app/plugin_event_hooks.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/tools/file_tools.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/theme.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/static_resources.h"
#include "ava/permissions/permission.h"
#include "ava/context/skill_loader.h"
#include "ava/core/atomic_file.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

constexpr auto kMaxKeybindingsImportBytes = std::uintmax_t{256 * 1024};

struct JsonObjectEntry
{
  std::string key;
  std::string raw_key;
  std::string raw_value;
};

bool starts_with_command(std::string_view line, std::string_view command) noexcept
{
  return line == command || (line.starts_with(command) && line.size() > command.size() && line[command.size()] == ' ');
}

bool is_json_whitespace(char ch) noexcept
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

void skip_json_whitespace(std::string_view text, std::size_t& offset) noexcept
{
  while (offset < text.size() && is_json_whitespace(text[offset])) ++offset;
}

std::string trim_copy(std::string_view text)
{
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
  return std::string(text.substr(begin, end - begin));
}

std::optional<std::size_t> json_string_literal_end(std::string_view text, std::size_t start) noexcept
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

std::optional<std::vector<JsonObjectEntry>> top_level_json_object_entries(std::string_view object)
{
  if (!ava::core::json::is_valid_object(object))
    return std::nullopt;

  std::vector<JsonObjectEntry> entries;
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
    entries.push_back(JsonObjectEntry{.key = *decoded_key, .raw_key = std::move(raw_key), .raw_value = std::move(raw_value)});
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

std::vector<std::string> split_keybinding_tokens(std::vector<std::string> const& args, std::size_t first)
{
  std::vector<std::string> tokens;
  for (std::size_t index = first; index < args.size(); ++index)
  {
    std::string_view text = args[index];
    std::size_t start = 0;
    while (start <= text.size())
    {
      auto const comma = text.find(',', start);
      auto const end = comma == std::string_view::npos ? text.size() : comma;
      auto token = trim_copy(text.substr(start, end - start));
      if (!token.empty())
        tokens.push_back(std::move(token));
      if (comma == std::string_view::npos)
        break;
      start = comma + 1;
    }
  }
  return tokens;
}

std::optional<std::vector<std::string>> keybinding_key_displays(std::vector<std::string> const& args, std::size_t first, std::string& error)
{
  auto tokens = split_keybinding_tokens(args, first);
  if (tokens.empty())
  {
    error = "missing keybinding key";
    return std::nullopt;
  }

  std::vector<std::string> displays;
  for (auto const& token : tokens)
  {
    auto const key = ava::tui::parse_key_name(token);
    if (!key)
    {
      error = "unknown TUI key binding:\n  key: " + token;
      return std::nullopt;
    }
    auto display = ava::tui::key_display(*key);
    if (display.empty())
    {
      error = "unsupported TUI key binding:\n  key: " + token;
      return std::nullopt;
    }
    if (std::ranges::find(displays, display) == displays.end())
      displays.push_back(std::move(display));
  }
  if (displays.empty())
  {
    error = "missing keybinding key";
    return std::nullopt;
  }
  return displays;
}

std::string keybinding_json_value(std::vector<std::string> const& key_displays)
{
  if (key_displays.size() == 1)
    return "\"" + ava::core::json::escape(key_displays.front()) + "\"";

  std::string output = "[";
  for (std::size_t index = 0; index < key_displays.size(); ++index)
  {
    if (index > 0)
      output += ", ";
    output += "\"" + ava::core::json::escape(key_displays[index]) + "\"";
  }
  output += "]";
  return output;
}

bool keybinding_entry_matches_action(JsonObjectEntry const& entry, ava::tui::TuiAction action)
{
  auto const entry_action = ava::tui::key_binding_action_from_name(entry.key);
  return entry_action && *entry_action == action;
}

std::string render_keybinding_object_setting_action(std::vector<JsonObjectEntry> const& entries, ava::tui::TuiAction action, std::string_view raw_key,
                                                    std::string_view raw_value)
{
  std::string output = "{\n";
  bool first = true;
  bool replaced = false;
  auto append_entry = [&](std::string_view key, std::string_view value) {
    if (!first)
      output += ",\n";
    first = false;
    output += "  ";
    output += key;
    output += ": ";
    output += value;
  };

  for (auto const& entry : entries)
  {
    if (keybinding_entry_matches_action(entry, action))
    {
      if (!replaced)
      {
        append_entry(raw_key, raw_value);
        replaced = true;
      }
      continue;
    }
    append_entry(entry.raw_key, entry.raw_value);
  }
  if (!replaced)
    append_entry(raw_key, raw_value);
  output += "\n}\n";
  return output;
}

std::string render_keybinding_object_without_action(std::vector<JsonObjectEntry> const& entries, ava::tui::TuiAction action)
{
  std::string output = "{\n";
  bool first = true;
  for (auto const& entry : entries)
  {
    if (keybinding_entry_matches_action(entry, action))
      continue;
    if (!first)
      output += ",\n";
    first = false;
    output += "  ";
    output += entry.raw_key;
    output += ": ";
    output += entry.raw_value;
  }
  output += "\n}\n";
  return output;
}

bool keybinding_object_has_action(std::vector<JsonObjectEntry> const& entries, ava::tui::TuiAction action)
{
  return std::ranges::any_of(entries, [action](auto const& entry) { return keybinding_entry_matches_action(entry, action); });
}

std::string join_display_list(std::vector<std::string> const& values)
{
  std::string output;
  for (std::size_t index = 0; index < values.size(); ++index)
  {
    if (index > 0)
      output += ", ";
    output += values[index];
  }
  return output;
}

std::string_view command_token(std::string_view line) noexcept
{
  auto const end = line.find_first_of(" \t\r\n");
  return line.substr(0, end == std::string_view::npos ? line.size() : end);
}

bool is_shell_helper_command(std::string_view line) noexcept
{
  return line.starts_with('!');
}

std::string shell_helper_argument(std::string_view line)
{
  if (!is_shell_helper_command(line))
    return {};
  line.remove_prefix(line.starts_with("!!") ? 2 : 1);
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
  return std::string(line);
}

CommandResult handled_prompt(std::string command, std::string source, std::string message)
{
  CommandResult result;
  result.handled = true;
  result.prompt_command = std::move(command);
  result.prompt_source = std::move(source);
  result.prompt_message = std::move(message);
  return result;
}

CommandResult run_keybindings_command(runtime::session_ts& unlocked_session, std::string_view argument, std::vector<CommandHotkey> const& hotkeys)
{
  auto const args = split_command_arguments(argument);
  if (args.empty())
    return handled_text(command_hotkeys_text(hotkeys));

  auto const keybinds_file = runtime::session_ts::rat(unlocked_session)->paths().ava_config_dir / "keybinds.json";
  if (args.front() == "set")
  {
    if (args.size() < 3)
      return handled_text(missing_argument("/keybindings set <action> <key>[,<key>...]"));

    auto const action = args[1];
    auto const resolved_action = ava::tui::key_binding_action_from_name(action);
    if (!resolved_action)
    {
      return handled_text("keybindings assignment is invalid:\ninvalid_argument: unknown TUI keybinding action\n  action: " + action +
                          "\nTarget was not changed.");
    }
    std::string key_error;
    auto const key_displays = keybinding_key_displays(args, 2, key_error);
    if (!key_displays)
      return handled_text(key_error + "\nusage: /keybindings set <action> <key>[,<key>...]");

    auto const canonical_action = ava::tui::key_binding_config_action_id(*resolved_action);
    auto const raw_key = "\"" + ava::core::json::escape(canonical_action) + "\"";
    auto const raw_value = keybinding_json_value(*key_displays);
    if (auto parsed = ava::tui::parse_key_bindings_json("{" + raw_key + ":" + raw_value + "}"); !parsed)
    {
      return handled_text("keybindings assignment is invalid:\n" + parsed.error().format() + "\nTarget was not changed.");
    }

    std::string content = "{}\n";
    std::error_code exists_error;
    auto const exists = std::filesystem::exists(keybinds_file, exists_error);
    if (exists_error)
    {
      return handled_text("failed to inspect keybindings file: " + keybinds_file.string() + "\n  cause: " + exists_error.message());
    }
    if (exists)
    {
      std::error_code regular_error;
      auto const regular = std::filesystem::is_regular_file(keybinds_file, regular_error);
      if (regular_error)
      {
        return handled_text("failed to inspect keybindings file: " + keybinds_file.string() + "\n  cause: " + regular_error.message());
      }
      if (!regular)
        return handled_text("keybindings file is not a regular file:\n  " + keybinds_file.string());

      std::error_code size_error;
      auto const config_bytes = std::filesystem::file_size(keybinds_file, size_error);
      if (size_error)
      {
        return handled_text("failed to size keybindings file: " + keybinds_file.string() + "\n  cause: " + size_error.message());
      }
      if (config_bytes > kMaxKeybindingsImportBytes)
      {
        return handled_text("keybindings file is too large to edit safely:\n  " + keybinds_file.string() +
                            "\n  limit: " + std::to_string(kMaxKeybindingsImportBytes) + " bytes");
      }

      content.assign(static_cast<std::size_t>(config_bytes), '\0');
      std::ifstream input(keybinds_file, std::ios::binary);
      if (!input)
        return handled_text("failed to read keybindings file:\n  " + keybinds_file.string());
      if (!content.empty())
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
      if (!input && static_cast<std::uintmax_t>(input.gcount()) != config_bytes)
      {
        return handled_text("failed to finish reading keybindings file:\n  " + keybinds_file.string());
      }
      if (trim_copy(content).empty())
        content = "{}\n";
    }

    auto const entries = top_level_json_object_entries(content);
    if (!entries)
    {
      return handled_text("keybindings file is not a valid JSON object:\n  " + keybinds_file.string() + "\nTarget was not changed.");
    }

    auto const candidate = render_keybinding_object_setting_action(*entries, *resolved_action, raw_key, raw_value);
    if (auto parsed = ava::tui::parse_key_bindings_json(candidate); !parsed)
    {
      return handled_text("keybindings assignment is invalid:\n" + parsed.error().format() + "\nTarget was not changed.");
    }

    if (auto written = ava::core::write_text_file_atomic(keybinds_file, candidate, "keybindings file"); !written)
      return handled_text(written.error().format());

    if (auto loaded = ava::tui::load_key_bindings(keybinds_file); !loaded)
    {
      return handled_text("wrote keybindings file, but validation failed:\n" + loaded.error().format());
    }

    return handled_text("Set keybinding:\n  action: " + action + "\n  keys: " + join_display_list(*key_displays) + "\n  target: " + keybinds_file.string() +
                        "\nRun /reload keybindings inside the interactive TUI to apply it.");
  }

  if (args.front() == "reset" || args.front() == "unset")
  {
    if (args.size() != 2)
      return handled_text(missing_argument("/keybindings reset <action>"));

    auto const action = args[1];
    auto const resolved_action = ava::tui::key_binding_action_from_name(action);
    if (!resolved_action)
    {
      return handled_text("keybindings reset target is invalid:\ninvalid_argument: unknown TUI keybinding action\n  action: " + action +
                          "\nTarget was not changed.");
    }

    std::error_code exists_error;
    auto const exists = std::filesystem::exists(keybinds_file, exists_error);
    if (exists_error)
    {
      return handled_text("failed to inspect keybindings file: " + keybinds_file.string() + "\n  cause: " + exists_error.message());
    }
    if (!exists)
    {
      return handled_text("No keybindings file found:\n  " + keybinds_file.string() + "\nNo override was reset for action: " + action);
    }

    std::error_code regular_error;
    auto const regular = std::filesystem::is_regular_file(keybinds_file, regular_error);
    if (regular_error)
    {
      return handled_text("failed to inspect keybindings file: " + keybinds_file.string() + "\n  cause: " + regular_error.message());
    }
    if (!regular)
      return handled_text("keybindings file is not a regular file:\n  " + keybinds_file.string());

    std::error_code size_error;
    auto const config_bytes = std::filesystem::file_size(keybinds_file, size_error);
    if (size_error)
    {
      return handled_text("failed to size keybindings file: " + keybinds_file.string() + "\n  cause: " + size_error.message());
    }
    if (config_bytes > kMaxKeybindingsImportBytes)
    {
      return handled_text("keybindings file is too large to edit safely:\n  " + keybinds_file.string() +
                          "\n  limit: " + std::to_string(kMaxKeybindingsImportBytes) + " bytes");
    }

    std::string content(static_cast<std::size_t>(config_bytes), '\0');
    {
      std::ifstream input(keybinds_file, std::ios::binary);
      if (!input)
        return handled_text("failed to read keybindings file:\n  " + keybinds_file.string());
      if (!content.empty())
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
      if (!input && static_cast<std::uintmax_t>(input.gcount()) != config_bytes)
      {
        return handled_text("failed to finish reading keybindings file:\n  " + keybinds_file.string());
      }
    }
    if (trim_copy(content).empty())
      content = "{}\n";

    auto const entries = top_level_json_object_entries(content);
    if (!entries)
    {
      return handled_text("keybindings file is not a valid JSON object:\n  " + keybinds_file.string() + "\nTarget was not changed.");
    }
    if (!keybinding_object_has_action(*entries, *resolved_action))
    {
      return handled_text("No keybinding override found:\n  action: " + action + "\n  target: " + keybinds_file.string() + "\nTarget was not changed.");
    }

    auto const candidate = render_keybinding_object_without_action(*entries, *resolved_action);
    if (auto parsed = ava::tui::parse_key_bindings_json(candidate); !parsed)
    {
      return handled_text("keybindings reset is invalid:\n" + parsed.error().format() + "\nTarget was not changed.");
    }

    if (auto written = ava::core::write_text_file_atomic(keybinds_file, candidate, "keybindings file"); !written)
      return handled_text(written.error().format());

    if (auto loaded = ava::tui::load_key_bindings(keybinds_file); !loaded)
    {
      return handled_text("wrote keybindings file, but validation failed:\n" + loaded.error().format());
    }

    return handled_text("Reset keybinding override:\n  action: " + action + "\n  target: " + keybinds_file.string() +
                        "\nRun /reload keybindings inside the interactive TUI to apply it.");
  }

  if (args.front() == "import")
  {
    if (args.size() < 2)
      return handled_text(missing_argument("/keybindings import <path> [--force]"));
    bool force = false;
    for (std::size_t index = 2; index < args.size(); ++index)
    {
      if (args[index] == "--force")
      {
        force = true;
        continue;
      }
      return handled_text("unsupported keybindings import option: " + args[index] + "\nsupported: --force");
    }

    auto import_path = std::filesystem::path(args[1]);
    if (import_path.is_relative())
      import_path = runtime::session_ts::rat(unlocked_session)->current_dir() / import_path;
    import_path = import_path.lexically_normal();

    std::error_code source_error;
    auto const source_exists = std::filesystem::exists(import_path, source_error);
    if (source_error)
    {
      return handled_text("failed to inspect keybindings import source: " + import_path.string() + "\n  cause: " + source_error.message());
    }
    if (!source_exists)
    {
      return handled_text("keybindings import source does not exist:\n  " + import_path.string());
    }
    auto const regular = std::filesystem::is_regular_file(import_path, source_error);
    if (source_error)
    {
      return handled_text("failed to inspect keybindings import source: " + import_path.string() + "\n  cause: " + source_error.message());
    }
    if (!regular)
    {
      return handled_text("keybindings import source is not a regular file:\n  " + import_path.string());
    }
    auto const import_bytes = std::filesystem::file_size(import_path, source_error);
    if (source_error)
    {
      return handled_text("failed to size keybindings import source: " + import_path.string() + "\n  cause: " + source_error.message());
    }
    if (import_bytes > kMaxKeybindingsImportBytes)
    {
      return handled_text("keybindings import source is too large:\n  " + import_path.string() + "\n  limit: " + std::to_string(kMaxKeybindingsImportBytes) +
                          " bytes");
    }

    if (auto loaded = ava::tui::load_key_bindings(import_path); !loaded)
    {
      return handled_text("keybindings import source is invalid:\n" + loaded.error().format() + "\nTarget was not changed.");
    }

    std::error_code exists_error;
    auto const target_exists = std::filesystem::exists(keybinds_file, exists_error);
    if (exists_error)
    {
      return handled_text("failed to inspect keybindings file: " + keybinds_file.string() + "\n  cause: " + exists_error.message());
    }
    if (target_exists && !force)
    {
      return handled_text("keybindings file already exists:\n  " + keybinds_file.string() +
                          "\nUse /keybindings import <path> --force to replace it explicitly.");
    }

    std::string content(static_cast<std::size_t>(import_bytes), '\0');
    {
      std::ifstream input(import_path, std::ios::binary);
      if (!input)
        return handled_text("failed to read keybindings import source:\n  " + import_path.string());
      if (!content.empty())
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
      if (!input && static_cast<std::uintmax_t>(input.gcount()) != import_bytes)
      {
        return handled_text("failed to finish reading keybindings import source:\n  " + import_path.string());
      }
    }

    if (auto written = ava::core::write_text_file_atomic(keybinds_file, content, "keybindings file"); !written)
      return handled_text(written.error().format());

    if (auto loaded = ava::tui::load_key_bindings(keybinds_file); !loaded)
    {
      return handled_text("imported keybindings file, but validation failed:\n" + loaded.error().format());
    }

    return handled_text("Imported keybindings file:\n  source: " + import_path.string() + "\n  target: " + keybinds_file.string() +
                        "\nRun /reload keybindings inside the interactive TUI to apply it.");
  }

  if (args.front() == "validate")
  {
    if (args.size() > 1)
    {
      return handled_text("unsupported keybindings validate option: " + args[1] + "\nsupported: no options");
    }
    std::error_code exists_error;
    auto const exists = std::filesystem::exists(keybinds_file, exists_error);
    if (exists_error)
    {
      return handled_text("failed to inspect keybindings file: " + keybinds_file.string() + "\n  cause: " + exists_error.message());
    }
    if (!exists)
    {
      return handled_text("No keybindings file found:\n  " + keybinds_file.string() +
                          "\nAVA is using built-in defaults. Run /keybindings init to create a starter file.");
    }
    auto loaded = ava::tui::load_key_bindings(keybinds_file);
    if (!loaded)
    {
      return handled_text("keybindings file is invalid:\n" + loaded.error().format() +
                          "\nPrevious active bindings remain in use until a valid /reload keybindings.");
    }
    return handled_text("keybindings file is valid:\n  " + keybinds_file.string() + "\nRun /reload keybindings inside the interactive TUI to apply edits.");
  }

  if (args.front() != "init")
  {
    return handled_text("unsupported keybindings command: " + args.front() +
                        "\nsupported: init [--force], import <path> [--force], set <action> <key>[,<key>...], reset <action>, validate");
  }

  bool force = false;
  for (std::size_t index = 1; index < args.size(); ++index)
  {
    if (args[index] == "--force")
    {
      force = true;
      continue;
    }
    return handled_text("unsupported keybindings init option: " + args[index] + "\nsupported: --force");
  }

  std::error_code exists_error;
  auto const exists = std::filesystem::exists(keybinds_file, exists_error);
  if (exists_error)
  {
    return handled_text("failed to inspect keybindings file: " + keybinds_file.string() + "\n  cause: " + exists_error.message());
  }
  if (exists && !force)
  {
    return handled_text("keybindings file already exists:\n  " + keybinds_file.string() +
                        "\nUse /keybindings init --force to replace it, or edit it and run /reload keybindings in the TUI.");
  }

  auto const content = ava::tui::default_key_bindings_config_json();
  if (auto parsed = ava::tui::parse_key_bindings_json(content); !parsed)
  {
    return handled_text("failed to build default keybindings template:\n" + parsed.error().format());
  }

  if (auto written = ava::core::write_text_file_atomic(keybinds_file, content, "keybindings file"); !written)
    return handled_text(written.error().format());

  if (auto loaded = ava::tui::load_key_bindings(keybinds_file); !loaded)
  {
    return handled_text("wrote keybindings file, but validation failed:\n" + loaded.error().format());
  }

  return handled_text(std::string(force ? "Replaced" : "Created") + " keybindings starter file:\n  " + keybinds_file.string() +
                      "\nEdit it, then run /reload keybindings inside the interactive TUI.");
}

ava::core::Result<CommandResult> run_theme_command(runtime::session_ts& unlocked_session, std::string_view argument)
{
  auto const args = split_command_arguments(argument);
  if (args.size() > 1)
    return handled_text("unsupported theme options: " + std::string(argument) + "\n" + tui_theme_setting_usage());

  auto&& session_r = [&unlocked_session]() -> runtime::session_ts::crat {
    return unlocked_session;
  };

  auto settings = load_tui_display_settings(session_r()->paths());
  if (!settings)
    return std::unexpected(std::move(settings.error()));

  if (args.empty())
  {
    ava::tui::set_tui_config_theme(settings->theme, settings->custom_theme);
    return handled_text("TUI theme:\n  config: " + settings->path.string() +
                        "\n  configured: " + (settings->theme ? *settings->theme : std::string("built-in default")) +
                        "\n  active: " + active_tui_theme_summary() + "\n" + tui_theme_setting_usage());
  }

  if (is_tui_theme_reset_value(args.front()))
  {
    auto stored = store_tui_theme_setting(session_r()->paths(), std::nullopt);
    if (!stored)
      return std::unexpected(std::move(stored.error()));
    ava::tui::set_tui_config_theme(std::nullopt);
    return handled_text("Reset TUI theme to the built-in default.\n  config: " + tui_display_settings_file(session_r()->paths()).string() +
                        "\n  active: " + active_tui_theme_summary());
  }

  if (!normalize_tui_theme_setting(args.front()))
  {
    auto custom_theme = load_tui_custom_theme(session_r()->paths(), args.front());
    if (!custom_theme && custom_theme.error().category() == ava::core::ErrorCategory::NotFound)
      return handled_text("unsupported theme: " + args.front() + "\n" + tui_theme_setting_usage());
    if (!custom_theme)
      return std::unexpected(std::move(custom_theme.error()));
  }

  auto stored = store_tui_theme_setting(session_r()->paths(), args.front());
  if (!stored)
    return std::unexpected(std::move(stored.error()));
  auto settings_after_store = load_tui_display_settings(session_r()->paths());
  if (!settings_after_store)
    return std::unexpected(std::move(settings_after_store.error()));
  ava::tui::set_tui_config_theme(settings_after_store->theme, settings_after_store->custom_theme);
  return handled_text("Stored TUI theme " + *settings_after_store->theme + ".\n  config: " + tui_display_settings_file(session_r()->paths()).string() +
                      "\n  active: " + active_tui_theme_summary());
}

ava::core::Result<CommandResult> run_images_command(runtime::session_ts& unlocked_session, std::string_view argument)
{
  auto const args = split_command_arguments(argument);
  if (args.size() > 1)
    return handled_text("unsupported images options: " + std::string(argument) + "\n" + tui_show_images_setting_usage());

  auto const paths = runtime::session_ts::crat(unlocked_session)->paths();
  auto settings = load_tui_display_settings(paths);
  if (!settings)
    return std::unexpected(std::move(settings.error()));

  if (args.empty())
  {
    return handled_text(std::string("TUI images:\n  config: ") + settings->path.string() +
                        "\n  configured: " + (settings->show_images_configured ? (settings->show_images ? "on" : "off") : std::string("default on")) +
                        "\n  effective: " + (settings->show_images ? "on" : "off") + "\n" + tui_show_images_setting_usage());
  }

  if (is_tui_show_images_reset_value(args.front()))
  {
    auto stored = store_tui_show_images_setting(paths, std::nullopt);
    if (!stored)
      return std::unexpected(std::move(stored.error()));
    return handled_text("Reset TUI image visibility to the default (on).\n  config: " + tui_display_settings_file(paths).string());
  }

  auto const normalized = normalize_tui_show_images_setting(args.front());
  if (!normalized)
    return handled_text("unsupported images option: " + args.front() + "\n" + tui_show_images_setting_usage());

  auto stored = store_tui_show_images_setting(paths, *normalized);
  if (!stored)
    return std::unexpected(std::move(stored.error()));
  return handled_text(std::string("Stored TUI image visibility ") + (*normalized ? "on" : "off") +
                      ".\n  config: " + tui_display_settings_file(paths).string());
}

ava::core::Result<CommandResult> run_cursor_command(runtime::session_ts& unlocked_session, std::string_view argument)
{
  auto const args = split_command_arguments(argument);
  if (args.empty() || args.size() > 2)
    return handled_text(tui_cursor_setting_usage());

  auto const style = normalize_tui_cursor_style_setting(args.front());
  if (!style)
    return handled_text("unsupported cursor style: " + args.front() + "\n" + tui_cursor_setting_usage());

  auto blink = std::optional<bool>{};
  if (args.size() == 2)
  {
    blink = normalize_tui_cursor_blink_setting(args[1]);
    if (!blink)
      return handled_text("unsupported cursor blink mode: " + args[1] + "\n" + tui_cursor_setting_usage());
  }

  auto const paths = runtime::session_ts::crat(unlocked_session)->paths();
  auto stored = store_tui_cursor_setting(paths, *style, blink);
  if (!stored)
    return std::unexpected(std::move(stored.error()));
  auto settings = load_tui_display_settings(paths);
  if (!settings)
    return std::unexpected(std::move(settings.error()));
  auto const blink_name = settings->cursor.blink() ? "blink" : "steady";
  return handled_text("Stored TUI cursor " + std::string(tui_cursor_style_name(settings->cursor.style())) + " " + blink_name +
                      ".\n  config: " + tui_display_settings_file(paths).string());
}

ava::core::Result<CommandResult> run_image_width_command(runtime::session_ts& unlocked_session, std::string_view argument)
{
  auto const args = split_command_arguments(argument);
  if (args.size() > 1)
    return handled_text("unsupported image-width options: " + std::string(argument) + "\n" + tui_image_width_setting_usage());

  auto const paths = runtime::session_ts::crat(unlocked_session)->paths();
  auto settings = load_tui_display_settings(paths);
  if (!settings)
    return std::unexpected(std::move(settings.error()));

  if (args.empty())
  {
    return handled_text(std::string("TUI image width:\n  config: ") + settings->path.string() + "\n  configured: " +
                        (settings->image_width_configured ? std::to_string(settings->image_width_cells) + " cells" : std::string("default 60 cells")) +
                        "\n  effective: " + std::to_string(settings->image_width_cells) + " cells\n" + tui_image_width_setting_usage());
  }

  if (is_tui_image_width_reset_value(args.front()))
  {
    auto stored = store_tui_image_width_setting(paths, std::nullopt);
    if (!stored)
      return std::unexpected(std::move(stored.error()));
    return handled_text("Reset TUI image width to the default (60 cells).\n  config: " + tui_display_settings_file(paths).string());
  }

  auto const normalized = normalize_tui_image_width_setting(args.front());
  if (!normalized)
    return handled_text("unsupported image width: " + args.front() + "\n" + tui_image_width_setting_usage());

  auto stored = store_tui_image_width_setting(paths, *normalized);
  if (!stored)
    return std::unexpected(std::move(stored.error()));
  return handled_text("Stored TUI image width " + std::to_string(*normalized) + " cells.\n  config: " + tui_display_settings_file(paths).string());
}

std::string dynamic_command_argument(std::string_view line)
{
  auto const token = command_token(line);
  if (line.size() <= token.size())
    return {};
  auto rest = line.substr(token.size());
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
  return std::string(rest);
}

ava::core::Result<std::string> skill_prompt_message(runtime::session_ts& unlocked_session, CommandRequest const& request, CommandRegistryEntry const& entry)
{
  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  CRITICAL_AREA_BEGIN_R(session);
  auto plugin_diagnostics =
      ava::plugin::collect_plugin_diagnostics(resource_policy.plugin_discovery, resource_policy.plugin_enablement_file, session_r->workspace_dir());
  auto loaded = ava::context::load_skills(ava::context::SkillLoadOptions{
      .workspace_root = session_r->workspace_dir(),
      .global_skill_dirs = resource_policy.global_skill_dirs,
      .project_skill_dirs = resource_policy.project_skill_dirs,
      .declared_skill_files = ava::plugin::declared_plugin_skill_files(plugin_diagnostics),
      .include_project_skills = resource_policy.include_project_resources,
  });
  CRITICAL_AREA_END_R(session);

  auto const match = std::ranges::find_if(loaded.skills, [&](ava::context::LoadedSkill const& skill) { return skill.name == entry.skill_name; });
  if (match == loaded.skills.end())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "skill not found");
    error.with_context("skill", entry.skill_name);
    return std::unexpected(std::move(error));
  }

  auto context = make_tool_context(unlocked_session, request.permission_resolver);
  context.permission_tool_name = "skill";
  context.current_tool_name = "skill";
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::SkillLoad, match->path, match->name, "skill",
                                                      "skill loading requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  auto sampled_files = ava::context::sample_skill_files(match->directory);
  return ava::context::format_loaded_skill_for_tool(*match, sampled_files);
}

ava::core::Result<CommandResult> run_registry_command(runtime::session_ts& unlocked_session, CommandRequest request, CommandRegistryEntry const& entry)
{
  if (!entry.enabled)
    return handled_text(entry.command + " is disabled: " + entry.disabled_reason);
  auto const argument = dynamic_command_argument(request.command);
  switch (entry.kind)
  {
    case UnifiedCommandKind::Backend:
      return CommandResult{};
    case UnifiedCommandKind::PromptTemplate: {
      auto prompt = expand_prompt_command_template(entry.template_text, argument);
      if (!prompt)
        return std::unexpected(std::move(prompt.error()));
      return handled_prompt(entry.command, to_string(entry.source), std::move(*prompt));
    }
    case UnifiedCommandKind::SkillPrompt: {
      auto prompt = skill_prompt_message(unlocked_session, request, entry);
      if (!prompt)
        return std::unexpected(std::move(prompt.error()));
      return handled_prompt(entry.command, to_string(entry.source), std::move(*prompt));
    }
    case UnifiedCommandKind::McpPrompt: {
      auto prompt = mcp_prompt_message(unlocked_session, request, entry, argument);
      if (!prompt)
        return std::unexpected(std::move(prompt.error()));
      return handled_prompt(entry.command, to_string(entry.source), std::move(*prompt));
    }
    case UnifiedCommandKind::PluginCommand: {
      auto delegated = request;
      auto args = argument.empty() ? std::string("{}") : argument;
      delegated.command = "/plugin run " + entry.plugin_id + " " + entry.plugin_command_name + " " + args;
      return run_plugin_command(unlocked_session, delegated);
    }
  }
  return CommandResult{};
}

}  // namespace

bool is_backend_command(std::string_view line) noexcept
{
  if (is_shell_helper_command(line))
    return true;
  return find_command_catalog_entry(line) != nullptr;
}

bool is_backend_command_1(std::string_view line, runtime::session_ts& unlocked_session)
{
  if (is_backend_command(line))
    return true;
  return command_registry_contains(unlocked_session, line);
}

// Dispatch a normalized backend command against unlocked_session and return its frontend-facing result.
//
// Session state remains write-locked while invoking legacy command handlers. The lock is released before
// invoking a session-aware handler that acquires its own access guard.
ava::core::Result<CommandResult> run_command(runtime::session_ts& unlocked_session, CommandRequest request)
{
  CommandResult result;
  if (request.command.empty())
    return result;

  if (is_shell_helper_command(request.command))
  {
    auto const shell_command = shell_helper_argument(request.command);
    if (shell_command.empty())
      return handled_text(missing_argument("!<command> or !!<command>"));
    request.command = "/bash " + shell_command;
  }

  auto const* entry = find_command_catalog_entry(request.command);
  if (!entry)
  {
    auto const token = command_token(request.command);
    auto registry = load_command_registry(unlocked_session, CommandRegistryOptions{.include_builtins = false,
                                                                             .include_prompt_commands = true,
                                                                             .include_skills = true,
                                                                             .include_plugin_commands = true,
                                                                             .include_mcp_prompts = token.starts_with("/mcp:"),
                                                                             .permission_resolver = request.permission_resolver,
                                                                             .cancel_requested = request.cancel_requested});
    if (auto const* registry_entry = find_command_registry_entry(registry, request.command))
      return run_registry_command(unlocked_session, std::move(request), *registry_entry);
    if (!token.starts_with("/skill:") && !token.starts_with("/mcp:") && !token.starts_with("/plugin:"))
    {
      registry = load_command_registry(unlocked_session, CommandRegistryOptions{.include_builtins = false,
                                                                         .include_prompt_commands = false,
                                                                         .include_skills = false,
                                                                         .include_plugin_commands = false,
                                                                         .include_mcp_prompts = true,
                                                                         .permission_resolver = request.permission_resolver,
                                                                         .cancel_requested = request.cancel_requested});
      if (auto const* registry_entry = find_command_registry_entry(registry, request.command))
        return run_registry_command(unlocked_session, std::move(request), *registry_entry);
    }

    if (token.starts_with("/skill:") || token.starts_with("/mcp:") || token.starts_with("/plugin:"))
    {
      if (!registry.diagnostics.empty())
        return handled_text(registry.diagnostics.front().message);
      return handled_text("command not found: " + std::string(token));
    }
    return result;
  }
  request.command = normalize_command_line(request.command, *entry);

  if (!entry->enabled)
  {
    return handled_text(entry->command + " is disabled: " + entry->disabled_reason);
  }

  auto plugin_observer_options = plugin_event_observer_options(unlocked_session, request.permission_resolver);
  plugin_observer_options.cancel_requested = request.cancel_requested;
  request.event_sink = make_plugin_event_observer_sink(std::move(plugin_observer_options), std::move(request.event_sink));

  if (request.command == "/quit" || request.command == "/exit")
  {
    result.handled = true;
    result.quit = true;
    return result;
  }
  if (request.command == "/help")
  {
    return handled_text(command_help_text(request.hotkeys));
  }
  if (starts_with_command(request.command, "/hotkeys"))
  {
    return run_keybindings_command(unlocked_session, command_argument(request.command, "/hotkeys"), request.hotkeys);
  }
  if (starts_with_command(request.command, "/theme"))
  {
    return run_theme_command(unlocked_session, command_argument(request.command, "/theme"));
  }
  if (starts_with_command(request.command, "/images"))
  {
    return run_images_command(unlocked_session, command_argument(request.command, "/images"));
  }
  if (starts_with_command(request.command, "/image-width"))
  {
    return run_image_width_command(unlocked_session, command_argument(request.command, "/image-width"));
  }
  if (starts_with_command(request.command, "/cursor"))
  {
    return run_cursor_command(unlocked_session, command_argument(request.command, "/cursor"));
  }
  if (request.command == "/settings")
  {
    return handled_text("Settings are shown as a TUI view. Use /theme, /images, /image-width, and /cursor to persist display settings.");
  }
  if (starts_with_command(request.command, "/details"))
  {
    return handled_text(
        "Tool cards default to Rich. In the TUI, exact /details or Ctrl+O toggles Rich and Expanded; use /details compact, /details rich, or "
        "/details expanded to select a view explicitly.");
  }
  if (request.command == "/sidebar")
  {
    return handled_text("The current session overview is an interactive TUI view. Use /sidebar inside the TUI to open it.");
  }
  if (request.command == "/overview")
  {
    return handled_text("The startup resources overview is an interactive TUI view. Use /overview inside the TUI to toggle it.");
  }
  if (starts_with_command(request.command, "/search"))
  {
    return handled_text(
        "Transcript search is available only inside the interactive TUI. Use /search [query] there to find currently rendered transcript items.");
  }
  if (starts_with_command(request.command, "/tool"))
  {
    return handled_text(
        "Tool detail inspection is available inside the interactive TUI. Use /tool [query] to toggle the latest or matching card between its inherited "
        "non-expanded view and Expanded, /details to toggle Rich and Expanded globally, or /copy tool [query] to copy safe tool details.");
  }
  if (starts_with_command(request.command, "/diff"))
  {
    return handled_text(
        "Tool diff inspection is available inside the interactive TUI. Use /diff [query] to show the latest or matching unified diff, or /copy diff [query] to "
        "copy it.");
  }
  if (starts_with_command(request.command, "/copy"))
  {
    auto const argument = command_argument(request.command, "/copy");
    auto const copy_args = split_command_arguments(argument);
    auto const target = copy_args.empty() ? std::string{} : copy_args.front();
    // Exact first token parsing: "user" opens the user-turn picker in the TUI.
    // Aliases tools/diffs/permissions remain accepted for those targets only.
    if (!target.empty() && target != "user" && target != "tool" && target != "tools" && target != "diff" && target != "diffs" && target != "permission" &&
        target != "permissions")
    {
      return handled_text("unsupported copy target: " + target + "\nsupported: user [query], tool [query], diff [query], permission [query]");
    }
    return handled_text(
        "Clipboard copy is available inside the interactive TUI. Use /copy for the latest AVA message, /copy user [query] to pick a public user turn, /copy "
        "tool "
        "[query] for tool details, /copy diff [query] for unified diffs, or /copy permission [query] for permission audit details.");
  }
  if (request.command == "/thinking" || request.command == "/thinking details")
  {
    return handled_text(
        "Thinking visibility is a TUI display toggle. It does not change provider reasoning mode. Bare /thinking shows or hides all inline thinking. In the "
        "TUI, "
        "/thinking details toggles the latest completed long thinking block between its bounded preview and full text; mouse-click the Thinking: header for "
        "the "
        "same per-item expand/collapse. Expansion is presentation-only and is not persisted across reload.");
  }
  if (starts_with_command(request.command, "/attach"))
  {
    return handled_text(
        "Image attachment import is available inside the interactive TUI with /attach <path>. In headless RPC, send prompt attachments with the attachments "
        "array.");
  }
  if (starts_with_command(request.command, "/reload"))
  {
    return run_reload_command(unlocked_session, command_argument(request.command, "/reload"));
  }
  if (starts_with_command(request.command, "/models"))
  {
    return run_models_command(unlocked_session, command_argument(request.command, "/models"));
  }
  if (starts_with_command(request.command, "/providers"))
  {
    return run_providers_command(unlocked_session, command_argument(request.command, "/providers"));
  }
  if (request.command == "/scoped-models")
  {
    return handled_text("Scoped model cycling is a TUI selector. In the TUI, /scoped-models opens the model cycle list and Ctrl+S persists it to models.json.");
  }
  if (starts_with_command(request.command, "/connect"))
  {
    return run_connect_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/mcp"))
  {
    return run_mcp_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/plugins"))
  {
    return run_plugins_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/plugin"))
  {
    return run_plugin_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/trust"))
  {
    return run_trust_command(unlocked_session, command_argument(request.command, "/trust"));
  }
  if (starts_with_command(request.command, "/permissions"))
  {
    return run_permissions_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/sessions"))
  {
    return run_sessions_command(unlocked_session, command_argument(request.command, "/sessions"));
  }
  if (starts_with_command(request.command, "/jobs"))
  {
    return run_jobs_command_1(unlocked_session, command_argument(request.command, "/jobs"));
  }
  if (request.command == "/recover-persistence")
  {
    return run_recover_persistence_command(unlocked_session);
  }
  if (request.command == "/fork-from" || starts_with_command(request.command, "/fork-from"))
  {
    return handled_text(
        "Fork-from is available inside the interactive TUI. Use /fork-from to open the public user-turn picker, or /fork [name] to fork at the latest entry.");
  }
  if (starts_with_command(request.command, "/fork"))
  {
    return run_fork_command(unlocked_session, command_argument(request.command, "/fork"));
  }
  if (starts_with_command(request.command, "/clone"))
  {
    return run_clone_command(unlocked_session, command_argument(request.command, "/clone"));
  }
  if (starts_with_command(request.command, "/new"))
  {
    return run_new_session_command(unlocked_session, command_argument(request.command, "/new"));
  }
  if (starts_with_command(request.command, "/resume"))
  {
    return run_resume_command(unlocked_session, command_argument(request.command, "/resume"));
  }
  if (starts_with_command(request.command, "/name"))
  {
    return run_name_command(unlocked_session, command_argument(request.command, "/name"));
  }
  if (starts_with_command(request.command, "/labels"))
  {
    return run_labels_command(unlocked_session, command_argument(request.command, "/labels"));
  }
  if (request.command == "/mode")
  {
    return run_mode_command(unlocked_session);
  }
  if (starts_with_command(request.command, "/context"))
  {
    return run_context_command(unlocked_session, command_argument(request.command, "/context"));
  }
  if (request.command == "/stats" || request.command == "/status")
  {
    return run_stats_command(unlocked_session);
  }
  if (starts_with_command(request.command, "/compact"))
  {
    return run_compact_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/import"))
  {
    return run_import_command(unlocked_session, command_argument(request.command, "/import"));
  }
  if (starts_with_command(request.command, "/export"))
  {
    return run_export_command(unlocked_session, request);
  }

  if (entry->hint.empty() && starts_with_command(request.command, entry->command))
  {
    return handled_text(missing_argument(entry->command));
  }

  if (request.command == "/glob")
  {
    return handled_text(missing_argument("/glob <pattern>"));
  }
  if (request.command == "/find")
  {
    return handled_text(missing_argument("/find <pattern>"));
  }
  if (request.command == "/grep")
  {
    return handled_text(missing_argument("/grep <text> [glob]"));
  }
  if (request.command == "/read")
  {
    return handled_text(missing_argument("/read <path>"));
  }
  if (request.command == "/write")
  {
    return handled_text(missing_argument("/write <path> <text>"));
  }
  if (request.command == "/bash")
  {
    return handled_text(missing_argument("/bash <command>"));
  }

  return run_tool_command(unlocked_session, request);
}

}  // namespace ava::app
