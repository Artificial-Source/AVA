#include "sys.h"
#ifdef CWDEBUG
#include "ava/debug/debug_ostream_operators.h"
#endif
#include "SessionStats.h"

#include <iomanip>
#include <limits>
#include <sstream>

namespace ava::app::frontend {

// Add active-history and system-prompt estimates without overflowing the signed projection.
//static
std::optional<long long> SessionStats::active_token_total(ava::session::SessionStats const& stats)
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
//static
std::string SessionStats::token_count_display_str(std::optional<long long> total)
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
//static
std::string SessionStats::percentage_display_str(std::optional<long long> total, std::optional<long long> context_window_tokens)
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
void SessionStats::refresh()
{
  Data const* stats_data = frontend_->get_data(stats_index_);
  // stats_index_ must identify the stable SessionStats_ts address registered by initialize().
  ASSERT(dynamic_cast<runtime::SessionStats_ts const*>(stats_data) != nullptr);
  auto const* unlocked_stats = static_cast<runtime::SessionStats_ts const*>(stats_data);
  runtime::SessionStats_ts::crat stats_r{*unlocked_stats};

  auto const old_token_count = token_count_display_str(active_token_total_);
  // Keep the full frontend snapshot in step with every canonical refresh before deriving individual displays.
  stats_snapshot_ = *stats_r;
  auto const new_active_token_total = active_token_total(stats_snapshot_);
  auto const new_token_count = token_count_display_str(new_active_token_total);
  bool const token_count_changed = old_token_count != new_token_count;
  Dout(dc::frontend(token_count_changed), "token_count changed from " << old_token_count << " to " << new_token_count);
  token_count_.changed_ = token_count_.changed_ || token_count_changed;

  // Exact changes matter to percentage even when compact token text is unchanged.
  bool const active_token_total_changed = active_token_total_ != new_active_token_total;
  Dout(dc::frontend(active_token_total_changed), "active_token_total changed from " << active_token_total_ << " to " << new_active_token_total);
  percentage_recompute_pending_ = percentage_recompute_pending_ || active_token_total_changed;
  active_token_total_ = new_active_token_total;
}

// Recompute the percentage display when an exact token total or context window change is pending.
void SessionStats::recompute_percentage(std::optional<long long> context_window_tokens)
{
  if (percentage_recompute_pending_)
  {
    auto const new_percentage = percentage_display_str(active_token_total_, context_window_tokens);
    bool const percentage_changed = percentage_.display_ != new_percentage;
    Dout(dc::frontend, "percentage changed from " << percentage_.display_ << " to " << new_percentage);
    percentage_.changed_ = percentage_.changed_ || percentage_changed;
    percentage_.display_ = new_percentage;
    percentage_recompute_pending_ = false;
  }
}

// Connect this cache once to the registered canonical statistics at its stable address.
void SessionStats::initialize(runtime::SessionStats_ts const* unlocked_stats)
{
  // Only call initialize once.
  ASSERT(stats_index_.undefined());
  stats_index_ = unlocked_stats->index();
  dirty_ = true;
}

} // namespace ava::app::frontend
