#include "sys.h"
#include "PromptMetadata.h"

namespace ava::frontend {

// Read one coherent mode/vector-size pair under the canonical lock without copying the source vector.
void PromptMetadata::refresh()
{
  Data const* data = frontend_->get_data(index_);
  // index_ must identify the PromptModeAndContextSources_ts registered during frontend initialization.
  ASSERT(dynamic_cast<app::runtime::PromptModeAndContextSources_ts const*>(data) != nullptr);
  auto const* unlocked_metadata = static_cast<app::runtime::PromptModeAndContextSources_ts const*>(data);
  app::runtime::PromptModeAndContextSources_ts::crat metadata_r{*unlocked_metadata};

  bool const mode_changed = mode_snapshot_ != metadata_r->mode;
  mode_.changed_ = mode_.changed_ || mode_changed;

  auto const new_context_source_count = metadata_r->context_sources.size();
  bool const context_source_count_changed = context_source_count_snapshot_ != new_context_source_count;
  context_source_count_.changed_ = context_source_count_.changed_ || context_source_count_changed;

  mode_snapshot_ = metadata_r->mode;
  context_source_count_snapshot_ = new_context_source_count;
}

// Store the registered index and ensure the first display refresh sees the canonical value.
void PromptMetadata::initialize(app::runtime::PromptModeAndContextSources_ts const* unlocked_metadata)
{
  // Only call initialize once.
  ASSERT(index_.undefined());
  index_ = unlocked_metadata->index();
  dirty_ = true;
}

RefreshResult<std::string const&> PromptMetadata::mode_refresh()
{
  // Call initialize before requesting projected prompt metadata.
  ASSERT(!index_.undefined());
  if (dirty_.exchange(false))
    refresh();
  bool const changed = mode_.clear_changed();
  if (changed)
    mode_.display_ = ava::agent::to_string(mode_snapshot_);
  return {mode_.display_, changed};
}

RefreshResult<std::string const&> PromptMetadata::context_source_count_refresh()
{
  // Call initialize before requesting projected prompt metadata.
  ASSERT(!index_.undefined());
  if (dirty_.exchange(false))
    refresh();
  bool const changed = context_source_count_.clear_changed();
  if (changed)
    context_source_count_.display_ = std::to_string(context_source_count_snapshot_);
  return {context_source_count_.display_, changed};
}

} // namespace ava::frontend
