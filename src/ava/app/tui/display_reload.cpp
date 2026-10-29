#include "sys.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/tui/configuration_commands.h"
#include "ava/app/tui/display_reload.h"
#include "ava/tui/theme.h"

#include <utility>

namespace ava::app {

ava::core::Result<std::vector<std::pair<std::string, std::string>>> reload_tui_display_settings(ava::config::XdgPaths const& paths)
{
  auto settings = load_tui_display_settings(paths);
  if (!settings)
    return std::unexpected(std::move(settings.error()));
  std::vector<std::pair<std::string, std::string>> details;
  details.emplace_back("config", settings->path.string());
  details.emplace_back("configured", settings->theme ? *settings->theme : std::string("built-in default"));
  auto const effective = ava::tui::resolve_tui_theme(settings->theme, settings->custom_theme);
  details.emplace_back("effective", effective.name + " (" + effective.badge + ")");
  return details;
}

LocalTuiDisplaySettings::LocalTuiDisplaySettings(runtime::session_ts const& unlocked_session, std::shared_ptr<MermaidTuiBridge> mermaid)
    : unlocked_session_(unlocked_session), effective_{}, mermaid_(std::move(mermaid))
{
}

//static
LocalTuiDisplaySettings LocalTuiDisplaySettings::create(runtime::session_ts const& unlocked_session)
{
  return {unlocked_session, MermaidTuiBridge::create({})};
}

ava::tui::TuiLocalDisplaySettings LocalTuiDisplaySettings::presentation() const
{
  auto const paths = runtime::session_ts::crat(unlocked_session_)->paths();
  return presentation(paths);
}

ava::tui::TuiLocalDisplaySettings LocalTuiDisplaySettings::presentation(ava::config::XdgPaths const& paths) const
{
  std::vector<ava::tui::ThemeOptionItem> themes;
  for (auto const& theme : available_tui_custom_themes(paths))
  {
    themes.push_back(ava::tui::ThemeOptionItem{.name = theme.name, .detail = theme.path.string(), .palette = theme.palette, .revision = theme.revision});
  }
  auto const mermaid = mermaid_->presentation_configuration();
  return ava::tui::TuiLocalDisplaySettings{.custom_themes = std::move(themes),
                                           .show_images = effective_.show_images,
                                           .image_width_cells = effective_.image_width_cells,
                                           .cursor = effective_.cursor,
                                           .mermaid_config_epoch = mermaid.epoch,
                                           .mermaid_enabled = mermaid.enabled};
}

ava::tui::TuiMermaidRenderBridge LocalTuiDisplaySettings::mermaid_bridge() const
{
  return mermaid_->tui_bridge();
}

void LocalTuiDisplaySettings::shutdown() noexcept
{
  mermaid_->shutdown();
}

ava::core::Result<ava::tui::TuiLocalDisplaySettingsResult> LocalTuiDisplaySettings::apply_loaded(ava::config::XdgPaths const& paths, TuiDisplaySettings loaded,
                                                                                                 TuiDisplaySettingsWatchState watched, std::string status,
                                                                                                 std::vector<std::string> output)
{
  // `watched` was captured before `loaded`: do not re-read it here, or an edit
  // arriving after this load could be acknowledged without ever being applied.
  if (auto applied = mermaid_->apply(loaded.mermaid); !applied)
    return std::unexpected(std::move(applied.error()));
  ava::tui::set_tui_config_theme(loaded.theme, loaded.custom_theme);
  if (status == "display theme reloaded" || status == "display theme auto-reloaded")
  {
    auto const active = ava::tui::active_tui_theme();
    status += ": " + active.name + " (" + active.badge + ")";
  }
  effective_ = std::move(loaded);
  watch_state_ = std::move(watched);
  return ava::tui::TuiLocalDisplaySettingsResult{.settings = presentation(paths), .status = std::move(status), .output = std::move(output)};
}

ava::core::Result<ava::tui::TuiLocalDisplaySettingsResult> LocalTuiDisplaySettings::reload(std::string status)
{
  auto const paths = runtime::session_ts::crat(unlocked_session_)->paths();
  auto watched = load_tui_display_settings_watch_state(paths);
  if (!watched)
    return std::unexpected(std::move(watched.error()));
  auto loaded = load_tui_display_settings(paths);
  if (!loaded)
    return std::unexpected(std::move(loaded.error()));
  return apply_loaded(paths, std::move(*loaded), std::move(*watched), std::move(status));
}

ava::core::Result<std::optional<ava::tui::TuiLocalDisplaySettingsResult>> LocalTuiDisplaySettings::maybe_reload()
{
  auto const paths = runtime::session_ts::crat(unlocked_session_)->paths();
  auto watched = load_tui_display_settings_watch_state(paths);
  if (!watched)
    return std::unexpected(std::move(watched.error()));
  if (watch_state_ && !tui_display_settings_watch_state_changed(*watch_state_, *watched))
    return std::optional<ava::tui::TuiLocalDisplaySettingsResult>{};

  // Invalid edits leave both the effective presentation and watch baseline at
  // their last-known-good values so a corrected same-path edit is retried.
  auto loaded = load_tui_display_settings(paths);
  if (!loaded)
    return std::unexpected(std::move(loaded.error()));
  auto applied = apply_loaded(paths, std::move(*loaded), std::move(*watched), "display theme auto-reloaded");
  if (!applied)
    return std::unexpected(std::move(applied.error()));
  return std::optional<ava::tui::TuiLocalDisplaySettingsResult>{std::move(*applied)};
}

ava::core::Result<ava::tui::TuiLocalDisplaySettingsResult> LocalTuiDisplaySettings::apply_action(std::string_view action)
{
  std::string command(action);
  if (action == "settings:images.on")
    command = "/images on";
  else if (action == "settings:images.off")
    command = "/images off";
  else if (action == "settings:images.reset")
    command = "/images reset";
  else if (action == "settings:image-width.reset")
    command = "/image-width reset";
  else if (action.starts_with("settings:image-width."))
    command = "/image-width " + std::string(action.substr(std::string_view("settings:image-width.").size()));
  else if (action.starts_with("settings:cursor.style."))
    command = "/cursor " + std::string(action.substr(std::string_view("settings:cursor.style.").size()));
  else if (action == "settings:cursor.blink" || action == "settings:cursor.steady")
  {
    auto const blink = action == "settings:cursor.blink" ? "blink" : "steady";
    command = "/cursor " + std::string(tui_cursor_style_name(effective_.cursor.style())) + " " + blink;
  }
  else if (action.starts_with("theme:"))
    command = "/theme " + std::string(action.substr(std::string_view("theme:").size()));

  auto const paths = runtime::session_ts::crat(unlocked_session_)->paths();
  auto persisted = run_tui_display_configuration_command(paths, command);
  if (!persisted)
    return std::unexpected(std::move(persisted.error()));
  auto output = std::move(persisted->output);
  auto status = output.empty() ? std::string("display settings updated") : output.front();
  if (auto const newline = status.find('\n'); newline != std::string::npos)
    status.erase(newline);
  auto watched = load_tui_display_settings_watch_state(paths);
  if (!watched)
    return std::unexpected(std::move(watched.error()));
  auto loaded = load_tui_display_settings(paths);
  if (!loaded)
    return std::unexpected(std::move(loaded.error()));
  return apply_loaded(paths, std::move(*loaded), std::move(*watched), std::move(status), std::move(output));
}

}  // namespace ava::app
