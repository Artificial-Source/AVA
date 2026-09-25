#include "sys.h"
#include "tests/app_runtime_test_declarations.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "ava/command/command.h"
#include "ava/app/commands.h"
#include "ava/app/display_settings.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/startup_overview.h"
#include "ava/agent/agent_loop.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime.h"
#include "ava/tui/terminal/Cursor.h"
#include "ava/tui/theme.h"
#include "ava/config/auth.h"
#include "ava/permissions/permission.h"
#include "ava/core/fingerprint.h"
#include "ava/core/json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

// Exercise UI command dispatcher behavior using unlocked_session as the runtime session and paths, workspace, and custom_hotkeys as fixture inputs.
//
// The command calls may mutate session-backed state and acquire their own access guards.
void app_command_dispatcher_ui_part(ava::app::runtime::session_ts& unlocked_session, ava::config::XdgPaths const& paths, std::filesystem::path const& workspace,
                                    std::vector<ava::app::CommandHotkey> const& custom_hotkeys)
{
  auto hotkeys = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/hotkeys", .hotkeys = custom_hotkeys});
  expect(hotkeys && hotkeys->handled && !hotkeys->output.empty() && hotkeys->output[0].find("Ctrl+M") != std::string::npos &&
             hotkeys->output[0].find("variant_cycle") != std::string::npos &&
             hotkeys->output[0].find("$XDG_CONFIG_HOME/ava/keybinds.json") != std::string::npos &&
             hotkeys->output[0].find("/reload keybindings") != std::string::npos,
         "command dispatcher /hotkeys reports effective keybind metadata");
  auto const default_hotkeys_text = ava::app::command_hotkeys_text({});
  auto const jump_line_pos = default_hotkeys_text.find("Jump to live tail");
  auto const jump_id_pos = default_hotkeys_text.find("jump_to_bottom");
  auto const mode_line_pos = default_hotkeys_text.find("Toggle build/plan mode");
  auto const mode_id_pos = default_hotkeys_text.find("mode_toggle");
  expect(jump_line_pos != std::string::npos && jump_id_pos != std::string::npos && jump_line_pos < jump_id_pos &&
             default_hotkeys_text.find("Ctrl+End", jump_line_pos) != std::string::npos && mode_line_pos != std::string::npos &&
             mode_id_pos != std::string::npos && mode_line_pos < mode_id_pos &&
             default_hotkeys_text.find("Submit input or select the highlighted slash command") == std::string::npos &&
             default_hotkeys_text.find("Return the transcript to the live tail") == std::string::npos,
         "command help/hotkeys dense text leads with human labels, keeps machine ids secondary, and drops long action descriptions");
  auto packages_disabled = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/packages list"});
  expect(packages_disabled && packages_disabled->handled && !packages_disabled->output.empty() &&
             packages_disabled->output[0].find("/packages is disabled") != std::string::npos &&
             packages_disabled->output[0].find("provenance") != std::string::npos,
         "command dispatcher recognizes deferred package-manager commands instead of sending them to the model");
  auto keybindings = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings", .hotkeys = custom_hotkeys});
  expect(keybindings && keybindings->handled && !keybindings->output.empty() && keybindings->output[0].find("Keybindings:") != std::string::npos &&
             keybindings->output[0].find("Ctrl+M") != std::string::npos && keybindings->output[0].find("/keybindings init") != std::string::npos &&
             keybindings->output[0].find("/keybindings import <path>") != std::string::npos &&
             keybindings->output[0].find("/keybindings set <action>") != std::string::npos &&
             keybindings->output[0].find("/keybindings reset <action>") != std::string::npos &&
             keybindings->output[0].find("/keybindings validate") != std::string::npos,
         "command dispatcher /keybindings aliases the effective keybinding discovery surface");
  auto keybindings_validate_missing = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings validate"});
  expect(keybindings_validate_missing && keybindings_validate_missing->handled && !keybindings_validate_missing->output.empty() &&
             keybindings_validate_missing->output[0].find("No keybindings file found") != std::string::npos &&
             keybindings_validate_missing->output[0].find("/keybindings init") != std::string::npos,
         "command dispatcher /keybindings validate reports missing config without failing closed");
  auto keybindings_init = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings init"});
  auto const keybinds_file = paths.ava_config_dir / "keybinds.json";
  auto const initialized_keybinds = ava::tui::load_key_bindings(keybinds_file);
  expect(keybindings_init && keybindings_init->handled && !keybindings_init->output.empty() &&
             keybindings_init->output[0].find("Created keybindings starter file") != std::string::npos &&
             keybindings_init->output[0].find(keybinds_file.string()) != std::string::npos && initialized_keybinds &&
             ava::tui::key_matches_action(*initialized_keybinds, ava::tui::TuiAction::Submit, ava::tui::Key::Enter),
         "command dispatcher /keybindings init writes a validated starter file to the runtime config dir");
  auto keybindings_validate = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings validate"});
  expect(keybindings_validate && keybindings_validate->handled && !keybindings_validate->output.empty() &&
             keybindings_validate->output[0].find("keybindings file is valid") != std::string::npos &&
             keybindings_validate->output[0].find(keybinds_file.string()) != std::string::npos &&
             keybindings_validate->output[0].find("/reload keybindings") != std::string::npos,
         "command dispatcher /keybindings validate checks the configured keybind file without reloading");
  {
    std::ofstream output(keybinds_file, std::ios::binary | std::ios::trunc);
    output << "{\"submit\":\"Ctrl+P\",\"model_cycle_forward\":\"Ctrl+P\"}\n";
  }
  auto invalid_keybindings_validate = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings validate"});
  expect(invalid_keybindings_validate && invalid_keybindings_validate->handled && !invalid_keybindings_validate->output.empty() &&
             invalid_keybindings_validate->output[0].find("keybindings file is invalid") != std::string::npos &&
             invalid_keybindings_validate->output[0].find("conflicting TUI keybinding") != std::string::npos &&
             invalid_keybindings_validate->output[0].find("Ctrl+P") != std::string::npos &&
             invalid_keybindings_validate->output[0].find(keybinds_file.string()) != std::string::npos,
         "command dispatcher /keybindings validate surfaces parser diagnostics with path context");
  auto unsupported_keybindings_validate = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings validate --bad"});
  expect(unsupported_keybindings_validate && unsupported_keybindings_validate->handled && !unsupported_keybindings_validate->output.empty() &&
             unsupported_keybindings_validate->output[0].find("unsupported keybindings validate option") != std::string::npos,
         "command dispatcher /keybindings validate reports unsupported options");
  auto keybindings_init_existing = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings init"});
  expect(keybindings_init_existing && keybindings_init_existing->handled && !keybindings_init_existing->output.empty() &&
             keybindings_init_existing->output[0].find("already exists") != std::string::npos &&
             keybindings_init_existing->output[0].find("--force") != std::string::npos,
         "command dispatcher /keybindings init refuses accidental overwrite");
  auto keybindings_init_force = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings init --force"});
  expect(keybindings_init_force && keybindings_init_force->handled && !keybindings_init_force->output.empty() &&
             keybindings_init_force->output[0].find("Replaced keybindings starter file") != std::string::npos,
         "command dispatcher /keybindings init --force replaces the starter file explicitly");
  auto read_keybinds_file = [&] {
    std::ifstream input(keybinds_file, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
  };
  auto const import_source = workspace / "import-keybinds.json";
  auto const valid_import_content = std::string("{\"tui.editor.cursorLeft\":[\"Left\",\"Alt+H\"],\"app.tools.expand\":\"Ctrl+O\"}\n");
  write_app_test_file(import_source, valid_import_content);
  auto keybindings_import_existing = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings import import-keybinds.json"});
  expect(keybindings_import_existing && keybindings_import_existing->handled && !keybindings_import_existing->output.empty() &&
             keybindings_import_existing->output[0].find("keybindings file already exists") != std::string::npos &&
             keybindings_import_existing->output[0].find("/keybindings import <path> --force") != std::string::npos,
         "command dispatcher /keybindings import refuses accidental overwrite");
  auto keybindings_import_force =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings import import-keybinds.json --force"});
  auto const imported_keybinds = ava::tui::load_key_bindings(keybinds_file);
  auto const installed_import_content = read_keybinds_file();
  expect(keybindings_import_force && keybindings_import_force->handled && !keybindings_import_force->output.empty() &&
             keybindings_import_force->output[0].find("Imported keybindings file") != std::string::npos &&
             keybindings_import_force->output[0].find(import_source.string()) != std::string::npos &&
             keybindings_import_force->output[0].find(keybinds_file.string()) != std::string::npos &&
             keybindings_import_force->output[0].find("/reload keybindings") != std::string::npos && installed_import_content == valid_import_content &&
             imported_keybinds && ava::tui::key_matches_action(*imported_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::Key::AltH) &&
             ava::tui::key_matches_action(*imported_keybinds, ava::tui::TuiAction::DetailsToggle, ava::tui::Key::CtrlO),
         "command dispatcher /keybindings import --force validates and installs a relative source file");
  auto keybindings_set = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings set cursor_left Alt+H"});
  auto const set_keybinds = ava::tui::load_key_bindings(keybinds_file);
  auto const set_content = read_keybinds_file();
  expect(keybindings_set && keybindings_set->handled && !keybindings_set->output.empty() &&
             keybindings_set->output[0].find("Set keybinding") != std::string::npos &&
             keybindings_set->output[0].find("action: cursor_left") != std::string::npos &&
             keybindings_set->output[0].find("keys: Alt+H") != std::string::npos &&
             keybindings_set->output[0].find("/reload keybindings") != std::string::npos &&
             set_content.find("\"tui.editor.cursorLeft\": \"Alt+H\"") != std::string::npos && set_content.find("\"cursor_left\"") == std::string::npos &&
             set_keybinds && ava::tui::key_matches_action(*set_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::Key::AltH) &&
             !ava::tui::key_matches_action(*set_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::Key::ArrowLeft),
         "command dispatcher /keybindings set validates, canonicalizes, and edits one action in keybinds.json");
  auto keybindings_set_multi = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings set cursor_left Left,Alt+H"});
  auto const set_multi_keybinds = ava::tui::load_key_bindings(keybinds_file);
  auto const set_multi_content = read_keybinds_file();
  expect(keybindings_set_multi && keybindings_set_multi->handled && !keybindings_set_multi->output.empty() &&
             keybindings_set_multi->output[0].find("keys: Left, Alt+H") != std::string::npos &&
             set_multi_content.find("\"tui.editor.cursorLeft\": [\"Left\", \"Alt+H\"]") != std::string::npos && set_multi_keybinds &&
             ava::tui::key_matches_action(*set_multi_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::Key::ArrowLeft) &&
             ava::tui::key_matches_action(*set_multi_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::Key::AltH),
         "command dispatcher /keybindings set accepts comma-separated key lists");
  auto const before_failed_set_content = read_keybinds_file();
  auto keybindings_set_conflict = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings set app.tools.expand Alt+H"});
  expect(keybindings_set_conflict && keybindings_set_conflict->handled && !keybindings_set_conflict->output.empty() &&
             keybindings_set_conflict->output[0].find("keybindings assignment is invalid") != std::string::npos &&
             keybindings_set_conflict->output[0].find("conflicting TUI keybinding") != std::string::npos &&
             keybindings_set_conflict->output[0].find("Target was not changed") != std::string::npos && read_keybinds_file() == before_failed_set_content,
         "command dispatcher /keybindings set validates the whole config before writing conflicts");
  auto keybindings_set_unknown_action = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings set no_such_action Alt+H"});
  expect(keybindings_set_unknown_action && keybindings_set_unknown_action->handled && !keybindings_set_unknown_action->output.empty() &&
             keybindings_set_unknown_action->output[0].find("unknown TUI keybinding action") != std::string::npos,
         "command dispatcher /keybindings set reports unknown actions");
  auto keybindings_set_unknown_key = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings set cursor_left Hyper+H"});
  expect(keybindings_set_unknown_key && keybindings_set_unknown_key->handled && !keybindings_set_unknown_key->output.empty() &&
             keybindings_set_unknown_key->output[0].find("unknown TUI key binding") != std::string::npos,
         "command dispatcher /keybindings set reports unknown keys");
  auto keybindings_reset = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings reset cursor_left"});
  auto const reset_keybinds = ava::tui::load_key_bindings(keybinds_file);
  auto const reset_content = read_keybinds_file();
  expect(keybindings_reset && keybindings_reset->handled && !keybindings_reset->output.empty() &&
             keybindings_reset->output[0].find("Reset keybinding override") != std::string::npos &&
             keybindings_reset->output[0].find("action: cursor_left") != std::string::npos &&
             keybindings_reset->output[0].find("/reload keybindings") != std::string::npos &&
             reset_content.find("tui.editor.cursorLeft") == std::string::npos && reset_content.find("cursor_left") == std::string::npos && reset_keybinds &&
             ava::tui::key_matches_action(*reset_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::Key::ArrowLeft) &&
             !ava::tui::key_matches_action(*reset_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::Key::AltH),
         "command dispatcher /keybindings reset removes equivalent action aliases and restores default bindings");
  auto keybindings_reset_missing = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings reset cursor_left"});
  expect(keybindings_reset_missing && keybindings_reset_missing->handled && !keybindings_reset_missing->output.empty() &&
             keybindings_reset_missing->output[0].find("No keybinding override found") != std::string::npos &&
             keybindings_reset_missing->output[0].find("Target was not changed") != std::string::npos,
         "command dispatcher /keybindings reset reports absent overrides without rewriting");
  auto keybindings_reset_unknown_action = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings reset no_such_action"});
  expect(keybindings_reset_unknown_action && keybindings_reset_unknown_action->handled && !keybindings_reset_unknown_action->output.empty() &&
             keybindings_reset_unknown_action->output[0].find("unknown TUI keybinding action") != std::string::npos,
         "command dispatcher /keybindings reset reports unknown actions");
  auto const invalid_import_source = workspace / "bad-keybinds.json";
  write_app_test_file(invalid_import_source, "{\"submit\":\"Ctrl+P\",\"model_cycle_forward\":\"Ctrl+P\"}\n");
  auto const before_invalid_import_content = read_keybinds_file();
  auto keybindings_import_invalid =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings import bad-keybinds.json --force"});
  expect(keybindings_import_invalid && keybindings_import_invalid->handled && !keybindings_import_invalid->output.empty() &&
             keybindings_import_invalid->output[0].find("keybindings import source is invalid") != std::string::npos &&
             keybindings_import_invalid->output[0].find("conflicting TUI keybinding") != std::string::npos &&
             keybindings_import_invalid->output[0].find("Target was not changed") != std::string::npos && read_keybinds_file() == before_invalid_import_content,
         "command dispatcher /keybindings import validates before replacing the target file");
  auto keybindings_import_missing =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings import missing-keybinds.json --force"});
  expect(keybindings_import_missing && keybindings_import_missing->handled && !keybindings_import_missing->output.empty() &&
             keybindings_import_missing->output[0].find("keybindings import source does not exist") != std::string::npos,
         "command dispatcher /keybindings import reports missing source files");
  auto unsupported_keybindings_import =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings import import-keybinds.json --bad"});
  expect(unsupported_keybindings_import && unsupported_keybindings_import->handled && !unsupported_keybindings_import->output.empty() &&
             unsupported_keybindings_import->output[0].find("unsupported keybindings import option") != std::string::npos,
         "command dispatcher /keybindings import reports unsupported options");
  auto unsupported_keybindings_init = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings init --bad"});
  expect(unsupported_keybindings_init && unsupported_keybindings_init->handled && !unsupported_keybindings_init->output.empty() &&
             unsupported_keybindings_init->output[0].find("unsupported keybindings init option") != std::string::npos,
         "command dispatcher /keybindings init reports unsupported options");
  auto const symlink_keybinds_target = workspace / "symlink-target-keybinds.json";
  write_app_test_file(symlink_keybinds_target, valid_import_content);
  std::error_code remove_keybinds_error;
  std::filesystem::remove(keybinds_file, remove_keybinds_error);
  std::error_code keybinds_symlink_error;
  std::filesystem::create_symlink(symlink_keybinds_target, keybinds_file, keybinds_symlink_error);
  if (!keybinds_symlink_error)
  {
    auto keybindings_init_symlink = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings init --force"});
    expect(keybindings_init_symlink && keybindings_init_symlink->handled && !keybindings_init_symlink->output.empty() &&
               keybindings_init_symlink->output[0].find("keybindings file target is a symlink") != std::string::npos,
           "command dispatcher /keybindings writes use atomic replacement and reject symlink targets");
    std::error_code cleanup_keybinds_symlink_error;
    std::filesystem::remove(keybinds_file, cleanup_keybinds_symlink_error);
  }
  auto keybindings_restore_after_symlink_test = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/keybindings init --force"});
  expect(keybindings_restore_after_symlink_test && keybindings_restore_after_symlink_test->handled && !keybindings_restore_after_symlink_test->output.empty() &&
             keybindings_restore_after_symlink_test->output[0].find("Replaced keybindings starter file") != std::string::npos,
         "command dispatcher /keybindings init restores a normal config file after symlink safety checks");
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "");
    ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
    ScopedEnvVar colorfgbg_guard("COLORFGBG", "");
    ava::tui::set_tui_config_theme(std::nullopt);
    auto theme_status = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/theme"});
    expect(theme_status && theme_status->handled && !theme_status->output.empty() && theme_status->output[0].find("TUI theme:") != std::string::npos &&
               theme_status->output[0].find("usage: /theme [dark|light|plain|custom-name|reset]") != std::string::npos,
           "command dispatcher /theme reports current config, active theme, and usage");
    auto theme_light = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/theme light"});
    auto loaded_theme = ava::app::load_tui_display_settings(paths);
    auto active_theme = ava::tui::active_tui_theme();
    expect(theme_light && theme_light->handled && !theme_light->output.empty() && theme_light->output[0].find("Stored TUI theme light") != std::string::npos &&
               loaded_theme && loaded_theme->theme && *loaded_theme->theme == "light" && active_theme.kind == ava::tui::TuiThemeKind::Light &&
               active_theme.badge == "display.json",
           "command dispatcher /theme light persists display.json and applies the active TUI theme override");
    write_app_test_file(paths.ava_config_dir / "themes" / "sunrise.json",
                        "{\n"
                        "  \"name\": \"sunrise\",\n"
                        "  \"vars\": {\"primary\": \"#0066cc\", \"paper\": 255},\n"
                        "  \"colors\": {\n"
                        "    \"text\": \"\",\n"
                        "    \"muted\": 242,\n"
                        "    \"success\": 34,\n"
                        "    \"warning\": \"#ffaa00\",\n"
                        "    \"error\": \"#ff0000\",\n"
                        "    \"accent\": \"primary\",\n"
                        "    \"screenBg\": \"paper\",\n"
                        "    \"composerBg\": 236,\n"
                        "    \"toolBg\": 235,\n"
                        "    \"questionBg\": 234\n"
                        "  }\n"
                        "}\n");
    write_app_test_file(
        paths.ava_config_dir / "themes" / "legacy.json",
        "{\n"
        "  \"name\": \"legacy\",\n"
        "  \"colors\": {\"text\":\"\",\"muted\":242,\"success\":34,\"warning\":220,\"error\":196,\"accent\":39,\"screenBg\":235,\"composerBg\":236}\n"
        "}\n");
    write_app_test_file(paths.ava_config_dir / "themes" / "broken.json",
                        "{\n"
                        "  \"name\": \"broken\",\n"
                        "  \"colors\": {\"text\":\"\",\"muted\":242,\"success\":34,\"warning\":220,\"error\":196,\"accent\":39,\"screenBg\":235}\n"
                        "}\n");
    auto custom_theme = ava::app::load_tui_custom_theme(paths, "sunrise");
    auto legacy_theme = ava::app::load_tui_custom_theme(paths, "legacy");
    auto invalid_custom_theme_file = ava::app::load_tui_custom_theme_file(paths.ava_config_dir / "themes" / "broken.json");
    expect(custom_theme && custom_theme->name == "sunrise" && custom_theme->palette.text == -1 && custom_theme->palette.screen_bg == 255 &&
               custom_theme->palette.composer_bg == 236 && custom_theme->palette.tool_bg == 235 && custom_theme->palette.question_bg == 234 && legacy_theme &&
               legacy_theme->palette.tool_bg == legacy_theme->palette.composer_bg && legacy_theme->palette.question_bg == legacy_theme->palette.composer_bg &&
               !invalid_custom_theme_file && invalid_custom_theme_file.error().format().find("composerBg") != std::string::npos,
           "display settings load custom tool/question backgrounds, preserve composer fallbacks, and reject incomplete custom themes with token context");
    auto theme_custom = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/theme sunrise"});
    loaded_theme = ava::app::load_tui_display_settings(paths);
    active_theme = ava::tui::active_tui_theme();
    expect(theme_custom && theme_custom->handled && !theme_custom->output.empty() &&
               theme_custom->output[0].find("Stored TUI theme sunrise") != std::string::npos && loaded_theme && loaded_theme->theme &&
               *loaded_theme->theme == "sunrise" && loaded_theme->custom_theme && active_theme.kind == ava::tui::TuiThemeKind::Custom &&
               active_theme.name == "sunrise" && active_theme.badge == "display.json" && active_theme.palette && active_theme.palette->composer_bg == 236 &&
               active_theme.palette->tool_bg == 235 && active_theme.palette->question_bg == 234,
           "command dispatcher /theme <custom> persists display.json and applies a valid custom TUI theme");
    auto custom_watch = ava::app::load_tui_display_settings_watch_state(paths);
    expect(custom_watch && custom_watch->theme && *custom_watch->theme == "sunrise" && custom_watch->custom_theme_revision,
           "display settings watch state records the selected custom theme revision");
    write_app_test_file(paths.ava_config_dir / "themes" / "sunrise.json",
                        "{\n"
                        "  \"name\": \"sunrise\",\n"
                        "  \"vars\": {\"primary\": \"#0066cc\", \"paper\": 255},\n"
                        "  \"colors\": {\n"
                        "    \"text\": \"\",\n"
                        "    \"muted\": 242,\n"
                        "    \"success\": 34,\n"
                        "    \"warning\": \"#ffaa00\",\n"
                        "    \"error\": \"#ff0000\",\n"
                        "    \"accent\": \"primary\",\n"
                        "    \"screenBg\": \"paper\",\n"
                        "    \"composerBg\": 237\n"
                        "  }\n"
                        "}\n");
    auto changed_custom_watch = ava::app::load_tui_display_settings_watch_state(paths);
    expect(custom_watch && changed_custom_watch && ava::app::tui_display_settings_watch_state_changed(*custom_watch, *changed_custom_watch) &&
               changed_custom_watch->custom_theme_revision != custom_watch->custom_theme_revision,
           "display settings watch state detects selected custom theme file edits by content revision");
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"plain\"\n}\n");
    auto changed_display_watch = ava::app::load_tui_display_settings_watch_state(paths);
    expect(changed_custom_watch && changed_display_watch && ava::app::tui_display_settings_watch_state_changed(*changed_custom_watch, *changed_display_watch) &&
               changed_display_watch->theme && *changed_display_watch->theme == "plain" && !changed_display_watch->custom_theme_revision,
           "display settings watch state detects display.json edits and clears custom theme tracking");

    // W2-002: while config is a built-in theme, edits to an unconfigured previewable custom theme must
    // still change the application-owned watch catalog so live preview can reload.
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"dark\"\n}\n");
    write_app_test_file(paths.ava_config_dir / "themes" / "sunrise.json",
                        "{\n"
                        "  \"name\": \"sunrise\",\n"
                        "  \"vars\": {\"primary\": \"#0066cc\", \"paper\": 255},\n"
                        "  \"colors\": {\n"
                        "    \"text\": \"\",\n"
                        "    \"muted\": 242,\n"
                        "    \"success\": 34,\n"
                        "    \"warning\": \"#ffaa00\",\n"
                        "    \"error\": \"#ff0000\",\n"
                        "    \"accent\": \"primary\",\n"
                        "    \"screenBg\": \"paper\",\n"
                        "    \"composerBg\": 236,\n"
                        "    \"toolBg\": 235,\n"
                        "    \"questionBg\": 234\n"
                        "  }\n"
                        "}\n");
    auto dark_with_sunrise_watch = ava::app::load_tui_display_settings_watch_state(paths);
    expect(dark_with_sunrise_watch && dark_with_sunrise_watch->theme && *dark_with_sunrise_watch->theme == "dark" &&
               !dark_with_sunrise_watch->custom_theme_revision &&
               std::ranges::any_of(dark_with_sunrise_watch->custom_theme_catalog,
                                   [](ava::app::TuiCustomThemeCatalogEntry const& entry) { return entry.name == "sunrise"; }),
           "watch catalog includes validated unconfigured custom themes while a built-in theme is configured");
    write_app_test_file(paths.ava_config_dir / "themes" / "sunrise.json",
                        "{\n"
                        "  \"name\": \"sunrise\",\n"
                        "  \"vars\": {\"primary\": \"#0066cc\", \"paper\": 255},\n"
                        "  \"colors\": {\n"
                        "    \"text\": \"\",\n"
                        "    \"muted\": 242,\n"
                        "    \"success\": 34,\n"
                        "    \"warning\": \"#ffaa00\",\n"
                        "    \"error\": \"#ff0000\",\n"
                        "    \"accent\": \"primary\",\n"
                        "    \"screenBg\": \"paper\",\n"
                        "    \"composerBg\": 238,\n"
                        "    \"toolBg\": 235,\n"
                        "    \"questionBg\": 234\n"
                        "  }\n"
                        "}\n");
    auto dark_sunrise_edited_watch = ava::app::load_tui_display_settings_watch_state(paths);
    expect(dark_with_sunrise_watch && dark_sunrise_edited_watch &&
               ava::app::tui_display_settings_watch_state_changed(*dark_with_sunrise_watch, *dark_sunrise_edited_watch) &&
               !dark_sunrise_edited_watch->custom_theme_revision,
           "watch state detects valid edits to an unconfigured previewable custom theme via catalog revision");
    auto sunrise_after_edit = ava::app::load_tui_custom_theme(paths, "sunrise");
    expect(sunrise_after_edit && sunrise_after_edit->palette.composer_bg == 238,
           "valid unconfigured custom theme edit reloads through ordinary theme discovery with the new palette");

    // Invalid edit drops the candidate from the catalog without making invalid bytes authoritative.
    write_app_test_file(paths.ava_config_dir / "themes" / "sunrise.json",
                        "{\n"
                        "  \"name\": \"sunrise\",\n"
                        "  \"colors\": {\"text\":\"\",\"muted\":242,\"success\":34,\"warning\":220,\"error\":196,\"accent\":39,\"screenBg\":235}\n"
                        "}\n");
    auto dark_sunrise_invalid_watch = ava::app::load_tui_display_settings_watch_state(paths);
    expect(dark_sunrise_edited_watch && dark_sunrise_invalid_watch &&
               ava::app::tui_display_settings_watch_state_changed(*dark_sunrise_edited_watch, *dark_sunrise_invalid_watch) &&
               std::ranges::none_of(dark_sunrise_invalid_watch->custom_theme_catalog,
                                    [](ava::app::TuiCustomThemeCatalogEntry const& entry) { return entry.name == "sunrise"; }),
           "invalid custom theme edits drop the candidate from the watch catalog without adopting invalid bytes");
    auto available_after_invalid = ava::app::available_tui_custom_themes(paths);
    expect(std::ranges::none_of(available_after_invalid, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "sunrise"; }),
           "invalid custom themes cannot be newly selected from validated discovery");

    // Confirm-after-invalid must not persist the newly invalid theme name; prior display.json is preserved.
    auto const display_before_confirm = (paths.ava_config_dir / "display.json");
    auto const prior_display_text = [&]() {
      std::ifstream in(display_before_confirm);
      std::ostringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }();
    auto confirm_invalid = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/theme sunrise"});
    auto const display_after_confirm = [&]() {
      std::ifstream in(display_before_confirm);
      std::ostringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }();
    auto loaded_after_failed_confirm = ava::app::load_tui_display_settings(paths);
    expect((!confirm_invalid ||
            (confirm_invalid->handled && !confirm_invalid->output.empty() &&
             (confirm_invalid->output[0].find("unsupported theme") != std::string::npos || confirm_invalid->output[0].find("invalid") != std::string::npos ||
              confirm_invalid->output[0].find("composerBg") != std::string::npos))) &&
               display_after_confirm == prior_display_text && loaded_after_failed_confirm && loaded_after_failed_confirm->theme &&
               *loaded_after_failed_confirm->theme == "dark",
           "confirm of a newly invalid custom theme fails closed and preserves prior display.json");

    // Restore a valid sunrise for subsequent tests in this fixture.
    write_app_test_file(paths.ava_config_dir / "themes" / "sunrise.json",
                        "{\n"
                        "  \"name\": \"sunrise\",\n"
                        "  \"vars\": {\"primary\": \"#0066cc\", \"paper\": 255},\n"
                        "  \"colors\": {\n"
                        "    \"text\": \"\",\n"
                        "    \"muted\": 242,\n"
                        "    \"success\": 34,\n"
                        "    \"warning\": \"#ffaa00\",\n"
                        "    \"error\": \"#ff0000\",\n"
                        "    \"accent\": \"primary\",\n"
                        "    \"screenBg\": \"paper\",\n"
                        "    \"composerBg\": 236,\n"
                        "    \"toolBg\": 235,\n"
                        "    \"questionBg\": 234\n"
                        "  }\n"
                        "}\n");

    // W2-002 boundary coverage for bounded deterministic custom-theme discovery/watch.
    {
      auto const themes_dir = paths.ava_config_dir / "themes";
      auto const valid_colors = std::string(
          "\"colors\": {\"text\":\"\",\"muted\":242,\"success\":34,\"warning\":220,\"error\":196,\"accent\":39,\"screenBg\":255,\"composerBg\":236}");
      auto write_valid_theme = [&](std::filesystem::path const& file, std::string const& name, std::size_t pad_bytes = 0) {
        std::string body = "{\"name\":\"" + name + "\"," + valid_colors;
        if (pad_bytes > 0)
          body += ",\"pad\":\"" + std::string(pad_bytes, 'y') + "\"";
        body += "}\n";
        write_app_test_file(file, body);
        return body.size();
      };

      // Oversized unconfigured file is rejected from discovery and must not fail built-in watch reload.
      write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"dark\"\n}\n");
      std::string oversized =
          std::string("{\"name\":\"huge\",") + valid_colors + ",\"pad\":\"" + std::string(ava::app::kMaxTuiCustomThemeFileBytes, 'x') + "\"}";
      write_app_test_file(themes_dir / "huge.json", oversized);
      auto oversized_file = ava::app::load_tui_custom_theme_file(themes_dir / "huge.json");
      auto dark_with_huge = ava::app::load_tui_display_settings_watch_state(paths);
      auto available_with_huge = ava::app::available_tui_custom_themes(paths);
      expect(!oversized_file && oversized_file.error().format().find("too large") != std::string::npos &&
                 oversized_file.error().format().find(std::to_string(ava::app::kMaxTuiCustomThemeFileBytes)) != std::string::npos && dark_with_huge &&
                 dark_with_huge->theme && *dark_with_huge->theme == "dark" &&
                 std::ranges::none_of(dark_with_huge->custom_theme_catalog,
                                      [](ava::app::TuiCustomThemeCatalogEntry const& entry) { return entry.name == "huge"; }) &&
                 std::ranges::none_of(available_with_huge, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "huge"; }) &&
                 std::ranges::any_of(available_with_huge, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "sunrise"; }),
             "oversized unconfigured custom theme is rejected with max_bytes context, skipped from catalog/listing, and does not fail built-in display watch");
      std::error_code remove_error;
      std::filesystem::remove(themes_dir / "huge.json", remove_error);

      // Exact per-file boundary: a complete file whose size equals the cap is accepted when valid.
      {
        std::string exact_prefix = std::string("{\"name\":\"exactcap\",") + valid_colors + ",\"pad\":\"";
        std::string exact_suffix = "\"}";
        auto const pad = ava::app::kMaxTuiCustomThemeFileBytes - exact_prefix.size() - exact_suffix.size();
        expect(pad > 0, "exact-cap fixture math stays positive");
        auto const exact_body = exact_prefix + std::string(pad, 'z') + exact_suffix;
        expect(exact_body.size() == ava::app::kMaxTuiCustomThemeFileBytes, "exact-cap fixture is precisely the per-file byte cap");
        write_app_test_file(themes_dir / "exact-cap.json", exact_body);
        auto exact_file = ava::app::load_tui_custom_theme_file(themes_dir / "exact-cap.json");
        auto exact_named = ava::app::load_tui_custom_theme(paths, "exactcap");
        auto exact_discovery = ava::app::discover_tui_custom_themes(paths);
        expect(exact_file && exact_file->name == "exactcap" && exact_named && exact_named->name == "exactcap" && exact_discovery.complete &&
                   std::ranges::any_of(exact_discovery.themes, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "exactcap"; }) &&
                   exact_discovery.aggregate_bytes_read >= ava::app::kMaxTuiCustomThemeFileBytes,
               "exact per-file byte-cap complete theme is accepted by file load, named load, and discovery with charged bytes");
        std::filesystem::remove(themes_dir / "exact-cap.json", remove_error);
      }

      // Below-cap valid complete file is accepted and charges physical bytes_read.
      // Mutation between pre/post fstat has no normal production seam for a deterministic test
      // (no production-only hook, no fixed sleeps, no blocking/probabilistic races); the reader
      // algorithm is conservatively evident: complete requires pre_size == post_size == bytes_read
      // below cap, and exact-cap requires stable pre/post equal to max_bytes. Boundary coverage
      // below relies on that classification plus known oversize / exact-cap / incomplete paths.
      {
        auto const below_bytes = write_valid_theme(themes_dir / "below-cap.json", "belowcap", 128);
        expect(below_bytes < ava::app::kMaxTuiCustomThemeFileBytes, "below-cap fixture stays under the per-file byte cap");
        auto const below_file = ava::app::load_tui_custom_theme_file(themes_dir / "below-cap.json");
        auto const below_named = ava::app::load_tui_custom_theme(paths, "belowcap");
        auto const below_discovery = ava::app::discover_tui_custom_themes(paths);
        expect(below_file && below_file->name == "belowcap" && below_named && below_named->name == "belowcap" && below_discovery.complete &&
                   std::ranges::any_of(below_discovery.themes, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "belowcap"; }) &&
                   below_discovery.aggregate_bytes_read >= below_bytes,
               "below-cap valid complete theme is accepted by file load, named load, and discovery with charged bytes");
        std::filesystem::remove(themes_dir / "below-cap.json", remove_error);
      }

      // Deterministic static +1 oversize: rejected from load/catalog (pre-fstat may charge 0).
      {
        std::string over_prefix = std::string("{\"name\":\"overcap\",") + valid_colors + ",\"pad\":\"";
        std::string over_suffix = "\"}";
        auto const pad = ava::app::kMaxTuiCustomThemeFileBytes - over_prefix.size() - over_suffix.size();
        expect(pad > 0, "over-cap fixture math stays positive");
        auto const exact_body = over_prefix + std::string(pad, 'w') + over_suffix;
        expect(exact_body.size() == ava::app::kMaxTuiCustomThemeFileBytes, "over-cap base body is exactly the per-file byte cap");
        auto const over_path = themes_dir / "over-cap.json";
        write_app_test_file(over_path, exact_body + "X");
        auto const static_over_file = ava::app::load_tui_custom_theme_file(over_path);
        auto const static_over_discovery = ava::app::discover_tui_custom_themes(paths);
        bool const static_listed =
            std::ranges::any_of(static_over_discovery.themes, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "overcap"; });
        expect(!static_over_file && static_over_file.error().format().find("too large") != std::string::npos && !static_listed &&
                   static_over_discovery.aggregate_bytes_read <= ava::app::kMaxTuiCustomThemeCatalogAggregateBytes,
               "known static +1 oversize is rejected from load/catalog without exceeding the aggregate cap");
        std::filesystem::remove(over_path, remove_error);
      }

      // Deterministic path-order duplicate-name policy: first normalized path wins listing; named load fails closed.
      write_app_test_file(themes_dir / "a-dup.json", "{\"name\":\"twin\"," + valid_colors + "}\n");
      write_app_test_file(themes_dir / "z-dup.json",
                          "{\"name\":\"twin\",\"colors\":{\"text\":\"\",\"muted\":242,\"success\":34,\"warning\":220,\"error\":196,\"accent\":39,"
                          "\"screenBg\":254,\"composerBg\":237}}\n");
      auto available_dups = ava::app::available_tui_custom_themes(paths);
      auto twin_load = ava::app::load_tui_custom_theme(paths, "twin");
      auto twin_entries =
          std::count_if(available_dups.begin(), available_dups.end(), [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "twin"; });
      auto const twin_it = std::ranges::find_if(available_dups, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "twin"; });
      expect(twin_entries == 1 && twin_it != available_dups.end() && twin_it->path.filename() == "a-dup.json" && twin_it->palette.composer_bg == 236 &&
                 !twin_load && twin_load.error().format().find("duplicate") != std::string::npos &&
                 twin_load.error().format().find("a-dup.json") != std::string::npos && twin_load.error().format().find("z-dup.json") != std::string::npos,
             "duplicate theme names keep the first normalized-path winner in listing and fail closed for configured/named load");
      std::filesystem::remove(themes_dir / "a-dup.json", remove_error);
      std::filesystem::remove(themes_dir / "z-dup.json", remove_error);

      // Deterministic name ordering for distinct validated themes.
      write_app_test_file(themes_dir / "m-mid.json", "{\"name\":\"mid\"," + valid_colors + "}\n");
      write_app_test_file(themes_dir / "c-early.json", "{\"name\":\"early\"," + valid_colors + "}\n");
      write_app_test_file(themes_dir / "t-late.json", "{\"name\":\"late\"," + valid_colors + "}\n");
      auto ordered = ava::app::available_tui_custom_themes(paths);
      std::vector<std::string> ordered_names;
      for (auto const& theme : ordered)
      {
        if (theme.name == "early" || theme.name == "late" || theme.name == "mid" || theme.name == "legacy" || theme.name == "sunrise")
          ordered_names.push_back(theme.name);
      }
      expect(ordered_names.size() >= 5 && std::is_sorted(ordered_names.begin(), ordered_names.end()),
             "validated custom theme discovery returns stable sorted name order");
      auto watch_ordered = ava::app::load_tui_display_settings_watch_state(paths);
      auto discovery_ordered = ava::app::discover_tui_custom_themes(paths);
      expect(watch_ordered && discovery_ordered.complete &&
                 std::is_sorted(watch_ordered->custom_theme_catalog.begin(), watch_ordered->custom_theme_catalog.end(),
                                [](ava::app::TuiCustomThemeCatalogEntry const& left, ava::app::TuiCustomThemeCatalogEntry const& right) {
                                  return left.name < right.name;
                                }) &&
                 watch_ordered->custom_theme_catalog.size() == discovery_ordered.themes.size() &&
                 std::equal(watch_ordered->custom_theme_catalog.begin(), watch_ordered->custom_theme_catalog.end(), discovery_ordered.themes.begin(),
                            [](ava::app::TuiCustomThemeCatalogEntry const& left, ava::app::TuiCustomThemeSummary const& right) {
                              return left.name == right.name && left.revision == right.revision;
                            }),
             "watch catalog preserves stable sorted name order and matches the single discovery snapshot");

      // Candidate-count cap + late duplicate beyond the cap must fail named/configured lookup.
      std::vector<std::filesystem::path> cap_files;
      write_valid_theme(themes_dir / "00-late-dup-first.json", "latecap");
      cap_files.push_back(themes_dir / "00-late-dup-first.json");
      for (std::size_t index = 0; index < ava::app::kMaxTuiCustomThemeCandidates + 4; ++index)
      {
        auto file = themes_dir / ("cap" + std::to_string(index) + ".json");
        write_valid_theme(file, "cap" + std::to_string(index));
        cap_files.push_back(file);
      }
      write_valid_theme(themes_dir / "zz-late-dup-second.json", "latecap");
      cap_files.push_back(themes_dir / "zz-late-dup-second.json");
      auto capped_discovery = ava::app::discover_tui_custom_themes(paths);
      auto late_dup_load = ava::app::load_tui_custom_theme(paths, "latecap");
      auto capped_listed = ava::app::available_tui_custom_themes(paths);
      expect(!capped_discovery.complete && capped_discovery.incomplete_reason == ava::app::TuiCustomThemeDiscoveryIncompleteReason::CandidateCap &&
                 capped_listed.size() <= ava::app::kMaxTuiCustomThemeCandidates &&
                 std::ranges::any_of(capped_listed, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "latecap"; }) && !late_dup_load &&
                 late_dup_load.error().format().find("discovery is incomplete") != std::string::npos &&
                 late_dup_load.error().format().find("candidate_cap") != std::string::npos &&
                 late_dup_load.error().format().find(std::to_string(ava::app::kMaxTuiCustomThemeCandidates)) != std::string::npos,
             "candidate cap keeps a bounded listing prefix but fail-closes named load when a late duplicate may exist beyond the boundary");
      for (auto const& file : cap_files) std::filesystem::remove(file, remove_error);

      // Aggregate read budget: observable bytes_read never exceeds the cap, and named load fails closed when incomplete.
      auto const chunk = ava::app::kMaxTuiCustomThemeFileBytes - 256;
      std::size_t expected_budget_files = (ava::app::kMaxTuiCustomThemeCatalogAggregateBytes / chunk) + 2;
      std::vector<std::filesystem::path> agg_files;
      std::size_t first_agg_bytes = 0;
      for (std::size_t index = 0; index < expected_budget_files; ++index)
      {
        auto name = "agg" + std::to_string(index);
        auto file = themes_dir / (name + ".json");
        auto const bytes = write_valid_theme(file, name, chunk);
        if (index == 0)
          first_agg_bytes = bytes;
        agg_files.push_back(file);
      }
      auto aggregate_discovery = ava::app::discover_tui_custom_themes(paths);
      auto aggregate_named = ava::app::load_tui_custom_theme(paths, "agg0");
      std::size_t aggregate_hits = 0;
      for (auto const& theme : aggregate_discovery.themes)
      {
        if (theme.name.rfind("agg", 0) == 0)
          ++aggregate_hits;
      }
      expect(!aggregate_discovery.complete && aggregate_discovery.incomplete_reason == ava::app::TuiCustomThemeDiscoveryIncompleteReason::AggregateBudget &&
                 aggregate_discovery.aggregate_bytes_read <= ava::app::kMaxTuiCustomThemeCatalogAggregateBytes &&
                 aggregate_discovery.aggregate_bytes_read >= first_agg_bytes && aggregate_hits < expected_budget_files && aggregate_hits >= 1 &&
                 !aggregate_named && aggregate_named.error().format().find("discovery is incomplete") != std::string::npos &&
                 aggregate_named.error().format().find("aggregate_budget") != std::string::npos &&
                 aggregate_named.error().format().find(std::to_string(ava::app::kMaxTuiCustomThemeCatalogAggregateBytes)) != std::string::npos,
             "aggregate discovery reports incomplete budget, never reads past the aggregate cap, and fail-closes named lookup for an early match");
      for (auto const& file : agg_files) std::filesystem::remove(file, remove_error);

      // Watch path: configured custom resolution and catalog fingerprint share one discovery snapshot.
      write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"sunrise\"\n}\n");
      auto watch_custom = ava::app::load_tui_display_settings_watch_state(paths);
      auto discovery_for_watch = ava::app::discover_tui_custom_themes(paths);
      auto sunrise_summary =
          std::ranges::find_if(discovery_for_watch.themes, [](ava::app::TuiCustomThemeSummary const& theme) { return theme.name == "sunrise"; });
      expect(watch_custom && discovery_for_watch.complete && watch_custom->theme && *watch_custom->theme == "sunrise" && watch_custom->custom_theme_revision &&
                 sunrise_summary != discovery_for_watch.themes.end() && *watch_custom->custom_theme_revision == sunrise_summary->revision &&
                 watch_custom->custom_theme_catalog.size() == discovery_for_watch.themes.size() &&
                 std::equal(watch_custom->custom_theme_catalog.begin(), watch_custom->custom_theme_catalog.end(), discovery_for_watch.themes.begin(),
                            [](ava::app::TuiCustomThemeCatalogEntry const& left, ava::app::TuiCustomThemeSummary const& right) {
                              return left.name == right.name && left.revision == right.revision;
                            }),
             "watch state configured custom revision and catalog fingerprint come from one consistent discovery result");

      write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"dark\"\n}\n");
      std::filesystem::remove(themes_dir / "m-mid.json", remove_error);
      std::filesystem::remove(themes_dir / "c-early.json", remove_error);
      std::filesystem::remove(themes_dir / "t-late.json", remove_error);

      // Valid edit detection + invalid removal/last-good stay covered above; re-assert built-in watch remains healthy.
      auto healthy = ava::app::load_tui_display_settings_watch_state(paths);
      expect(healthy && healthy->theme && *healthy->theme == "dark", "after boundary fixtures, configured built-in display watch reload remains healthy");
    }

    auto invalid_theme = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/theme sepia"});
    expect(invalid_theme && invalid_theme->handled && !invalid_theme->output.empty() &&
               invalid_theme->output[0].find("unsupported theme: sepia") != std::string::npos &&
               invalid_theme->output[0].find("dark|light|plain|custom-name|reset") != std::string::npos,
           "command dispatcher /theme rejects unsupported theme names with usage");
    auto reset_theme = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/theme reset"});
    auto reset_loaded_theme = ava::app::load_tui_display_settings(paths);
    active_theme = ava::tui::active_tui_theme();
    expect(reset_theme && reset_theme->handled && !reset_theme->output.empty() && reset_theme->output[0].find("Reset TUI theme") != std::string::npos &&
               reset_loaded_theme && !reset_loaded_theme->theme && active_theme.kind == ava::tui::TuiThemeKind::Dark && active_theme.badge == "built-in",
           "command dispatcher /theme reset clears the persisted TUI theme and returns to the built-in default");

    // Wave 1 display document: theme-only legacy files keep image defaults.
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"light\"\n}\n");
    auto legacy_defaults = ava::app::load_tui_display_settings(paths);
    expect(legacy_defaults && legacy_defaults->theme && *legacy_defaults->theme == "light" && legacy_defaults->show_images &&
               legacy_defaults->image_width_cells == 60 && !legacy_defaults->show_images_configured && !legacy_defaults->image_width_configured &&
               legacy_defaults->cursor.style() == ava::tui::terminal::CursorStyle::Default && legacy_defaults->cursor.blink() &&
               !legacy_defaults->cursor_style_configured && !legacy_defaults->cursor_blink_configured,
           "theme-only legacy display.json uses image and non-interfering cursor defaults");

    auto missing_defaults = ava::app::load_tui_display_settings(paths);
    std::error_code remove_display_error;
    std::filesystem::remove(paths.ava_config_dir / "display.json", remove_display_error);
    missing_defaults = ava::app::load_tui_display_settings(paths);
    expect(missing_defaults && !missing_defaults->theme && missing_defaults->show_images && missing_defaults->image_width_cells == 60 &&
               missing_defaults->cursor == ava::tui::terminal::CursorSettings{ava::tui::terminal::CursorStyle::Default},
           "missing display.json uses all effective image and cursor defaults");

    auto images_status = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/images"});
    expect(images_status && images_status->handled && !images_status->output.empty() && images_status->output[0].find("TUI images:") != std::string::npos &&
               images_status->output[0].find("usage: /images [on|off|reset]") != std::string::npos,
           "command dispatcher /images reports current visibility and usage");
    auto images_off = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/images off"});
    auto theme_after_images = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/theme dark"});
    auto width_set = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/image-width 80"});
    auto cursor_set = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/cursor underline steady"});
    auto cursor_style_only = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/cursor bar"});
    auto preserved = ava::app::load_tui_display_settings(paths);
    auto preserved_document = ava::app::load_display_settings_document(paths);
    expect(images_off && images_off->handled && theme_after_images && theme_after_images->handled && width_set && width_set->handled && cursor_set &&
               cursor_set->handled && !cursor_set->output.empty() && cursor_set->output[0].find("Stored TUI cursor underline steady") != std::string::npos &&
               cursor_style_only && cursor_style_only->handled && preserved && preserved->theme && *preserved->theme == "dark" && !preserved->show_images &&
               preserved->image_width_cells == 80 && preserved->cursor.style() == ava::tui::terminal::CursorStyle::Bar && !preserved->cursor.blink() &&
               preserved_document && preserved_document->theme && *preserved_document->theme == "dark" && preserved_document->show_images &&
               !*preserved_document->show_images && preserved_document->image_width_cells && *preserved_document->image_width_cells == 80 &&
               preserved_document->cursor_style == ava::tui::terminal::CursorStyle::Bar && preserved_document->cursor_blink &&
               !*preserved_document->cursor_blink,
           "alternating theme/image/cursor setters preserve every recognized field and omitted blink retains its prior value");

    write_app_test_file(paths.ava_config_dir / "display.json",
                        "{\n  \"theme\": \"light\",\n  \"show_images\": false,\n  \"image_width_cells\": 72,\n  \"cursor_style\": \"block\",\n  "
                        "\"cursor_blink\": false,\n  \"future_flag\": {\"nested\": true}\n}\n");
    auto with_unknown = ava::app::load_display_settings_document(paths);
    auto store_width = ava::app::store_tui_image_width_setting(paths, 96);
    auto store_cursor = ava::app::store_tui_cursor_setting(paths, ava::tui::terminal::CursorStyle::Underline, std::nullopt);
    auto after_unknown = ava::app::load_display_settings_document(paths);
    std::ifstream after_unknown_input(paths.ava_config_dir / "display.json", std::ios::binary);
    std::string after_unknown_text((std::istreambuf_iterator<char>(after_unknown_input)), std::istreambuf_iterator<char>());
    expect(with_unknown && with_unknown->unknown_fields.size() == 1 && with_unknown->unknown_fields.front().first == "future_flag" && store_width &&
               store_cursor && after_unknown && after_unknown->theme && *after_unknown->theme == "light" && after_unknown->show_images &&
               !*after_unknown->show_images && after_unknown->image_width_cells && *after_unknown->image_width_cells == 96 &&
               after_unknown->cursor_style == ava::tui::terminal::CursorStyle::Underline && after_unknown->cursor_blink && !*after_unknown->cursor_blink &&
               after_unknown->unknown_fields.size() == 1 && after_unknown->unknown_fields.front().first == "future_flag" &&
               after_unknown_text.find("\"future_flag\": {\"nested\": true}") != std::string::npos,
           "recognized cursor fields and raw unknown display.json fields survive field-specific updates");

    auto reset_images = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/images reset"});
    auto reset_width = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/image-width reset"});
    auto after_resets = ava::app::load_display_settings_document(paths);
    expect(reset_images && reset_images->handled && reset_width && reset_width->handled && after_resets && after_resets->theme &&
               *after_resets->theme == "light" && !after_resets->show_images && !after_resets->image_width_cells &&
               after_resets->cursor_style == ava::tui::terminal::CursorStyle::Underline && after_resets->cursor_blink && !*after_resets->cursor_blink &&
               after_resets->unknown_fields.size() == 1,
           "each image reset removes only its key and preserves theme, cursor, and unknown fields");

    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"light\",\n  \"show_images\": \"yes\"\n}\n");
    auto invalid_show = ava::app::load_tui_display_settings(paths);
    auto rejected_store = ava::app::store_tui_theme_setting(paths, "dark");
    std::ifstream invalid_input(paths.ava_config_dir / "display.json", std::ios::binary);
    std::string invalid_text((std::istreambuf_iterator<char>(invalid_input)), std::istreambuf_iterator<char>());
    expect(!invalid_show && invalid_show.error().format().find("show_images") != std::string::npos && !rejected_store &&
               invalid_text.find("\"show_images\": \"yes\"") != std::string::npos,
           "malformed recognized display fields reject load/update and leave the file unchanged");

    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"cursor_style\": \"Block\"\n}\n");
    auto invalid_cursor_style = ava::app::load_tui_display_settings(paths);
    auto rejected_cursor_store = ava::app::store_tui_cursor_setting(paths, ava::tui::terminal::CursorStyle::Bar, true);
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"cursor_blink\": \"yes\"\n}\n");
    auto invalid_cursor_blink = ava::app::load_tui_display_settings(paths);
    expect(!invalid_cursor_style && invalid_cursor_style.error().format().find("cursor_style") != std::string::npos && !rejected_cursor_store &&
               !invalid_cursor_blink && invalid_cursor_blink.error().format().find("cursor_blink") != std::string::npos,
           "malformed recognized cursor fields fail closed and block field-specific updates");

    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"cursor_style\": \"block\",\n  \"cursor_blink\": true\n}\n");
    auto cursor_watch_before = ava::app::load_tui_display_settings_watch_state(paths);
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"cursor_style\": \"bar\",\n  \"cursor_blink\": false\n}\n");
    auto cursor_watch_after = ava::app::load_tui_display_settings_watch_state(paths);
    expect(cursor_watch_before && cursor_watch_after && ava::app::tui_display_settings_watch_state_changed(*cursor_watch_before, *cursor_watch_after) &&
               cursor_watch_after->cursor.style() == ava::tui::terminal::CursorStyle::Bar && !cursor_watch_after->cursor.blink(),
           "display reload watch detects persistent cursor style and blink changes");

    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"image_width_cells\": 7\n}\n");
    auto invalid_width_low = ava::app::load_tui_display_settings(paths);
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"image_width_cells\": 161\n}\n");
    auto invalid_width_high = ava::app::load_tui_display_settings(paths);
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"image_width_cells\": 60.5\n}\n");
    auto invalid_width_float = ava::app::load_tui_display_settings(paths);
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"image_width_cells\": true\n}\n");
    auto invalid_width_bool = ava::app::load_tui_display_settings(paths);
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": 1\n}\n");
    auto invalid_theme_type = ava::app::load_tui_display_settings(paths);
    expect(!invalid_width_low && !invalid_width_high && !invalid_width_float && !invalid_width_bool && !invalid_theme_type &&
               invalid_theme_type.error().format().find("theme") != std::string::npos,
           "display settings reject out-of-range, float, bool, and wrong-type recognized fields");

    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"light\"\n}\n");
    auto bad_width_command = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/image-width 200"});
    expect(bad_width_command && bad_width_command->handled && !bad_width_command->output.empty() &&
               bad_width_command->output[0].find("usage: /image-width <8..160>|reset") != std::string::npos,
           "command dispatcher /image-width rejects out-of-range widths with usage");
    auto bad_cursor_style = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/cursor beam"});
    auto bad_cursor_blink = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/cursor block pulse"});
    auto default_cursor = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/cursor default blink"});
    auto default_cursor_settings = ava::app::load_tui_display_settings(paths);
    expect(bad_cursor_style && bad_cursor_style->handled && !bad_cursor_style->output.empty() &&
               bad_cursor_style->output[0].find("usage: /cursor default|block|underline|bar [blink|steady]") != std::string::npos && bad_cursor_blink &&
               bad_cursor_blink->handled && !bad_cursor_blink->output.empty() &&
               bad_cursor_blink->output[0].find("unsupported cursor blink mode") != std::string::npos && default_cursor && default_cursor->handled &&
               default_cursor_settings && default_cursor_settings->cursor == ava::tui::terminal::CursorSettings{ava::tui::terminal::CursorStyle::Default},
           "command dispatcher validates cursor arguments and persists the non-interfering default");
  }
  auto details = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/details"});
  expect(details && details->handled && !details->output.empty() && details->output[0].find("default to Rich") != std::string::npos &&
             details->output[0].find("/details compact") != std::string::npos,
         "command dispatcher documents Rich default and explicit tool-card presentation modes");
  auto sidebar = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sidebar"});
  expect(sidebar && sidebar->handled && !sidebar->output.empty() && sidebar->output[0].find("interactive TUI") != std::string::npos &&
             sidebar->output[0].find("session overview") != std::string::npos && sidebar->output[0].find("session_id") == std::string::npos &&
             sidebar->output[0].find("workspace") == std::string::npos,
         "command dispatcher describes /sidebar as a TUI-only view without inventing sidebar data");
  auto search = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/search Unicode literal"});
  expect(search && search->handled && !search->output.empty() && search->output[0].find("only inside the interactive TUI") != std::string::npos &&
             search->output[0].find("currently rendered transcript items") != std::string::npos,
         "headless command dispatcher truthfully identifies transcript search as a TUI-only rendered view");
  auto tool = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/tools write"});
  expect(tool && tool->handled && !tool->output.empty() && tool->output[0].find("/tool [query] to toggle the latest or matching card") != std::string::npos &&
             tool->output[0].find("inherited non-expanded view and Expanded") != std::string::npos &&
             tool->output[0].find("/copy tool [query] to copy safe tool details") != std::string::npos,
         "command dispatcher recognizes /tool and /tools as TUI tool-card inspection commands");
  auto diff = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/diff src/main.cpp"});
  expect(diff && diff->handled && !diff->output.empty() &&
             diff->output[0].find("/diff [query] to show the latest or matching unified diff") != std::string::npos &&
             diff->output[0].find("/copy diff [query] to copy it") != std::string::npos,
         "command dispatcher recognizes filtered /diff as a TUI transcript inspection command");
  auto copy = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/copy tool"});
  expect(copy && copy->handled && !copy->output.empty() && copy->output[0].find("/copy for the latest AVA message") != std::string::npos &&
             copy->output[0].find("/copy user [query] to pick a public user turn") != std::string::npos &&
             copy->output[0].find("/copy tool [query] for tool details") != std::string::npos &&
             copy->output[0].find("/copy diff [query] for unified diffs") != std::string::npos &&
             copy->output[0].find("/copy permission [query] for permission audit details") != std::string::npos,
         "command dispatcher recognizes /copy as a TUI clipboard command");
  auto copy_user = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/copy user"});
  expect(copy_user && copy_user->handled && !copy_user->output.empty() &&
             copy_user->output[0].find("/copy user [query] to pick a public user turn") != std::string::npos,
         "command dispatcher recognizes exact /copy user as a TUI user-turn clipboard target");
  auto copy_diff = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/copy diff src/main.cpp"});
  expect(
      copy_diff && copy_diff->handled && !copy_diff->output.empty() && copy_diff->output[0].find("/copy diff [query] for unified diffs") != std::string::npos,
      "command dispatcher recognizes filtered /copy diff as a TUI clipboard command");
  auto copy_permission = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/copy permission git push"});
  expect(copy_permission && copy_permission->handled && !copy_permission->output.empty() &&
             copy_permission->output[0].find("/copy permission [query] for permission audit details") != std::string::npos,
         "command dispatcher recognizes filtered /copy permission as a TUI clipboard command");
  auto unsupported_copy = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/copy branch"});
  expect(unsupported_copy && unsupported_copy->handled && !unsupported_copy->output.empty() &&
             unsupported_copy->output[0].find("unsupported copy target: branch") != std::string::npos &&
             unsupported_copy->output[0].find("supported: user [query], tool [query], diff [query], permission [query]") != std::string::npos,
         "command dispatcher reports unsupported copy targets");
  auto fork_from = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/fork-from"});
  expect(fork_from && fork_from->handled && !fork_from->output.empty() && fork_from->output[0].find("interactive TUI") != std::string::npos &&
             fork_from->output[0].find("/fork-from") != std::string::npos && fork_from->output[0].find("/fork [name]") != std::string::npos,
         "command dispatcher recognizes /fork-from as a TUI user-turn fork picker");
  auto thinking = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/thinking"});
  expect(thinking && thinking->handled && !thinking->output.empty() && thinking->output[0].find("TUI display toggle") != std::string::npos &&
             thinking->output[0].find("does not change provider reasoning mode") != std::string::npos &&
             thinking->output[0].find("/thinking details") != std::string::npos,
         "command dispatcher recognizes /thinking as display-only instead of changing backend reasoning mode");
  auto thinking_details = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/thinking details"});
  expect(
      thinking_details && thinking_details->handled && !thinking_details->output.empty() && thinking_details->output[0].find("Thinking:") != std::string::npos,
      "command dispatcher documents /thinking details as the per-item expand fallback");
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "");
    ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
    ScopedEnvVar colorfgbg_guard("COLORFGBG", "");
    ava::tui::set_tui_config_theme(std::nullopt);
    write_app_test_file(paths.ava_config_dir / "display.json", "{\n  \"theme\": \"plain\"\n}\n");
    auto reload_theme = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload theme"});
    auto active_theme = ava::tui::active_tui_theme();
    expect(reload_theme && reload_theme->handled && !reload_theme->output.empty() && reload_theme->output[0].find("Reload report:") != std::string::npos &&
               reload_theme->output[0].find("display: loaded") != std::string::npos && reload_theme->output[0].find("configured: plain") != std::string::npos &&
               active_theme.kind == ava::tui::TuiThemeKind::Plain && active_theme.badge == "display.json",
           "command dispatcher /reload theme applies externally edited display.json without restarting");
    ava::tui::set_tui_config_theme(std::nullopt);
  }
  auto unsupported_reload = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload no-such"});
  expect(
      unsupported_reload && unsupported_reload->handled && !unsupported_reload->output.empty() &&
          unsupported_reload->output[0].find("unsupported reload target: no-such") != std::string::npos &&
          unsupported_reload->output[0].find(
              "supported: all, theme, models, prompts, trust, compaction, keybindings, auth, providers, permissions, lsp, mcp, plugins") != std::string::npos,
      "command dispatcher reports unsupported reload targets");
  auto help = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/help", .hotkeys = custom_hotkeys});
  expect(help && help->handled && !help->output.empty() && help->output[0].find("/hotkeys") != std::string::npos &&
             help->output[0].find("/keybindings") != std::string::npos && help->output[0].find("/sidebar") != std::string::npos &&
             help->output[0].find("/search") != std::string::npos && help->output[0].find("/connect") != std::string::npos &&
             help->output[0].find("/plugins") != std::string::npos && help->output[0].find("!<command>") != std::string::npos &&
             help->output[0].find("!!<command>") != std::string::npos && help->output[0].find("Unavailable commands") != std::string::npos &&
             help->output[0].find("Ctrl+M") != std::string::npos,
         "command dispatcher /help includes catalog commands and effective hotkeys");

  int shell_prompts = 0;
  auto const shell_resolver =
      [&shell_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    ++shell_prompts;
    expect(prompt.command_metadata && prompt.command_metadata->level == ava::command::CommandLevel::Critical &&
               prompt.command_metadata->family == ava::command::CommandFamily::RawShell &&
               prompt.command_metadata->execution_domain == ava::command::CommandExecutionDomain::RawShell &&
               prompt.command_metadata->backend_maximum_scope == ava::command::InteractiveScope::Once &&
               !ava::permissions::command_permission_allows_reusable_grant(*prompt.command_metadata),
           "Pi-style shell helper receives a critical one-shot raw-shell prompt");
    return ava::permissions::PermissionResolution::Allow;
  };
  auto bang_shell = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "!pwd", .permission_resolver = shell_resolver});
  expect(bang_shell && bang_shell->handled && bang_shell->tool_timeline.size() == 2 && bang_shell->tool_timeline[0].name == "bash" &&
             bang_shell->tool_timeline[0].argument_summary == "<redacted one-shot command>" &&
             bang_shell->tool_timeline[1].status == ava::agent::ToolTimelineStatus::Success && !bang_shell->output.empty() &&
             bang_shell->output[0].find("exit: 0") != std::string::npos,
         "Pi-style ! shell helper runs through the permissioned bash command path");
  auto hidden_bang_shell = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "!! pwd", .permission_resolver = shell_resolver});
  expect(hidden_bang_shell && hidden_bang_shell->handled && hidden_bang_shell->tool_timeline.size() == 2 &&
             hidden_bang_shell->tool_timeline[0].name == "bash" && hidden_bang_shell->tool_timeline[0].argument_summary == "<redacted one-shot command>" &&
             hidden_bang_shell->tool_timeline[1].status == ava::agent::ToolTimelineStatus::Success && shell_prompts == 2,
         "Pi-style !! shell helper is accepted as the critical one-shot bash helper without bypassing permissions");
  std::optional<ava::permissions::PermissionPrompt> explicit_bash_prompt;
  auto explicit_bash = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/bash git status",
                                                 .permission_resolver = [&explicit_bash_prompt](ava::permissions::PermissionPrompt const& prompt)
                                                     -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                                   explicit_bash_prompt = prompt;
                                                   return ava::permissions::PermissionResolution::Deny;
                                                 }});
  expect(explicit_bash && explicit_bash->handled && explicit_bash_prompt && explicit_bash_prompt->command_metadata &&
             explicit_bash_prompt->command_metadata->level == ava::command::CommandLevel::Critical &&
             explicit_bash_prompt->command_metadata->family == ava::command::CommandFamily::RawShell &&
             explicit_bash_prompt->command_metadata->execution_domain == ava::command::CommandExecutionDomain::RawShell &&
             explicit_bash_prompt->command_metadata->backend_maximum_scope == ava::command::InteractiveScope::Once &&
             !ava::permissions::command_permission_allows_reusable_grant(*explicit_bash_prompt->command_metadata) && !explicit_bash->tool_timeline.empty() &&
             explicit_bash->tool_timeline.back().status == ava::agent::ToolTimelineStatus::Error,
         "/bash git status is an explicit Critical raw-shell prompt and cannot create a reusable grant");
  auto missing_bang_shell = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "!"});
  expect(missing_bang_shell && missing_bang_shell->handled && !missing_bang_shell->output.empty() &&
             missing_bang_shell->output[0].find("!<command> or !!<command>") != std::string::npos,
         "empty shell helper reports the expected usage");
  auto find_alias = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/find src/*.cpp"});
  expect(find_alias && find_alias->handled && find_alias->tool_timeline.size() == 2 && find_alias->tool_timeline[0].name == "find" &&
             find_alias->tool_timeline[1].result_json.find("\"tool\":\"glob\"") != std::string::npos && !find_alias->output.empty() &&
             find_alias->output[0].find("src/main.cpp") != std::string::npos,
         "Pi-style /find alias runs through the native glob tool path");
  auto ls_alias = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/ls src"});
  expect(ls_alias && ls_alias->handled && ls_alias->tool_timeline.size() == 2 && ls_alias->tool_timeline[0].name == "ls" &&
             ls_alias->tool_timeline[1].result_json.find("\"tool\":\"list_directory\"") != std::string::npos && !ls_alias->output.empty() &&
             ls_alias->output[0].find("main.cpp") != std::string::npos,
         "Pi-style /ls alias runs through the native list_directory tool path");
  auto missing_find_alias = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/find"});
  expect(missing_find_alias && missing_find_alias->handled && !missing_find_alias->output.empty() &&
             missing_find_alias->output[0].find("/find <pattern>") != std::string::npos,
         "empty /find alias reports Pi-style usage instead of the native /glob name");

  auto plugins_usage = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins"});
  expect(plugins_usage && plugins_usage->handled && !plugins_usage->output.empty() && plugins_usage->output[0].find("usage: /plugins") != std::string::npos,
         "command dispatcher /plugins without a subcommand reports usage");
  auto plugins = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins list"});
  expect(plugins && plugins->handled && !plugins->output.empty() && plugins->output[0].find("com.example.global") != std::string::npos &&
             plugins->output[0].find("com.example.project") != std::string::npos && plugins->output[0].find("Failures: 1") != std::string::npos,
         "command dispatcher /plugins list reports discovered plugins and diagnostics failures");
  auto inspect_plugin = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins inspect com.example.project"});
  expect(inspect_plugin && inspect_plugin->handled && !inspect_plugin->output.empty() &&
             inspect_plugin->output[0].find("entrypoint: node plugin.js --safe (not executed)") != std::string::npos &&
             inspect_plugin->output[0].find("no plugin process is started yet") != std::string::npos,
         "command dispatcher /plugins inspect shows manifest details without executing entrypoints");
  auto enable_plugin = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins enable com.example.project"});
  expect(enable_plugin && enable_plugin->handled && !enable_plugin->output.empty() &&
             enable_plugin->output[0].find("Enabled project plugin com.example.project") != std::string::npos &&
             enable_plugin->output[0].find("No plugin process was started") != std::string::npos,
         "command dispatcher /plugins enable records state without starting plugin processes");
  auto plugins_after_enable = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins list"});
  expect(plugins_after_enable && plugins_after_enable->handled && !plugins_after_enable->output.empty() &&
             plugins_after_enable->output[0].find("com.example.project  enabled") != std::string::npos,
         "command dispatcher /plugins list reflects enablement state");

  auto overview_cmd = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/overview"});
  expect(overview_cmd && overview_cmd->handled && !overview_cmd->output.empty() && overview_cmd->output[0].find("/overview") != std::string::npos &&
             overview_cmd->output[0].find("interactive TUI") != std::string::npos,
         "command dispatcher /overview is a TUI-owned view with a non-mutating headless notice");
}

void test_startup_overview_snapshot_bounds_order_redaction()
{
  using ava::app::build_startup_overview_snapshot;
  using ava::app::kMaxStartupOverviewInputSources;
  using ava::app::kMaxStartupOverviewLabelBytes;
  using ava::app::kMaxStartupOverviewNamedItems;
  using ava::app::kMaxStartupOverviewResourceGroups;
  using ava::app::StartupOverviewBuildInput;

  auto default_bindings = ava::tui::default_key_bindings();
  std::vector<ava::app::runtime::ContextSourceMetadata> context_sources;
  std::vector<ava::app::runtime::FreshnessSourceMetadata> freshness_sources;
  for (std::size_t i = 0; i < kMaxStartupOverviewInputSources + 8; ++i)
  {
    context_sources.push_back(ava::app::runtime::ContextSourceMetadata{
        .path = std::filesystem::path("/private/home/user/.ava/AGENTS.md"),
        .source_type = ava::context::ContextSourceType::Global,
        .byte_count = 12,
        .content_fingerprint = 1 + i,
    });
  }

  auto push_freshness = [&](ava::app::runtime::FreshnessSourceKind kind, std::string scope, std::string source_id, std::string name, std::size_t bytes,
                            std::uint64_t fingerprint) {
    freshness_sources.push_back(ava::app::runtime::FreshnessSourceMetadata{.kind = kind,
                                                                           .scope = std::move(scope),
                                                                           .source_id = std::move(source_id),
                                                                           .name = std::move(name),
                                                                           .path = {},
                                                                           .byte_count = bytes,
                                                                           .content_fingerprint = fingerprint});
  };
  for (auto const* name : {"zeta-skill", "alpha-skill", "zeta-skill", "/tmp/evil", "api_key_holder"})
    push_freshness(ava::app::runtime::FreshnessSourceKind::Skill, "global", name, name, 4, 42);
  push_freshness(ava::app::runtime::FreshnessSourceKind::PluginManifest, "project", "com.example.plugin", "manifest", 8, 7);
  push_freshness(ava::app::runtime::FreshnessSourceKind::PluginPrompt, "project", "com.example.plugin", "broken-prompt", 0, 0);
  push_freshness(ava::app::runtime::FreshnessSourceKind::PluginSkill, "project", "com.example.plugin", "empty-ok", 0, ava::core::content_fingerprint(""));
  auto long_name = std::string(kMaxStartupOverviewLabelBytes + 20, 'x');
  push_freshness(ava::app::runtime::FreshnessSourceKind::PromptCommand, "global", long_name, long_name, 3, 9);

  // Multibyte / invalid sequences that would be split by naive byte resize.
  std::string const utf8_cross = std::string(kMaxStartupOverviewLabelBytes - 1, 'a') + "\xE4\xBD\xA0";  // 你 straddles the byte cap
  std::string const invalid_mid = std::string("ok") + std::string("\xC3", 1) + std::string(kMaxStartupOverviewLabelBytes, 'z');
  std::string const control_mid = std::string("hi\x1b[31m") + std::string(kMaxStartupOverviewLabelBytes, 'q');
  push_freshness(ava::app::runtime::FreshnessSourceKind::Skill, "global", "utf8-cross", utf8_cross, 4, 99);
  push_freshness(ava::app::runtime::FreshnessSourceKind::Skill, "global", "invalid-mid", invalid_mid, 4, 100);
  push_freshness(ava::app::runtime::FreshnessSourceKind::Skill, "global", "control-mid", control_mid, 4, 101);

  StartupOverviewBuildInput input{
      .mode = "build\x1b[31m",
      .provider = "openai",
      .model = "/secret/path/model",
      .trust_decision = "trusted",
      .project_resources = "enabled",
      .context_sources = context_sources,
      .freshness_sources = freshness_sources,
      .theme_name = "ava-dark",
      .theme_badge = "built-in",
      .key_bindings = &default_bindings,
  };

  auto const first = build_startup_overview_snapshot(input);
  auto const second = build_startup_overview_snapshot(input);
  expect(first.compact_line == second.compact_line && first.detail_line == second.detail_line && first.skill_names == second.skill_names &&
             first.plugin_ids == second.plugin_ids && first.resource_groups.size() == second.resource_groups.size(),
         "startup overview builder is deterministic for identical inputs");
  expect(first.model.empty(), "path-like model labels are redacted from the overview snapshot");
  expect(first.compact_line.find("/secret") == std::string::npos && first.compact_line.find("api_key") == std::string::npos &&
             first.compact_line.find("AGENTS.md") == std::string::npos && first.compact_line.find("/private") == std::string::npos &&
             first.compact_line.find("\x1b") == std::string::npos,
         "compact overview card never includes private leaves, paths, secrets, or terminal controls");
  expect(first.instruction_source_count == context_sources.size(),
         "overview reports the truthful instruction-source total without pretending the work cap is the total");
  expect(first.compact_line.find(std::to_string(context_sources.size()) + " ctx") != std::string::npos &&
             first.compact_line.find(std::to_string(context_sources.size()) + "+ ctx") == std::string::npos,
         "overview compact chrome keeps the O(1) instruction total exact (no N+) when only group aggregation was capped");
  expect(first.resource_groups.size() <= kMaxStartupOverviewResourceGroups, "overview bounds total resource groups");
  auto instruction_group_count = std::size_t{0};
  bool instruction_groups_lower_bound = false;
  for (auto const& group : first.resource_groups)
  {
    if (group.kind == "instruction")
    {
      instruction_group_count += group.count;
      instruction_groups_lower_bound = instruction_groups_lower_bound || group.count_is_lower_bound;
      expect(group.count_is_lower_bound, "capped context input marks every instruction group count as a lower bound");
    }
  }
  expect(instruction_group_count == kMaxStartupOverviewInputSources && instruction_groups_lower_bound,
         "overview instruction grouping work is hard-capped and surfaces lower-bound metadata when the truthful total is larger");
  expect(first.skill_names.size() <= kMaxStartupOverviewNamedItems && first.skill_names.size() >= 2 &&
             std::ranges::find(first.skill_names, "alpha-skill") != first.skill_names.end() &&
             std::ranges::find(first.skill_names, "zeta-skill") != first.skill_names.end() && std::ranges::is_sorted(first.skill_names) &&
             std::ranges::none_of(first.skill_names,
                                  [](std::string const& name) { return name.find('/') != std::string::npos || name.find("api_key") != std::string::npos; }),
         "overview skill names are unique, sorted, redacted, and bounded");
  expect(first.plugin_ids.size() == 1 && first.plugin_ids.front() == "com.example.plugin", "overview plugin ids stay path-free and deduped");
  expect(first.plugin_resource_failure_count && *first.plugin_resource_failure_count == 1 && !first.plugin_resource_failure_count_is_lower_bound,
         "overview counts only retained dual-zero plugin resource failures without a lower-bound flag when freshness fit in the cap");
  expect(!first.prompt_command_names.empty() && first.prompt_command_names.front().size() <= kMaxStartupOverviewLabelBytes &&
             first.prompt_command_names.front().ends_with("..."),
         "overview labels are byte-bounded with an in-budget ellipsis");
  for (auto const& name : first.skill_names)
  {
    expect(name.size() <= kMaxStartupOverviewLabelBytes && ava::core::json::is_valid_utf8(name),
           "overview skill labels stay within the byte cap as valid UTF-8");
    expect(!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0U) != 0xC0U, "overview labels never end on a split multibyte lead");
  }
  auto const utf8_skill = std::ranges::find_if(first.skill_names, [](std::string const& name) { return name.ends_with("..."); });
  expect(utf8_skill != first.skill_names.end() && utf8_skill->size() == kMaxStartupOverviewLabelBytes &&
             utf8_skill->find("\xE4\xBD\xA0") == std::string::npos && utf8_skill->find('\x1b') == std::string::npos,
         "overview UTF-8/control-crossing labels truncate on a codepoint boundary and stay terminal-safe");
  expect(!first.key_hints.empty() && first.key_hints.front().label == "overview" && first.key_hints.front().keys == "/overview",
         "unbound overview toggle still surfaces the /overview key hint");
  expect(first.overview_toggle_keys.empty(), "default bindings leave app.overview.toggle unbound");

  auto bound = ava::tui::parse_key_bindings_json(R"({"app.overview.toggle":"F6"})");
  expect(bound.has_value(), "overview toggle keybinding parses");
  input.key_bindings = &(*bound);
  input.model = "safe-model";
  auto const with_key = build_startup_overview_snapshot(input);
  expect(with_key.overview_toggle_keys == "F6" && with_key.compact_line.find("F6") != std::string::npos && with_key.model == "safe-model",
         "overview key hints and compact chrome refresh from effective bindings and refreshed snapshot fields");

  // Session titles/ids are never part of the overview surface (privacy).
  auto overview_view = ava::tui::overview_select_list_view(first);
  expect(std::ranges::none_of(overview_view.items, [](auto const& item) { return item.group == "Session"; }),
         "overview expanded view omits session title/id/origin rows entirely");
  expect(first.compact_line.find("Hello") == std::string::npos && first.detail_line.find("Hello") == std::string::npos &&
             first.detail_line.find("sess-") == std::string::npos,
         "overview collapsed chrome never includes free-form session titles or raw session ids");
  expect(std::ranges::any_of(overview_view.items,
                             [](auto const& item) {
                               return item.group == "Resources" && item.label.find("instruction") != std::string::npos &&
                                      item.detail.find('+') != std::string::npos;
                             }) &&
             std::ranges::any_of(overview_view.items,
                                 [&](auto const& item) {
                                   return item.group == "Instructions" && item.label == "Sources" && item.detail == std::to_string(context_sources.size()) &&
                                          item.detail.find('+') == std::string::npos;
                                 }),
         "expanded overview renders instruction group lower bounds as N+ while keeping the exact source total");
}

void test_startup_overview_bounded_lower_bound_counts()
{
  using ava::app::build_startup_overview_snapshot;
  using ava::app::kMaxStartupOverviewInputSources;
  using ava::app::StartupOverviewBuildInput;

  auto default_bindings = ava::tui::default_key_bindings();

  // Context >64 with mixed kinds: every instruction group is a lower bound; total stays exact.
  std::vector<ava::app::runtime::ContextSourceMetadata> context_sources;
  for (std::size_t i = 0; i < kMaxStartupOverviewInputSources + 5; ++i)
  {
    auto const type = (i % 2 == 0) ? ava::context::ContextSourceType::Global : ava::context::ContextSourceType::Workspace;
    context_sources.push_back(ava::app::runtime::ContextSourceMetadata{
        .path = std::filesystem::path("/private/context/") / std::to_string(i),
        .source_type = type,
        .byte_count = 8,
        .content_fingerprint = 100 + i,
    });
  }

  // Freshness >64: observed group counts and plugin-failure count are lower bounds.
  // First 64 include one plugin resource failure and successful plugin rows; past-cap
  // failures must not be scanned, so the observed failure count is a truthful N+.
  std::vector<ava::app::runtime::FreshnessSourceMetadata> freshness_sources;
  auto push_freshness = [&](ava::app::runtime::FreshnessSourceKind kind, std::string scope, std::string source_id, std::string name, std::size_t bytes,
                            std::uint64_t fingerprint) {
    freshness_sources.push_back(ava::app::runtime::FreshnessSourceMetadata{.kind = kind,
                                                                           .scope = std::move(scope),
                                                                           .source_id = std::move(source_id),
                                                                           .name = std::move(name),
                                                                           .path = {},
                                                                           .byte_count = bytes,
                                                                           .content_fingerprint = fingerprint});
  };
  for (std::size_t i = 0; i < kMaxStartupOverviewInputSources - 2; ++i)
    push_freshness(ava::app::runtime::FreshnessSourceKind::Skill, "global", "skill-" + std::to_string(i), "skill-" + std::to_string(i), 4, 10 + i);
  push_freshness(ava::app::runtime::FreshnessSourceKind::PluginPrompt, "project", "plug.a", "broken-a", 0, 0);
  push_freshness(ava::app::runtime::FreshnessSourceKind::PluginSkill, "project", "plug.a", "ok-a", 0, ava::core::content_fingerprint(""));
  // Beyond the first-64 work cap: more skills and an additional plugin failure that must not
  // be counted exactly, only force lower-bound marking of already-observed aggregates.
  for (std::size_t i = 0; i < 6; ++i)
    push_freshness(ava::app::runtime::FreshnessSourceKind::Skill, "global", "past-cap-skill-" + std::to_string(i), "past-cap-skill-" + std::to_string(i), 4,
                   900 + i);
  push_freshness(ava::app::runtime::FreshnessSourceKind::PluginPrompt, "project", "plug.b", "broken-past-cap", 0, 0);

  StartupOverviewBuildInput input{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-test",
      .trust_decision = "trusted",
      .project_resources = "enabled",
      .context_sources = context_sources,
      .freshness_sources = freshness_sources,
      .theme_name = "ava-dark",
      .theme_badge = "built-in",
      .key_bindings = &default_bindings,
  };

  auto const snapshot = build_startup_overview_snapshot(input);
  expect(snapshot.instruction_source_count == context_sources.size(), "bounded lower-bound case keeps exact instruction total from span size");

  bool saw_instruction_lower_bound = false;
  bool saw_freshness_lower_bound = false;
  for (auto const& group : snapshot.resource_groups)
  {
    if (group.kind == "instruction")
    {
      expect(group.count_is_lower_bound && group.count > 0, "context >64 marks instruction groups as positive lower bounds");
      saw_instruction_lower_bound = true;
    }
    else
    {
      expect(group.count_is_lower_bound && group.count > 0, "freshness >64 marks every observed freshness group as a lower bound");
      saw_freshness_lower_bound = true;
    }
  }
  expect(saw_instruction_lower_bound && saw_freshness_lower_bound, "both context and freshness lower-bound groups are present when inputs exceed the cap");

  expect(snapshot.plugin_resource_failure_count && *snapshot.plugin_resource_failure_count == 1 && snapshot.plugin_resource_failure_count_is_lower_bound,
         "plugin-resource failure count from the capped freshness prefix is a lower bound (does not scan past-cap failures)");
  expect(snapshot.detail_line.find("1+ plugin fails") != std::string::npos, "compact detail renders plugin-failure lower bounds as N+");

  auto const view = ava::tui::overview_select_list_view(snapshot);
  expect(std::ranges::any_of(view.items,
                             [](auto const& item) {
                               return item.group == "Resources" && item.label.find("instruction") != std::string::npos &&
                                      item.detail.starts_with("0") == false && item.detail.find('+') != std::string::npos;
                             }),
         "expanded overview renders instruction group lower bounds as N+");
  expect(std::ranges::any_of(view.items,
                             [](auto const& item) {
                               return item.group == "Resources" && item.label.find("skill") != std::string::npos && item.detail.find('+') != std::string::npos;
                             }),
         "expanded overview renders freshness group lower bounds as N+");
  expect(std::ranges::any_of(view.items, [](auto const& item) { return item.group == "Plugins" && item.label == "Resource failures" && item.detail == "1+"; }),
         "expanded overview renders plugin-failure lower bounds as N+");
  expect(std::ranges::any_of(view.items,
                             [&](auto const& item) {
                               return item.group == "Instructions" && item.label == "Sources" && item.detail == std::to_string(context_sources.size());
                             }),
         "expanded overview keeps the exact instruction source total without a plus marker");

  // Truthful 0+ when plugin resources were observed in the capped prefix with zero failures.
  std::vector<ava::app::runtime::FreshnessSourceMetadata> zero_fail_freshness;
  for (std::size_t i = 0; i < kMaxStartupOverviewInputSources; ++i)
  {
    zero_fail_freshness.push_back(ava::app::runtime::FreshnessSourceMetadata{
        .kind = ava::app::runtime::FreshnessSourceKind::PluginSkill,
        .scope = "project",
        .source_id = "plug.zero",
        .name = "ok-" + std::to_string(i),
        .path = {},
        .byte_count = 0,
        .content_fingerprint = ava::core::content_fingerprint("ok"),
    });
  }
  zero_fail_freshness.push_back(ava::app::runtime::FreshnessSourceMetadata{
      .kind = ava::app::runtime::FreshnessSourceKind::PluginPrompt,
      .scope = "project",
      .source_id = "plug.zero",
      .name = "past-cap-broken",
      .path = {},
      .byte_count = 0,
      .content_fingerprint = 0,
  });
  StartupOverviewBuildInput zero_fail_input{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-test",
      .context_sources = {},
      .freshness_sources = zero_fail_freshness,
      .key_bindings = &default_bindings,
  };
  auto const zero_fail = build_startup_overview_snapshot(zero_fail_input);
  expect(zero_fail.plugin_resource_failure_count && *zero_fail.plugin_resource_failure_count == 0 && zero_fail.plugin_resource_failure_count_is_lower_bound,
         "zero observed plugin failures under a capped freshness prefix remain a truthful 0+ lower bound");
  expect(zero_fail.detail_line.find("0+ plugin fails") != std::string::npos, "compact detail can show truthful 0+ plugin-failure lower bounds");
  auto const zero_view = ava::tui::overview_select_list_view(zero_fail);
  expect(std::ranges::any_of(zero_view.items,
                             [](auto const& item) { return item.group == "Plugins" && item.label == "Resource failures" && item.detail == "0+"; }),
         "expanded overview can show truthful 0+ plugin-failure lower bounds");
}

}  // namespace ava::tests::app_runtime_tests
