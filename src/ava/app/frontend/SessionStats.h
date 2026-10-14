#pragma once

#include "FrontEnd.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/runtime/Session.h"

#include <atomic>
#include <limits>
#include <sstream>

namespace ava::app::frontend {

// Frontend projection of canonical session statistics and the system-prompt estimate.
// Refresh and snapshot access are confined to the frontend thread; dirty() may be called by backend notification threads.
class SessionStats
{
 private:
  FrontEnd const* frontend_;
  ava::session::SessionStats stats_snapshot_;
  runtime::SystemPromptTokenEstimate system_prompt_snapshot_;
  DataListIndex stats_index_;
  DataListIndex system_prompt_index_;
  std::atomic_bool dirty_;
  DisplayString token_count_;

 private:
  // Format active context tokens using integer-truncated k/m tenths; absence is displayed as "unknown".
  static std::string token_count_display_str(ava::session::SessionStats const& stats, runtime::SystemPromptTokenEstimate const& system_prompt)
  {
    if (!stats.active_context_tokens || !system_prompt.tokens)
      return "unknown";

    auto const maximum_size = std::numeric_limits<std::size_t>::max();
    auto const size_total =
        *stats.active_context_tokens > maximum_size - *system_prompt.tokens ? maximum_size : *stats.active_context_tokens + *system_prompt.tokens;
    auto const maximum_display = std::numeric_limits<long long>::max();
    auto const total = size_total > static_cast<std::size_t>(maximum_display) ? maximum_display : static_cast<long long>(size_total);
    if (total < 1'000)
      return std::to_string(total);

    auto format_scaled = [](long long tenths, char suffix) {
      std::ostringstream output;
      output << (tenths / 10);
      if (tenths % 10 != 0)
        output << '.' << (tenths % 10);
      output << suffix;
      return output.str();
    };
    return total < 1'000'000 ? format_scaled(total / 100, 'k') : format_scaled(total / 100'000, 'M');
  }

  // Copy both registered canonical values in a fixed lock order and retain pending display changes until consumed.
  void refresh()
  {
    Data const* stats_data = frontend_->get_data(stats_index_);
    Data const* system_prompt_data = frontend_->get_data(system_prompt_index_);
    // stats_index_ must identify the stable SessionStats_ts address registered by initialize().
    ASSERT(dynamic_cast<runtime::SessionStats_ts const*>(stats_data) != nullptr);
    // system_prompt_index_ must identify the stable SystemPromptTokenEstimate_ts address registered by initialize().
    ASSERT(dynamic_cast<runtime::SystemPromptTokenEstimate_ts const*>(system_prompt_data) != nullptr);
    auto const* unlocked_stats = static_cast<runtime::SessionStats_ts const*>(stats_data);
    auto const* unlocked_system_prompt = static_cast<runtime::SystemPromptTokenEstimate_ts const*>(system_prompt_data);
    runtime::SessionStats_ts::crat stats_r{*unlocked_stats};
    runtime::SystemPromptTokenEstimate_ts::crat system_prompt_r{*unlocked_system_prompt};

    auto const old_token_count = token_count_display_str(stats_snapshot_, system_prompt_snapshot_);
    auto const new_token_count = token_count_display_str(*stats_r, *system_prompt_r);
    token_count_.changed_ = token_count_.changed_ || old_token_count != new_token_count;
    stats_snapshot_ = *stats_r;
    system_prompt_snapshot_ = *system_prompt_r;
  }

 public:
  explicit SessionStats(FrontEnd const* frontend) : frontend_(frontend) { }

  // Connect this cache once to both registered canonical objects at their stable addresses.
  void initialize(runtime::SessionStats_ts const* unlocked_stats, runtime::SystemPromptTokenEstimate_ts const* unlocked_system_prompt)
  {
    // Only call initialize once.
    ASSERT(stats_index_.undefined());
    // Only call initialize once for the paired prompt estimate.
    ASSERT(system_prompt_index_.undefined());
    stats_index_ = unlocked_stats->index();
    system_prompt_index_ = unlocked_system_prompt->index();
    dirty_ = true;
  }

  void dirty() { dirty_ = true; }

  // Return the compact active-history plus system-prompt estimate and consume only its pending display-change flag.
  RefreshResult token_count_refresh()
  {
    // Call initialize first.
    ASSERT(!stats_index_.undefined());
    // Call initialize with the paired prompt estimate first.
    ASSERT(!system_prompt_index_.undefined());
    if (dirty_.exchange(false))
      refresh();
    bool const token_count_changed = token_count_.clear_changed();
    if (token_count_changed)
      token_count_.display_ = token_count_display_str(stats_snapshot_, system_prompt_snapshot_);
    return {token_count_.display_, token_count_changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::app::frontend
