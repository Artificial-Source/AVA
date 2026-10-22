#include "sys.h"
#include "ava/tui/theme.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ava::tui {
namespace {

// Theme configuration, previews, and terminal-background observations belong to the TUI thread.
// Backend command workers may persist settings, but application of those settings happens here only on that thread.
std::optional<std::string>& config_theme_storage()
{
  static std::optional<std::string> theme;
  return theme;
}

std::optional<TuiCustomTheme>& config_custom_theme_storage()
{
  static std::optional<TuiCustomTheme> theme;
  return theme;
}

std::optional<TuiThemeInfo>& theme_preview_storage()
{
  static std::optional<TuiThemeInfo> preview;
  return preview;
}

std::uint64_t& theme_preview_generation_storage()
{
  static std::uint64_t generation = 0;
  return generation;
}

std::optional<TerminalBackgroundAppearance>& detected_terminal_background_storage()
{
  static std::optional<TerminalBackgroundAppearance> appearance;
  return appearance;
}

bool env_enabled(char const* name)
{
  auto const* value = std::getenv(name);
  return value != nullptr && value[0] != '\0';
}

std::string lower_ascii(std::string_view text)
{
  std::string lowered;
  lowered.reserve(text.size());
  for (char const ch : text)
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return lowered;
}

std::string theme_env_value()
{
  auto const* value = std::getenv("AVA_TUI_THEME");
  return value == nullptr ? std::string{} : lower_ascii(value);
}

std::string_view trim_ascii(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    text.remove_suffix(1);
  return text;
}

std::optional<int> parse_nonnegative_int(std::string_view text)
{
  text = trim_ascii(text);
  if (text.empty())
    return std::nullopt;

  int value = 0;
  auto const* begin = text.data();
  auto const* end = begin + text.size();
  auto [ptr, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || ptr != end || value < 0)
    return std::nullopt;
  return value;
}

std::optional<int> colorfgbg_background_index()
{
  auto const* value = std::getenv("COLORFGBG");
  if (value == nullptr || value[0] == '\0')
    return std::nullopt;

  std::string_view const text(value);
  auto const separator = text.find_last_of(";:");
  auto const background = separator == std::string_view::npos ? text : text.substr(separator + 1);
  return parse_nonnegative_int(background);
}

std::optional<int> xterm_color_luminance(int index)
{
  static constexpr int ansi16_luminance[] = {0, 76, 150, 114, 29, 105, 178, 229, 127, 127, 200, 226, 105, 170, 221, 255};
  if (index >= 0 && index < 16)
    return ansi16_luminance[index];

  if (index >= 16 && index <= 231)
  {
    auto const offset = index - 16;
    auto const red_index = offset / 36;
    auto const green_index = (offset / 6) % 6;
    auto const blue_index = offset % 6;
    auto const level = [](int value) { return value == 0 ? 0 : 55 + (value * 40); };
    auto const red = level(red_index);
    auto const green = level(green_index);
    auto const blue = level(blue_index);
    return ((red * 299) + (green * 587) + (blue * 114)) / 1000;
  }

  if (index >= 232 && index <= 255)
    return 8 + ((index - 232) * 10);

  return std::nullopt;
}

std::optional<std::string> configured_theme_value()
{
  return config_theme_storage();
}

std::optional<TuiCustomTheme> configured_custom_theme_value()
{
  return config_custom_theme_storage();
}

std::optional<TuiThemeInfo> theme_info_for_request(std::string_view requested, std::string badge)
{
  auto const value = lower_ascii(requested);
  if (value == "plain" || value == "none" || value == "no-color" || value == "no_color")
  {
    return TuiThemeInfo{.kind = TuiThemeKind::Plain,
                        .name = "plain",
                        .detail = badge + "=plain disables ANSI styling",
                        .badge = std::move(badge),
                        .palette = std::nullopt,
                        .revision = "plain"};
  }
  if (value == "light" || value == "ava-light")
  {
    return TuiThemeInfo{.kind = TuiThemeKind::Light,
                        .name = "ava-light",
                        .detail = "built-in light ncurses token palette",
                        .badge = std::move(badge),
                        .palette = std::nullopt,
                        .revision = "built-in-light"};
  }
  if (value == "dark" || value == "ava-dark")
  {
    return TuiThemeInfo{.kind = TuiThemeKind::Dark,
                        .name = "ava-dark",
                        .detail = "built-in dark ncurses token palette",
                        .badge = std::move(badge),
                        .palette = std::nullopt,
                        .revision = "built-in-dark"};
  }
  return std::nullopt;
}

TuiThemeInfo theme_info_for_custom(TuiCustomTheme const& theme)
{
  return TuiThemeInfo{.kind = TuiThemeKind::Custom,
                      .name = theme.name,
                      .detail = "custom theme file: " + theme.path.string(),
                      .badge = "display.json",
                      .palette = theme.palette,
                      .revision = theme.revision};
}

// Interpret the caller's optional terminal-background observation without reading frontend state.
std::optional<TuiThemeInfo> theme_info_for_detected_terminal_background(std::optional<TerminalBackgroundAppearance> appearance)
{
  if (!appearance)
    return std::nullopt;

  if (*appearance == TerminalBackgroundAppearance::Light)
  {
    return TuiThemeInfo{.kind = TuiThemeKind::Light,
                        .name = "ava-light",
                        .detail = "terminal background appears light from OSC 11",
                        .badge = "OSC 11",
                        .palette = std::nullopt,
                        .revision = "osc11-light"};
  }

  return TuiThemeInfo{.kind = TuiThemeKind::Dark,
                      .name = "ava-dark",
                      .detail = "terminal background appears dark from OSC 11",
                      .badge = "OSC 11",
                      .palette = std::nullopt,
                      .revision = "osc11-dark"};
}

std::optional<TuiThemeInfo> theme_info_for_terminal_background()
{
  auto const background = colorfgbg_background_index();
  if (!background)
    return std::nullopt;

  auto const luminance = xterm_color_luminance(*background);
  if (!luminance)
    return std::nullopt;

  if (*luminance >= 180)
  {
    return TuiThemeInfo{.kind = TuiThemeKind::Light,
                        .name = "ava-light",
                        .detail = "terminal background appears light from COLORFGBG",
                        .badge = "COLORFGBG",
                        .palette = std::nullopt,
                        .revision = "colorfgbg-light"};
  }

  return TuiThemeInfo{.kind = TuiThemeKind::Dark,
                      .name = "ava-dark",
                      .detail = "terminal background appears dark from COLORFGBG",
                      .badge = "COLORFGBG",
                      .palette = std::nullopt,
                      .revision = "colorfgbg-dark"};
}

bool theme_selection_is_automatic()
{
  if (env_enabled("NO_COLOR"))
    return false;

  auto const requested = theme_env_value();
  if (theme_info_for_request(requested, "AVA_TUI_THEME"))
    return false;

  auto const configured = configured_theme_value();
  if (configured)
  {
    if (theme_info_for_request(*configured, "display.json"))
      return false;
    auto const custom_theme = configured_custom_theme_value();
    if (custom_theme && custom_theme->name == *configured)
      return false;
  }

  return true;
}

}  // namespace

void set_tui_config_theme(std::optional<std::string> theme, std::optional<TuiCustomTheme> custom_theme)
{
  if (theme && !custom_theme)
    *theme = lower_ascii(*theme);
  if (custom_theme)
    theme = custom_theme->name;
  config_theme_storage() = std::move(theme);
  config_custom_theme_storage() = std::move(custom_theme);
}

void set_tui_theme_preview(std::optional<TuiThemeInfo> preview)
{
  if (preview)
  {
    // Ensure each preview identity invalidates composer style caches even when
    // the palette values match a previous candidate.
    ++theme_preview_generation_storage();
    preview->revision = preview->revision.empty() ? ("preview-" + std::to_string(theme_preview_generation_storage()))
                                                  : (preview->revision + "|preview-" + std::to_string(theme_preview_generation_storage()));
  }
  else
  {
    ++theme_preview_generation_storage();
  }
  theme_preview_storage() = std::move(preview);
}

void clear_tui_theme_preview()
{
  set_tui_theme_preview(std::nullopt);
}

bool tui_theme_preview_active()
{
  return theme_preview_storage().has_value();
}

void set_detected_terminal_background_appearance(std::optional<TerminalBackgroundAppearance> appearance)
{
  detected_terminal_background_storage() = appearance;
}

void reset_detected_terminal_background_appearance()
{
  set_detected_terminal_background_appearance(std::nullopt);
}

std::optional<TerminalBackgroundAppearance> detected_terminal_background_appearance()
{
  return detected_terminal_background_storage();
}

bool tui_theme_needs_terminal_background_probe()
{
  return theme_selection_is_automatic();
}

// Resolve supplied configuration and optional terminal detection against environment precedence.
// This never reads or changes the frontend's stored configuration or preview overlay.
TuiThemeInfo resolve_tui_theme(std::optional<std::string> const& configured, std::optional<TuiCustomTheme> const& custom_theme,
                               std::optional<TerminalBackgroundAppearance> detected_background)
{
  if (env_enabled("NO_COLOR"))
  {
    return TuiThemeInfo{.kind = TuiThemeKind::Plain, .name = "plain", .detail = "NO_COLOR disables ANSI styling", .badge = "NO_COLOR"};
  }

  auto const requested = theme_env_value();
  if (auto env_theme = theme_info_for_request(requested, "AVA_TUI_THEME"))
  {
    return *env_theme;
  }

  if (configured)
  {
    if (auto config_theme = theme_info_for_request(*configured, "display.json"))
      return *config_theme;
    if (custom_theme && custom_theme->name == *configured)
      return theme_info_for_custom(*custom_theme);
  }

  if (auto detected_theme = theme_info_for_detected_terminal_background(detected_background))
    return *detected_theme;

  if (auto terminal_theme = theme_info_for_terminal_background())
    return *terminal_theme;

  return TuiThemeInfo{.kind = TuiThemeKind::Dark,
                      .name = "ava-dark",
                      .detail = requested.empty() ? std::string("built-in dark ncurses token palette") : std::string("unknown AVA_TUI_THEME ignored"),
                      .badge = requested.empty() ? std::string("built-in") : std::string("built-in fallback"),
                      .palette = std::nullopt,
                      .revision = requested.empty() ? std::string("built-in-dark") : std::string("built-in-dark-fallback")};
}

// Read the UI-owned preview and effective theme on the TUI thread; NO_COLOR overrides previews too.
TuiThemeInfo active_tui_theme()
{
  if (!env_enabled("NO_COLOR") && theme_preview_storage())
    return *theme_preview_storage();
  return resolve_tui_theme(config_theme_storage(), config_custom_theme_storage(), detected_terminal_background_storage());
}

bool tui_plain_output()
{
  return active_tui_theme().kind == TuiThemeKind::Plain;
}

}  // namespace ava::tui
