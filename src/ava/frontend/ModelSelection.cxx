#include "sys.h"
#ifdef CWDEBUG
#include "ava/debug/debug_ostream_operators.h"
#endif
#include "ModelSelection.h"
#include "ava/config/model_profiles.h"

namespace ava::frontend {

// Return the explicit model display name or derive the configured label from its identifier.
//static
std::string ModelSelection::model_display_str(ava::config::ModelInfo const& model_info)
{
  return model_info.display_name.empty() ? ava::config::model_display_label(model_info.model_id) : model_info.display_name;
}

// Project the optional canonical reasoning selection to its frontend display value.
//static
std::string ModelSelection::reasoning_display_str(std::optional<app::runtime::ReasoningSelection> const& reasoning)
{
  if (reasoning)
  {
    // Don't set the optional to a app::runtime::ReasoningSelection with an empty `level`.
    ASSERT(!reasoning->level.empty());
    return reasoning->level;
  }

  static std::string const unknown_reasoning("unknown");
  return unknown_reasoning;
}

// Copy canonical model selection state and accumulate every independently consumed display change.
void ModelSelection::refresh()
{
  Data const* data = frontend_->get_data(index_);
  // index_ must be set to the index returned by FrontEnd::register_data while registering a ModelSelection_ts.
  // The life-time of that ModelSelection_ts must be larger than the use of this function, and never be moved in memory.
  ASSERT(dynamic_cast<app::runtime::ModelSelection_ts const*>(data) != nullptr);
  app::runtime::ModelSelection_ts const* unlocked_model_selection = static_cast<app::runtime::ModelSelection_ts const*>(data);
  app::runtime::ModelSelection_ts::crat model_selection_r{*unlocked_model_selection};

  auto const old_model = model_display_str(model_snapshot_);
  auto const new_model = model_display_str(model_selection_r->model);
  bool const model_changed = old_model != new_model;
  Dout(dc::frontend(model_changed), "model changed from " << old_model << " to " << new_model);
  model_.changed_ = model_.changed_ || model_changed;

  auto const& old_provider = model_snapshot_.provider_id;
  auto const& new_provider = model_selection_r->model.provider_id;
  bool const provider_changed = old_provider != new_provider;
  Dout(dc::frontend(provider_changed), "provider hanged from " << old_provider << " to " << new_provider);
  provider_.changed_ = provider_.changed_ || old_provider != new_provider;

  auto const& old_reasoning = reasoning_display_str(reasoning_snapshot_);
  // Don't set the optional `reasoning` if the model doesn't support more than one reasoning level.
  ASSERT(!model_selection_r->reasoning ||
         (model_selection_r->model.supports_reasoning.value_or(false) && ava::config::supported_reasoning_levels(model_selection_r->model).size() > 1));
  auto const& new_reasoning = reasoning_display_str(model_selection_r->reasoning);
  bool const reasoning_changed = old_reasoning != new_reasoning;
  Dout(dc::frontend(reasoning_changed), "reasoning changed from " << old_reasoning << " to " << new_reasoning);
  reasoning_.changed_ = reasoning_.changed_ || reasoning_changed;

  // Compare through the existing full-model snapshot so the numeric projection does not duplicate canonical model state.

  auto const old_context_window_tokens = model_snapshot_.context_window_tokens;
  auto const new_context_window_tokens = model_selection_r->model.context_window_tokens;
  bool const context_window_tokens_changed = old_context_window_tokens != new_context_window_tokens;
  Dout(dc::frontend(context_window_tokens_changed), "context_window_tokens changed from " << old_context_window_tokens << " to " << new_context_window_tokens);
  context_window_tokens_changed_ = context_window_tokens_changed_ || context_window_tokens_changed;

  // Refresh all snapshots.
  model_snapshot_ = model_selection_r->model;
  reasoning_snapshot_ = model_selection_r->reasoning;
}

// Connect this projection to the canonical model selection exactly once.
void ModelSelection::initialize(app::runtime::ModelSelection_ts const* unlocked_model_selection)
{
  // Only call initialize once.
  ASSERT(index_.undefined());
  index_ = unlocked_model_selection->index();
  dirty_ = true;
}

} // namespace ava::frontend
