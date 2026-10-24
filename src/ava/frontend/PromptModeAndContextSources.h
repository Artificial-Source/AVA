#pragma once

#include "Data.h"
#include "DisplayString.h"
#include "RefreshResult.h"
#include "ava/app/runtime/ResolvedPromptState.h"

#include <atomic>
#include <cstddef>
#include "debug.h"

namespace ava::frontend {

class FrontEnd;

// Frontend projection of the observable mode and context-source count.
class PromptModeAndContextSources
{
 private:
  FrontEnd const* frontend_;
  // Copy of canonical ava::app::runtime::PromptModeAndContextSources::mode, protected by PromptModeAndContextSources_ts.
  ava::agent::Mode mode_snapshot_ = ava::agent::Mode::Build;
  // Exact size of canonical ava::app::runtime::PromptModeAndContextSources::context_sources, read under PromptModeAndContextSources_ts.
  // Compares vector sizes, not vector contents; the vector itself is not copied.
  std::size_t context_source_count_snapshot_ = 0;
  DataListIndex index_;
  std::atomic_bool dirty_;
  DisplayString mode_;
  DisplayString context_source_count_;

  void refresh();

 public:
  // Construct a cache attached to frontend, which must outlive this object.
  explicit PromptModeAndContextSources(FrontEnd const* frontend) : frontend_(frontend) { }

  // Connect once to the registered backend metadata at unlocked_metadata's stable address.
  void initialize(app::runtime::PromptModeAndContextSources_ts const* unlocked_metadata);
  // Mark both display strings potentially stale from a backend notification thread.
  void dirty() { dirty_ = true; }

  // Return the selected execution mode's display on the frontend thread, consuming only its change flag.
  RefreshResult<std::string const&> mode_refresh();
  // Return the decimal number of context sources, consuming only that display's change flag.
  RefreshResult<std::string const&> context_source_count_refresh();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::frontend
