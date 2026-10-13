#pragma once

#include "ava/app/runtime.h"
#include "ava/tui/composer.h"

#include <optional>
#include <string>
#include <string_view>

namespace ava::app {

// Returns no view when policy resolution exposes no configurable non-off level.
// Canonical level values remain hidden selector authority; rows use concise labels.
[[nodiscard]] std::optional<tui::SelectListView> reasoning_selector_view(ava::config::ModelInfo const& model,
                                                                         std::optional<runtime::ReasoningSelection> const& current,
                                                                         std::string footer_hint = {});
[[nodiscard]] std::optional<tui::SelectListView> reasoning_selector_view(runtime::session_ts const& unlocked_session, std::string footer_hint = {});
[[nodiscard]] std::string reasoning_level_label(std::string_view level);

}  // namespace ava::app
