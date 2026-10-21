#pragma once

#include "FrontEnd.h"
#include "ava/app/runtime/ModelSelection.h"     // app::runtime::ModelSelection_ts
#include "utils/macros.h"

#include <atomic>
#include "debug.h"

namespace ava::frontend {

class ModelSelection
{
 private:
  FrontEnd const* frontend_;
  config::ModelInfo model_snapshot_;                                                    // Copy of the canonical ava::app::runtime::ModelSelection::model.
  std::optional<app::runtime::ReasoningSelection> reasoning_snapshot_ = std::nullopt;   // Copy of the canonical ava::app::runtime::ModelSelection::reasoning.
  DataListIndex index_;
  std::atomic_bool dirty_;

  // Cached strings for immediate display.
  DisplayString model_;
  DisplayString provider_;
  DisplayString reasoning_;
  // Accumulate numeric changes until the numeric consumer observes them.
  bool context_window_tokens_changed_{true};

 private:
  // Return the frontend label for model_info, preferring its explicit display name.
  static std::string model_display_str(ava::config::ModelInfo const& model_info);

  // Project model capabilities and the optional reasoning selection to the level shown by frontends.
  // reasoning is the canonical optional selection from the model-selection snapshot.
  // Return no level when reasoning is unavailable or no non-empty level is selected.
  // Callers reading shared state must keep both arguments protected by the same model-selection lock.
  static std::string reasoning_display_str(std::optional<app::runtime::ReasoningSelection> const& reasoning);

  // Refresh all cached projections from the registered canonical model selection under one read lock.
  void refresh();

 public:
  // Construct a projection backed by frontend, which must remain valid for this object's lifetime.
  ModelSelection(FrontEnd const* frontend) : frontend_(frontend) { }

  // Connect once to unlocked_model_selection at its stable registered address and request an initial refresh.
  void initialize(app::runtime::ModelSelection_ts const* unlocked_model_selection);

  // Mark the snapshot stale so the next public refresh observes canonical state.
  void dirty() { dirty_ = true; }

  // Refresh our snapshot of `model_` if necessary. `changed` is set to true iff re-display is necessary.
  RefreshResult<std::string const&> model_refresh()
  {
    // Atomically test and set dirty_ to false, otherwise it is possible that a new update is missed
    // if dirty_ is set again inbetween the test and the clear.
    if (dirty_.exchange(false))
      refresh();
    bool model_changed = model_.clear_changed();
    if (AI_UNLIKELY(model_changed))
      model_.display_ = model_display_str(model_snapshot_);
    return {model_.display_, model_changed};
  }

  // Refresh our snapshot of `provider_` if necessary. `changed` is set to true iff re-display is necessary.
  RefreshResult<std::string const&> provider_refresh()
  {
    if (dirty_.exchange(false))
      refresh();
    bool provider_changed = provider_.clear_changed();
    if (AI_UNLIKELY(provider_changed))
      provider_.display_ = model_snapshot_.provider_id;
    return {provider_.display_, provider_changed};
  }

  // Obtain the current reasoning display on the frontend thread after initialize has connected this object to registered data.
  //
  // Returns the cached display and whether it needs redisplay, consuming only reasoning's pending change flag.
  // An empty display means no visible reasoning selection.
  RefreshResult<std::string const&> reasoning_refresh()
  {
    if (dirty_.exchange(false))
      refresh();
    bool reasoning_changed = reasoning_.clear_changed();
    if (AI_UNLIKELY(reasoning_changed))
      reasoning_.display_ = reasoning_display_str(reasoning_snapshot_);
    return {reasoning_.display_, reasoning_changed};
  }

  // Return the context-window metadata by value and consume only its pending numeric change on the frontend thread.
  // An absent value means the window is unknown; changes to or from that state also set changed.
  // SessionStats owns consumption of this notification when the projections are wired together.
  RefreshResult<std::optional<long long>> context_window_tokens_refresh()
  {
    if (dirty_.exchange(false))
      refresh();
    bool const changed = context_window_tokens_changed_;
    context_window_tokens_changed_ = false;
    return {model_snapshot_.context_window_tokens, changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::frontend
