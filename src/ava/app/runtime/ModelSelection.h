#pragma once

#include "ReasoningSelection.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/frontend/UnlockedData.h"
#include "ava/config/model_config.h"

#include <optional>
#include <string>
#include <vector>

namespace ava::app::runtime {

// Model selection.
//
// The active model and its derived/user-selected companions: the resolved
// ModelInfo, the latest reasoning selection, and the optional scoped model
// rotation. `model` and `reasoning` are re-derived on resume (model from the
// persisted entries, reasoning from the last assistant turn); all three are
// runtime-mutable: model/reasoning change on /models, scoped_model_cycle on
// /models scope commands.
struct ModelSelection
{
  ava::config::ModelInfo model;
  std::optional<ReasoningSelection> reasoning = std::nullopt;
  std::optional<std::vector<std::string>> scoped_model_cycle = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using ModelSelection_ts = UnlockedData<ModelSelection>;

} // namespace ava::app::runtime
