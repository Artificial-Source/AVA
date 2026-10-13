#pragma once

#include "ava/app/runtime.h"
#include "ava/provider/provider.h"

#include <optional>
#include <vector>

namespace ava::app::runtime {

[[nodiscard]] std::optional<ReasoningSelection> latest_persisted_reasoning(std::vector<ava::session::SessionEntry> const& entries,
                                                                           ava::config::ModelInfo const& model);
[[nodiscard]] ava::provider::ProviderReasoningOptions provider_reasoning_options(ReasoningSelection const& selection);
[[nodiscard]] bool has_reasoning_choice(ava::config::ModelInfo const& model);

// Resolve a candidate reasoning selection against the model's supported levels.
//
// Trims the level, confirms the model supports it (or has an explicit mapping), validates the
// budget/display fields, and fills in `provider_level` when the model maps the level differently.
// Returns failure with provider/model/level context when the model rejects the level.
[[nodiscard]] ava::core::Result<ReasoningSelection> resolve_runtime_reasoning_selection(ava::config::ModelInfo const& model, ReasoningSelection selection);

}  // namespace ava::app::runtime
