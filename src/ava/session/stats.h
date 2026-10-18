#pragma once

#include "ava/app/runtime/UnlockedData.h"
#include "ava/session/session_store.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ava::session {

struct SessionEntryCounts
{
  std::size_t session_start = 0;
  std::size_t session_metadata = 0;
  std::size_t user_message = 0;
  std::size_t assistant_message = 0;
  std::size_t tool_call = 0;
  std::size_t tool_result = 0;
  std::size_t permission_decision = 0;
  std::size_t mode_change = 0;
  std::size_t model_change = 0;
  std::size_t reasoning_block = 0;
  std::size_t reasoning_change = 0;
  std::size_t compaction = 0;
  std::size_t branch_summary = 0;
  std::size_t error = 0;
  std::size_t cancel = 0;

  // Compare all entry counts with other using memberwise equality.
  bool operator==(SessionEntryCounts const& other) const = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionStats
{
  std::size_t entry_count = 0;
  std::string first_timestamp;
  std::string last_timestamp;
  SessionEntryCounts counts;
  std::optional<long long> input_tokens;
  std::optional<long long> output_tokens;
  std::optional<long long> reasoning_tokens;
  std::optional<long long> cache_read_tokens;
  std::optional<long long> cache_write_tokens;
  std::optional<long long> total_tokens;
  std::optional<long long> estimated_input_bytes;
  std::optional<long long> estimated_output_bytes;
  std::optional<long long> estimated_total_bytes;
  // Runtime context estimates are published together so consumers never observe a history/prompt pair from different refreshes.
  // active_context_tokens covers retained history after the latest valid compaction and excludes the system prompt.
  std::optional<std::size_t> active_context_tokens;
  std::optional<std::size_t> system_prompt_tokens;
  std::optional<long double> total_cost_usd;
  std::optional<long double> known_cost_usd;
  bool cost_complete = true;
  std::size_t unknown_cost_entries = 0;
  std::size_t exact_usage_entries = 0;
  std::size_t estimated_usage_entries = 0;

  // Compare all statistics, including nested entry counts, with other using memberwise equality.
  bool operator==(SessionStats const& other) const = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Canonical session statistics, including cumulative provider usage and the paired active-context estimates.
using SessionStats_ts = app::runtime::UnlockedData<SessionStats>;

// Produces stats only from a classifier-valid logical history. A malformed v4
// transaction is never silently omitted from accounting or status surfaces.
[[nodiscard]] ava::core::Result<SessionStats> compute_session_stats(std::vector<SessionEntry> const& entries);

}  // namespace ava::session
