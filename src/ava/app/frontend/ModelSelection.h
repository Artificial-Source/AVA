#pragma once

#include "FrontEnd.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/runtime/Session.h"    // runtime::ModelSelection
#include "ava/config/model_profiles.h"  // ava::config::model_display_label

#include <atomic>

namespace ava::app::frontend {

class ModelSelection
{
 private:
  FrontEnd const* frontend_;
  config::ModelInfo model_snapshot_;                                                    // Copy of the canonical ava::app::runtime::ModelSelection::model.
  std::optional<runtime::ReasoningSelection> reasoning_snapshot_ = std::nullopt;        // Copy of the canonical ava::app::runtime::ModelSelection::reasoning.
  DataListIndex index_;
  std::atomic_bool dirty_;

  // Cached strings for immediate display.
  DisplayString model_;
  DisplayString provider_;
  DisplayString reasoning_;
  // Accumulate numeric changes until the numeric consumer observes them.
  bool context_window_tokens_changed_{true};

 private:
  static std::string model_display_str(ava::config::ModelInfo const& model_info)
  {
    return model_info.display_name.empty() ? ava::config::model_display_label(model_info.model_id) : model_info.display_name;
  }

  // Project model capabilities and the optional reasoning selection to the level shown by frontends.
  // Return no level when reasoning is unavailable or no non-empty level is selected.
  // Callers reading shared state must keep both arguments protected by the same model-selection lock.
  static std::string reasoning_display_str(std::optional<runtime::ReasoningSelection> const& reasoning)
  {
    if (reasoning)
    {
      // Don't set the optional to a runtime::ReasoningSelection with an empty `level`.
      ASSERT(!reasoning->level.empty());
      return reasoning->level;
    }

    static std::string const unknown_reasoning("unknown");
    return unknown_reasoning;
  }

  void refresh()
  {
    Data const* data = frontend_->get_data(index_);
    // index_ must be set to the index returned by FrontEnd::register_data while registering a ModelSelection_ts.
    // The life-time of that ModelSelection_ts must be larger than the use of this function, and never be moved in memory.
    ASSERT(dynamic_cast<runtime::ModelSelection_ts const*>(data) != nullptr);
    runtime::ModelSelection_ts const* unlocked_model_selection = static_cast<runtime::ModelSelection_ts const*>(data);
    runtime::ModelSelection_ts::crat model_selection_r{*unlocked_model_selection};

    auto const old_model = model_display_str(model_snapshot_);
    auto const new_model = model_display_str(model_selection_r->model);
    model_.changed_ = model_.changed_ || old_model != new_model;

    auto const& old_provider = model_snapshot_.provider_id;
    auto const& new_provider = model_selection_r->model.provider_id;
    provider_.changed_ = provider_.changed_ || old_provider != new_provider;

    auto const& old_reasoning = reasoning_display_str(reasoning_snapshot_);
    // Don't set the optional `reasoning` if the model doesn't support more than one reasoning level.
    ASSERT(!model_selection_r->reasoning ||
           (model_selection_r->model.supports_reasoning.value_or(false) && ava::config::supported_reasoning_levels(model_selection_r->model).size() > 1));
    auto const& new_reasoning = reasoning_display_str(model_selection_r->reasoning);
    reasoning_.changed_ = reasoning_.changed_ || old_reasoning != new_reasoning;

    // Compare through the existing full-model snapshot so the numeric projection does not duplicate canonical model state.
    context_window_tokens_changed_ = context_window_tokens_changed_ || model_snapshot_.context_window_tokens != model_selection_r->model.context_window_tokens;

    // Refresh all snapshots.
    model_snapshot_ = model_selection_r->model;
    reasoning_snapshot_ = model_selection_r->reasoning;
  }

 public:
  ModelSelection(FrontEnd const* frontend) : frontend_(frontend) { }

  void initialize(runtime::ModelSelection_ts const* unlocked_model_selection)
  {
    // Only call initialize once.
    ASSERT(index_.undefined());
    index_ = unlocked_model_selection->index();
    dirty_ = true;
  }

  // Mark the snapshot stale.
  void dirty() { dirty_ = true; }

  // Refresh our snapshot of `model_` if necessary. `changed` is set to true iff re-display is necessary.
  RefreshResult<std::string const&> refresh_model()
  {
    // Call initialize first.
    ASSERT(!index_.undefined());

    // Atomically test and set dirty_ to false, otherwise it is possible that a new update is missed
    // if dirty_ is set again inbetween the test and the clear.
    if (dirty_.exchange(false))
      refresh();
    bool model_changed = model_.clear_changed();
    if (model_changed)
      model_.display_ = model_display_str(model_snapshot_);
    return {model_.display_, model_changed};
  }

  // Refresh our snapshot of `provider_` if necessary. `changed` is set to true iff re-display is necessary.
  RefreshResult<std::string const&> refresh_provider()
  {
    if (dirty_.exchange(false))
      refresh();
    bool provider_changed = provider_.clear_changed();
    if (provider_changed)
      provider_.display_ = model_snapshot_.provider_id;
    return {provider_.display_, provider_changed};
  }

  // Obtain the current reasoning display on the frontend thread after initialize has connected this object to registered data.
  //
  // Returns the cached display and whether it needs redisplay, consuming only reasoning's pending change flag.
  // An empty display means no visible reasoning selection.
  RefreshResult<std::string const&> refresh_reasoning()
  {
    if (dirty_.exchange(false))
      refresh();
    bool reasoning_changed = reasoning_.clear_changed();
    if (reasoning_changed)
      reasoning_.display_ = reasoning_display_str(reasoning_snapshot_);
    return {reasoning_.display_, reasoning_changed};
  }

  // Return the context-window metadata by value and consume only its pending numeric change on the frontend thread.
  // An absent value means the window is unknown; changes to or from that state also set changed.
  // SessionStats owns consumption of this notification when the projections are wired together.
  RefreshResult<std::optional<long long>> context_window_tokens_refresh()
  {
    // Call initialize before requesting a projected context window.
    ASSERT(!index_.undefined());
    if (dirty_.exchange(false))
      refresh();
    bool const changed = context_window_tokens_changed_;
    context_window_tokens_changed_ = false;
    return {model_snapshot_.context_window_tokens, changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::app::frontend
