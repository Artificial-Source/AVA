#include "sys.h"
#include "ava/app/tui/display_reload.h"
#include "ava/app/tui/tui_display_settings.h"

namespace ava::app {

ava::core::Result<std::vector<std::pair<std::string, std::string>>> reload_tui_display_settings(ava::config::XdgPaths const& paths)
{
  auto settings = apply_tui_display_settings(paths);
  if (!settings)
    return std::unexpected(std::move(settings.error()));

  std::vector<std::pair<std::string, std::string>> details;
  details.emplace_back("config", settings->path.string());
  details.emplace_back("configured", settings->theme ? *settings->theme : std::string("built-in default"));
  details.emplace_back("active", active_tui_theme_summary());
  return details;
}

}  // namespace ava::app
