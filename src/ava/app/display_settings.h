#pragma once

#include "ava/tui/terminal/Cursor.h"
#include "ava/tui/theme.h"
#include "ava/config/xdg_paths.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

inline constexpr std::size_t kDefaultTuiImageWidthCells = 60;
inline constexpr std::size_t kMinTuiImageWidthCells = 8;
inline constexpr std::size_t kMaxTuiImageWidthCells = 160;
inline constexpr std::size_t kMaxTuiDisplaySettingsBytes = 64 * 1024;
inline constexpr std::size_t kMaxMermaidArgCount = 32;
inline constexpr std::size_t kMaxMermaidArgBytes = 4 * 1024;
inline constexpr std::size_t kMaxMermaidArgvBytes = 16 * 1024;
// Conservative application-owned bounds for custom theme discovery and the 500ms display watch.
// One oversized/invalid unconfigured file is skipped and must not fail built-in display reload.
inline constexpr std::size_t kMaxTuiCustomThemeFileBytes = 64 * 1024;
inline constexpr std::size_t kMaxTuiCustomThemeCandidates = 64;
inline constexpr std::size_t kMaxTuiCustomThemeCatalogAggregateBytes = 256 * 1024;

struct TuiCustomThemeSummary
{
  std::string name;
  std::filesystem::path path;
  // Parsed/validated palette + content revision. Invalid theme files are never listed.
  ava::tui::TuiThemePalette palette;
  std::string revision;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Validated Mermaid helper settings. Missing enabled is equivalent to false.
// Unknown nested members are retained verbatim by display.json field-specific updates.
struct MermaidDisplaySettings
{
  bool enabled = false;
  bool enabled_configured = false;
  std::vector<std::string> argv;
  bool argv_configured = false;
  std::vector<std::pair<std::string, std::string>> unknown_fields;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Validated display.json document with field-preserving unknown members.
struct DisplaySettingsDocument
{
  std::optional<std::string> theme;
  std::optional<bool> show_images;
  std::optional<std::size_t> image_width_cells;
  std::optional<ava::tui::terminal::CursorStyle> cursor_style;
  std::optional<bool> cursor_blink;
  std::optional<MermaidDisplaySettings> mermaid;
  // Unknown top-level fields retained as raw JSON values for forward-compatible updates.
  std::vector<std::pair<std::string, std::string>> unknown_fields;
  std::filesystem::path path;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TuiDisplaySettings
{
  std::optional<std::string> theme;
  std::optional<ava::tui::TuiCustomTheme> custom_theme;
  bool show_images = true;
  std::size_t image_width_cells = kDefaultTuiImageWidthCells;
  bool show_images_configured = false;
  bool image_width_configured = false;
  ava::tui::terminal::CursorSettings cursor{ava::tui::terminal::CursorStyle::Default};
  bool cursor_style_configured = false;
  bool cursor_blink_configured = false;
  MermaidDisplaySettings mermaid;
  std::filesystem::path path;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// One validated previewable custom theme observed by the application-owned watcher.
// Invalid/oversized/unreadable theme files are omitted; ordering is stable by name.
// Discovery is bounded (see kMaxTuiCustomTheme* constants): candidate files are scanned in
// normalized-path order, symlinks/special files are skipped, first valid file per name wins
// for catalog/listing, and configured/named load remains fail-closed on duplicates or when the
// bounded scan is incomplete (candidate cap or aggregate budget), even if one match appeared.
enum class TuiCustomThemeDiscoveryIncompleteReason : std::uint8_t
{
  None = 0,
  CandidateCap,
  AggregateBudget,
};

struct TuiCustomThemeCatalogEntry
{
  std::string name;
  std::string revision;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Observable bounded discovery result used by listing, watch catalog, and uniqueness-sensitive
// named/configured load. Listing may return the validated prefix when complete is false.
struct TuiCustomThemeDiscoveryResult
{
  std::vector<TuiCustomThemeSummary> themes;
  bool complete = true;
  TuiCustomThemeDiscoveryIncompleteReason incomplete_reason = TuiCustomThemeDiscoveryIncompleteReason::None;
  // Bytes physically read from candidate descriptors during this scan, including truncated/oversized
  // candidates that were skipped after work (overflow-safe, <= aggregate cap). Pre-fstat rejects of
  // known-oversized files contribute 0 because no content bytes were pulled.
  std::size_t aggregate_bytes_read = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiDisplaySettingsWatchState
{
  std::string display_revision;
  std::optional<std::string> theme;
  std::optional<std::filesystem::path> custom_theme_path;
  std::optional<std::string> custom_theme_revision;
  bool show_images = true;
  std::size_t image_width_cells = kDefaultTuiImageWidthCells;
  ava::tui::terminal::CursorSettings cursor{ava::tui::terminal::CursorStyle::Default};
  MermaidDisplaySettings mermaid;
  // Bounded deterministic catalog of every validated custom theme candidate (stable name order),
  // including themes that are only previewable and not currently configured.
  std::vector<TuiCustomThemeCatalogEntry> custom_theme_catalog;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] std::filesystem::path tui_display_settings_file(ava::config::XdgPaths const& paths);
[[nodiscard]] std::filesystem::path tui_theme_directory(ava::config::XdgPaths const& paths);
[[nodiscard]] std::optional<std::string> normalize_tui_theme_setting(std::string_view value);
[[nodiscard]] bool is_tui_theme_reset_value(std::string_view value);
[[nodiscard]] std::string tui_theme_setting_usage();
[[nodiscard]] std::optional<bool> normalize_tui_show_images_setting(std::string_view value);
[[nodiscard]] bool is_tui_show_images_reset_value(std::string_view value);
[[nodiscard]] std::string tui_show_images_setting_usage();
[[nodiscard]] std::optional<std::size_t> normalize_tui_image_width_setting(std::string_view value);
[[nodiscard]] bool is_tui_image_width_reset_value(std::string_view value);
[[nodiscard]] std::string tui_image_width_setting_usage();
[[nodiscard]] std::optional<ava::tui::terminal::CursorStyle> normalize_tui_cursor_style_setting(std::string_view value);
[[nodiscard]] std::optional<bool> normalize_tui_cursor_blink_setting(std::string_view value);
[[nodiscard]] std::string_view tui_cursor_style_name(ava::tui::terminal::CursorStyle style) noexcept;
[[nodiscard]] std::string tui_cursor_setting_usage();
[[nodiscard]] std::string active_tui_theme_summary();
[[nodiscard]] ava::core::Result<ava::tui::TuiCustomTheme> load_tui_custom_theme_file(std::filesystem::path const& path);
[[nodiscard]] ava::core::Result<ava::tui::TuiCustomTheme> load_tui_custom_theme(ava::config::XdgPaths const& paths, std::string_view name);
[[nodiscard]] TuiCustomThemeDiscoveryResult discover_tui_custom_themes(ava::config::XdgPaths const& paths);
[[nodiscard]] std::vector<TuiCustomThemeSummary> available_tui_custom_themes(ava::config::XdgPaths const& paths);
[[nodiscard]] ava::core::Result<DisplaySettingsDocument> load_display_settings_document(ava::config::XdgPaths const& paths);
[[nodiscard]] ava::core::Result<TuiDisplaySettings> load_tui_display_settings(ava::config::XdgPaths const& paths);
[[nodiscard]] ava::core::Result<TuiDisplaySettings> apply_tui_display_settings(ava::config::XdgPaths const& paths);
[[nodiscard]] ava::core::Result<TuiDisplaySettingsWatchState> load_tui_display_settings_watch_state(ava::config::XdgPaths const& paths);
[[nodiscard]] bool tui_display_settings_watch_state_changed(TuiDisplaySettingsWatchState const& previous, TuiDisplaySettingsWatchState const& current);
// Field-specific setters update or erase only the owned key and preserve every other field.
[[nodiscard]] ava::core::VoidResult store_tui_theme_setting(ava::config::XdgPaths const& paths, std::optional<std::string> theme);
[[nodiscard]] ava::core::VoidResult store_tui_show_images_setting(ava::config::XdgPaths const& paths, std::optional<bool> show_images);
[[nodiscard]] ava::core::VoidResult store_tui_image_width_setting(ava::config::XdgPaths const& paths, std::optional<std::size_t> image_width_cells);
[[nodiscard]] ava::core::VoidResult store_tui_cursor_setting(ava::config::XdgPaths const& paths, ava::tui::terminal::CursorStyle style,
                                                             std::optional<bool> blink);

}  // namespace ava::app
