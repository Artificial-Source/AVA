#pragma once

#include "ava/app/runtime.h"
#include "ava/core/result.h"

#include <optional>
#include <string>
#include <string_view>

namespace ava::app {

[[nodiscard]] std::optional<std::string> reasoning_status_for_session(runtime::session_ts const& unlocked_session);

[[nodiscard]] ava::core::Result<runtime::ReasoningSelection> reasoning_selection_for_level(ava::config::ModelInfo const& model, std::string level);

[[nodiscard]] ava::core::Result<std::string> cycle_runtime_reasoning(runtime::session_ts& unlocked_session);

}  // namespace ava::app
