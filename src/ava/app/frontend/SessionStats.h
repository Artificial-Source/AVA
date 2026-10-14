#pragma once

#include "FrontEnd.h"
#include "ModelSelection.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/runtime/Session.h"

#include <atomic>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ava::app::frontend {

// Frontend projection of canonical session statistics, including both active-context estimates.
// Refresh and snapshot access are confined to the frontend thread; dirty() may be called by backend notification threads.
class SessionStats
{
 private:
  FrontEnd const* frontend_;
  ModelSelection* model_selection_;
  // Preserve complete canonical copies for current and future session-stat projections.
  ava::session::SessionStats stats_snapshot_;
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
  // Add active-history and system-prompt estimates without overflowing the signed projection.
  static std::optional<long long> active_token_total(ava::session::SessionStats const& stats)
  {
    if (!stats.active_context_tokens || !stats.system_prompt_tokens)
      return std::nullopt;

    // Saturate totals that cannot be represented by the public signed projection type.
    auto const maximum_size = std::numeric_limits<std::size_t>::max();
    auto const size_total =
        *stats.active_context_tokens > maximum_size - *stats.system_prompt_tokens ? maximum_size : *stats.active_context_tokens + *stats.system_prompt_tokens;
    auto const maximum_display = std::numeric_limits<long long>::max();
    return size_total > static_cast<std::size_t>(maximum_display) ? maximum_display : static_cast<long long>(size_total);
  }

  // Format the exact total compactly for existing token-count presentation.
  static std::string token_count_display_str(std::optional<long long> total)
  {
    if (!total)
      return "unknown";
    if (*total < 1'000)
      return std::to_string(*total);

    auto format_scaled = [](long long tenths, char suffix) {
      std::ostringstream output;
      output << (tenths / 10);
      if (tenths % 10 != 0)
        output << '.' << (tenths % 10);
      output << suffix;
      return output.str();
    };
    return *total < 1'000'000 ? format_scaled(*total / 100, 'k') : format_scaled(*total / 100'000, 'M');
  }

  // Format context usage with one decimal, preserving very small positive usage and values above 100 percent.
  static std::string percentage_display_str(std::optional<long long> total, std::optional<long long> context_window_tokens)
  {
    if (!total || !context_window_tokens || *context_window_tokens <= 0)
      return "unknown";
    if (*total == 0)
      return "0.0%";
    long double const percentage = static_cast<long double>(*total) * 100.0L / static_cast<long double>(*context_window_tokens);
    if (percentage < 0.1L)
      return "<0.1%";
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << percentage << '%';
    return output.str();
  }

  // Copy one canonical stats snapshot under one lock and retain pending display changes until consumed.
  void refresh()
  {
    Data const* stats_data = frontend_->get_data(stats_index_);
    // stats_index_ must identify the stable SessionStats_ts address registered by initialize().
    ASSERT(dynamic_cast<runtime::SessionStats_ts const*>(stats_data) != nullptr);
    auto const* unlocked_stats = static_cast<runtime::SessionStats_ts const*>(stats_data);
    runtime::SessionStats_ts::crat stats_r{*unlocked_stats};

    auto const old_token_count = token_count_display_str(active_token_total_);
    // Keep the full frontend snapshot in step with every canonical refresh before deriving individual displays.
    stats_snapshot_ = *stats_r;
    auto const new_total = active_token_total(stats_snapshot_);
    auto const new_token_count = token_count_display_str(new_total);
    token_count_.changed_ = token_count_.changed_ || old_token_count != new_token_count;
    // Exact changes matter to percentage even when compact token text is unchanged.
    percentage_recompute_pending_ = percentage_recompute_pending_ || active_token_total_ != new_total;
    active_token_total_ = new_total;
  }

 public:
  // Wire this cache to frontend and the sibling model_selection projection, both of which must outlive it.
  // model_selection must be non-null and initialized before percentage refreshes.
  // This cache consumes its numeric context-window change notification.
  SessionStats(FrontEnd const* frontend, ModelSelection* model_selection) : frontend_(frontend), model_selection_(model_selection) { }

  // Connect this cache once to the registered canonical statistics at its stable address.
  void initialize(runtime::SessionStats_ts const* unlocked_stats)
  {
    // Only call initialize once.
    ASSERT(stats_index_.undefined());
    stats_index_ = unlocked_stats->index();
    dirty_ = true;
  }

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
    if (percentage_recompute_pending_)
    {
      auto const new_percentage = percentage_display_str(active_token_total_, context_window.value);
      percentage_.changed_ = percentage_.changed_ || percentage_.display_ != new_percentage;
      percentage_.display_ = new_percentage;
      percentage_recompute_pending_ = false;
    }
    bool const percentage_changed = percentage_.clear_changed();
    return {percentage_.display_, percentage_changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::app::frontend
