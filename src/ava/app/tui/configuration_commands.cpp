#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/settings_json.h"
#include "ava/app/tui/configuration_commands.h"
#include "ava/app/tui/tui_display_settings.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/theme.h"
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

enum class TuiConfigurationCommand : std::uint8_t
{
  None = 0,
  Keybindings,
  Theme,
  Images,
  ImageWidth,
  Cursor,
};

constexpr auto kMaxKeybindingsImportBytes = std::uintmax_t{256 * 1024};

bool starts_with_command(std::string_view line, std::string_view command) noexcept
{
  return line == command || (line.starts_with(command) && line.size() > command.size() && line[command.size()] == ' ');
}

std::string trim_copy(std::string_view text)
{
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
    ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
    --end;
  return std::string(text.substr(begin, end - begin));
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

bool keybinding_entry_matches_action(SettingsJsonEntry const& entry, ava::tui::TuiAction action)
{
  auto const entry_action = ava::tui::key_binding_action_from_name(entry.key);
  return entry_action && *entry_action == action;
}

std::string render_keybinding_object_setting_action(std::vector<SettingsJsonEntry> const& entries, ava::tui::TuiAction action, std::string_view raw_key,
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

std::string render_keybinding_object_without_action(std::vector<SettingsJsonEntry> const& entries, ava::tui::TuiAction action)
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

bool keybinding_object_has_action(std::vector<SettingsJsonEntry> const& entries, ava::tui::TuiAction action)
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

    auto const entries = parse_settings_json_object_entries(content);
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

    auto const entries = parse_settings_json_object_entries(content);
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

  auto&& session_r = [&unlocked_session]() -> runtime::session_ts::crat { return unlocked_session; };

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
  return handled_text(std::string("Stored TUI image visibility ") + (*normalized ? "on" : "off") + ".\n  config: " + tui_display_settings_file(paths).string());
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

TuiConfigurationCommand classify_tui_configuration_command(std::string_view normalized_command) noexcept
{
  if (starts_with_command(normalized_command, "/hotkeys"))
    return TuiConfigurationCommand::Keybindings;
  if (starts_with_command(normalized_command, "/theme"))
    return TuiConfigurationCommand::Theme;
  if (starts_with_command(normalized_command, "/images"))
    return TuiConfigurationCommand::Images;
  if (starts_with_command(normalized_command, "/image-width"))
    return TuiConfigurationCommand::ImageWidth;
  if (starts_with_command(normalized_command, "/cursor"))
    return TuiConfigurationCommand::Cursor;
  return TuiConfigurationCommand::None;
}

}  // namespace

std::optional<ava::core::Result<CommandResult>> run_tui_configuration_command(runtime::session_ts& unlocked_session, std::string_view normalized_command,
                                                                              std::vector<CommandHotkey> const& hotkeys)
{
  switch (classify_tui_configuration_command(normalized_command))
  {
    case TuiConfigurationCommand::Keybindings:
      return run_keybindings_command(unlocked_session, command_argument(normalized_command, "/hotkeys"), hotkeys);
    case TuiConfigurationCommand::Theme:
      return run_theme_command(unlocked_session, command_argument(normalized_command, "/theme"));
    case TuiConfigurationCommand::Images:
      return run_images_command(unlocked_session, command_argument(normalized_command, "/images"));
    case TuiConfigurationCommand::ImageWidth:
      return run_image_width_command(unlocked_session, command_argument(normalized_command, "/image-width"));
    case TuiConfigurationCommand::Cursor:
      return run_cursor_command(unlocked_session, command_argument(normalized_command, "/cursor"));
    case TuiConfigurationCommand::None:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace ava::app
