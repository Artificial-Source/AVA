#pragma once

#include "ava/debug/print_members_on.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ava::tui {

enum class TuiThemeKind
{
  Dark,
  Light,
  Plain,
  Custom,
};

enum class TerminalBackgroundAppearance
{
  Dark,
  Light,
};

struct TuiThemePalette
{
  int text = -1;
  int muted = 6;
  int success = 2;
  int warning = 3;
  int error = 1;
  int accent = 6;
  int screen_bg = -1;
  int composer_bg = -1;
  int tool_bg = -1;
  int question_bg = -1;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiCustomTheme
{
  std::string name;
  std::filesystem::path path;
  TuiThemePalette palette;
  std::string revision;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiThemeInfo
{
  TuiThemeKind kind = TuiThemeKind::Dark;
  std::string name = "ava-dark";
  std::string detail = "built-in dark ncurses token palette";
  std::string badge = "built-in";
  std::optional<TuiThemePalette> palette = {};
  std::string revision = "built-in-dark";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Resolve configured theme, custom_theme palette, and optional detected_background using environment precedence.
// Reads no frontend storage and applies no changes, so headless configuration reporting can use it safely.
[[nodiscard]] TuiThemeInfo resolve_tui_theme(std::optional<std::string> const& configured, std::optional<TuiCustomTheme> const& custom_theme,
                                             std::optional<TerminalBackgroundAppearance> detected_background = std::nullopt);

// Theme access below is TUI-thread-only, including configuration, previews, and terminal-background detection.
// Backend workers persist configuration without reading or mutating this presentation state.
// Apply theme and its optional custom_theme palette locally; environment overrides still take precedence.
void set_tui_config_theme(std::optional<std::string> theme, std::optional<TuiCustomTheme> custom_theme = std::nullopt);
// Presentation-only theme overlay for settings preview. Does not write config.
// Cleared by cancel/confirm/modal replacement/runtime exit. NO_COLOR still wins.
void set_tui_theme_preview(std::optional<TuiThemeInfo> preview);
void clear_tui_theme_preview();
[[nodiscard]] bool tui_theme_preview_active();
void set_detected_terminal_background_appearance(std::optional<TerminalBackgroundAppearance> appearance);
void reset_detected_terminal_background_appearance();
[[nodiscard]] std::optional<TerminalBackgroundAppearance> detected_terminal_background_appearance();
[[nodiscard]] bool tui_theme_needs_terminal_background_probe();
[[nodiscard]] TuiThemeInfo active_tui_theme();
[[nodiscard]] bool tui_plain_output();

}  // namespace ava::tui
