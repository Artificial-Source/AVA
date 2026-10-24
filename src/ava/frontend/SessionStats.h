#pragma once

#include "Data.h"
#include "DisplayString.h"
#include "ModelSelection.h"
#include "RefreshResult.h"
#include "ava/debug/print_members_on.h"
#include "ava/session/Stats.h"

#include <atomic>
#include "debug.h"

namespace ava::frontend {

class FrontEnd;

// Frontend projection of canonical session statistics, including both active-context estimates.
// Refresh and snapshot access are confined to the frontend thread; dirty() may be called by backend notification threads.
class SessionStats
{
 private:
  FrontEnd const* frontend_;
  ModelSelection* model_selection_;
  // Copy of the entire canonical ava::session::Stats value (all members), protected by ava::session::Stats_ts.
  ava::session::Stats stats_snapshot_;
  // Retain the exact saturated total rather than recovering it from compact display text.
  std::optional<long long> active_token_total_;
  DataListIndex stats_index_;
  std::atomic_bool dirty_;
  DisplayString token_count_;
  // Percentage has its own pending display state, independent of the compact token count.
  DisplayString percentage_;
  // Exact input changes request a recomputation even before they alter the rounded display.
  bool percentage_recompute_pending_{true};

 private:
  // Format the exact total compactly for existing token-count presentation.
  static std::string token_count_display_str(std::optional<long long> total);

  // Copy one canonical stats snapshot under one lock and retain pending display changes until consumed.
  void refresh();

  // Recompute the percentage display when an exact token total or context window change is pending.
  void recompute_percentage(std::optional<long long> context_window_tokens);

 public:
  // Wire this cache to frontend and the sibling model_selection projection, both of which must outlive it.
  // model_selection must be non-null and initialized before percentage refreshes.
  // This cache consumes its numeric context-window change notification.
  SessionStats(FrontEnd const* frontend, ModelSelection* model_selection) : frontend_(frontend), model_selection_(model_selection) { }

  // Connect this cache once to the registered canonical statistics at its stable address.
  void initialize(session::Stats_ts const* unlocked_stats);

  void dirty() { dirty_ = true; }

  // Return the compact active-history plus system-prompt estimate and consume only its pending display-change flag.
  RefreshResult<std::string const&> token_count_refresh()
  {
    // Call initialize first.
    ASSERT(!stats_index_.undefined());
    if (dirty_.exchange(false))
      refresh();
    bool const token_count_changed = token_count_.clear_changed();
    if (token_count_changed)
      token_count_.display_ = token_count_display_str(active_token_total_);
    return {token_count_.display_, token_count_changed};
  }

  // Return the context percentage on the frontend thread and independently consume its accumulated display change.
  // Unknown estimates or absent/nonpositive windows display "unknown"; known percentages are not capped at 100%.
  RefreshResult<std::string const&> contex_percentage_refresh()
  {
    // Call initialize before requesting context percentage.
    ASSERT(!stats_index_.undefined());
    if (dirty_.exchange(false))
      refresh();

    // Always observe model projection changes, regardless of token refresh order.
    auto const context_window = model_selection_->context_window_tokens_refresh();
    percentage_recompute_pending_ = percentage_recompute_pending_ || context_window.changed;
    recompute_percentage(context_window.value);
    bool const percentage_changed = percentage_.clear_changed();
    return {percentage_.display_, percentage_changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::frontend
