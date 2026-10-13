#pragma once

#include "ava/config/xdg_paths.h"
#include "ava/core/result.h"

#include <string>
#include <utility>
#include <vector>

namespace ava::app {

[[nodiscard]] ava::core::Result<std::vector<std::pair<std::string, std::string>>> reload_tui_display_settings(ava::config::XdgPaths const& paths);

}  // namespace ava::app
