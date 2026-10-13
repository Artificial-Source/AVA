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
  FrontEnd* frontend_;
  config::ModelInfo model_snapshot_;
  DataListIndex index_;
  std::atomic_bool dirty_;

  // Cached strings for immediate display.
  State model_;
  State provider_;

 private:
  static std::string model_display_str(ava::config::ModelInfo const& model_info)
  {
    return model_info.display_name.empty() ? ava::config::model_display_label(model_info.model_id) : model_info.display_name;
  }

  void refresh()
  {
    Data const* data = frontend_->get_data(index_);
    // index_ must be set to the index returned by FrontEnd::register_data while registering a ModelSelection_ts.
    // The life-time of that ModelSelection_ts must be larger than the use of this function, and never be moved in memory.
    ASSERT(dynamic_cast<runtime::ModelSelection_ts const*>(data) != nullptr);
    runtime::ModelSelection_ts const* unlocked_model_selection = static_cast<runtime::ModelSelection_ts const*>(data);
    runtime::ModelSelection_ts::crat model_selection_r{*unlocked_model_selection};

    model_.changed_ = model_display_str(model_snapshot_) != model_display_str(model_selection_r->model);
    provider_.changed_ = model_snapshot_.provider_id != model_selection_r->model.provider_id;

    // Refresh the whole ModelInfo.
    model_snapshot_ = model_selection_r->model;
  }

 public:
  ModelSelection(FrontEnd* frontend) : frontend_(frontend) { }

  void initialize(runtime::ModelSelection_ts const* unlocked_model_selection)
  {
    // Only call initialize once.
    ASSERT(index_.undefined());
    index_ = unlocked_model_selection->index();
    dirty_ = true;
  }

  void dirty() { dirty_ = true; }

  // Refresh our snapshot of `model_` if necessary. `changed` is set to true iff re-display is necessary.
  RefreshResult refresh_model_display()
  {
    // Call initialize first.
    ASSERT(!index_.undefined());

    // Atomically test and set dirty_ to false, otherwise it is possible that a new update is missed
    // if dirty_ is set again inbetween the test and the clear.
    if (dirty_.exchange(false))
      refresh();
    bool model_changed = model_changed = model_.clear_changed();
    if (model_changed)
      model_.display_ = model_display_str(model_snapshot_);
    return {model_.display_, model_changed};
  }

  // Refresh our snapshot of `provider_` if necessary. `changed` is set to true iff re-display is necessary.
  RefreshResult refresh_provider()
  {
    if (dirty_.exchange(false))
      refresh();
    bool provider_changed = provider_.clear_changed();
    if (provider_changed)
      provider_.display_ = model_snapshot_.provider_id;
    return {provider_.display_, provider_changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::app::frontend
