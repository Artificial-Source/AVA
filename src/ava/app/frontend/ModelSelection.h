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
  config::ModelInfo model_snapshot_;
  DataListIndex index_;
  std::atomic_bool dirty_;

  bool model_display_changed_{true};
  bool provider_changed_{true};

 private:
  static std::string model_display_str(ava::config::ModelInfo const& model_info)
  {
    return model_info.display_name.empty() ? ava::config::model_display_label(model_info.model_id) : model_info.display_name;
  }

  void refresh(FrontEnd const* frontend)
  {
    Data const* data = frontend->get_data(index_);
    // index_ must be set to the index returned by FrontEnd::register_data while registering a ModelSelection_ts.
    // The life-time of that ModelSelection_ts must be larger than the use of this function, and never be moved in memory.
    ASSERT(dynamic_cast<runtime::ModelSelection_ts const*>(data) != nullptr);
    runtime::ModelSelection_ts const* unlocked_model_selection = static_cast<runtime::ModelSelection_ts const*>(data);
    runtime::ModelSelection_ts::crat model_selection_r{*unlocked_model_selection};

    model_display_changed_ = model_display_str(model_snapshot_) != model_display_str(model_selection_r->model);
    provider_changed_ = model_snapshot_.provider_id != model_selection_r->model.provider_id;

    // Refresh the whole ModelInfo.
    model_snapshot_ = model_selection_r->model;
  }

 public:
  ModelSelection() = default;

  void initialize(runtime::ModelSelection_ts const* unlocked_model_selection)
  {
    // Only call initialize once.
    ASSERT(index_.undefined());
    index_ = unlocked_model_selection->index();
    dirty_ = true;
  }

  void dirty() { dirty_ = true; }

  // Check if the model_display string changed. If it changed then the new value is written model_display and true is returned.
  // Otherwise false is returned, and model_display should not be used (it might or might not be updated).
  bool refresh_model_display(FrontEnd* frontend, std::string& model_display)
  {
    // Call initialize first.
    ASSERT(!index_.undefined());

    // Atomically test and set dirty_ to false, otherwise it is possible that a new update is missed
    // if dirty_ is set again inbetween the test and the clear.
    if (dirty_.exchange(false))
      refresh(frontend);

    // FIXME: eventually, writing to model_display is only needed if we return true.
    if (true /*model_display_changed_*/)
      model_display = model_display_str(model_snapshot_);

    bool model_display_changed = model_display_changed_;
    model_display_changed_ = false;
    return model_display_changed;
  }

  bool refresh_provider(FrontEnd* frontend, std::string& provider)
  {
    // Call initialize first.
    ASSERT(!index_.undefined());

    // Atomically test and set dirty_ to false, otherwise it is possible that a new update is missed
    // if dirty_ is set again inbetween the test and the clear.
    if (dirty_.exchange(false))
      refresh(frontend);

    // FIXME: eventually, writing to model_display is only needed if we return true.
    if (true /*provider_changed_*/)
      provider = model_snapshot_.provider_id;

    bool provider_changed = provider_changed_;
    provider_changed_ = false;
    return provider_changed;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::app::frontend
