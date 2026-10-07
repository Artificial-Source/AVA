#pragma once

#include "ava/agent/agent_loop.h"
#include "ava/tui/composer.h"

#include <vector>

namespace ava::app {

// Project backend tool events into the path-bounded presentation DTO consumed by the TUI.
[[nodiscard]] std::vector<ava::tui::ToolTimelineItem> tool_timeline_for_tui(std::vector<ava::agent::ToolTimelineEntry> const& entries);

}  // namespace ava::app
