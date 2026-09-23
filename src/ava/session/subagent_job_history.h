#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::session {

inline constexpr long long kSubagentJobHistorySchemaVersion = 1;
inline constexpr std::size_t kMaxSubagentJobHistoryIdBytes = 96;
inline constexpr std::size_t kMaxSubagentJobHistoryTimestampBytes = 64;
inline constexpr std::size_t kMaxSubagentJobHistorySummaryBytes = 16U * 1024U;
inline constexpr std::size_t kMaxSubagentJobHistoryErrorBytes = 4U * 1024U;
inline constexpr std::size_t kMaxSubagentJobHistoryStopReasonBytes = 1024;
inline constexpr std::size_t kMaxSubagentJobHistoryAccountingValue = 1024U * 1024U;
inline constexpr std::size_t kMaxProjectedSubagentJobHistory = 64;

enum class SubagentJobHistoryPhase
{
  Start,
  Terminal,
};

enum class SubagentJobHistoryMode
{
  Foreground,
  Background,
};

enum class SubagentJobHistoryExecution
{
  Starting,
  Running,
  Completed,
  Failed,
  Canceled,
  Interrupted,
};

// Bounded display-only job lifecycle record nested in session_metadata.
// Prompts, credentials, raw tool args, paths, and launch authority are never stored.
struct SubagentJobHistoryRecord
{
  SubagentJobHistoryPhase phase = SubagentJobHistoryPhase::Start;
  std::string job_id = {};
  std::string task_id = {};
  std::string parent_session_id = {};
  std::string child_session_id = {};
  std::string delivery_id = {};
  SubagentJobHistoryMode mode = SubagentJobHistoryMode::Foreground;
  SubagentJobHistoryExecution execution = SubagentJobHistoryExecution::Starting;
  std::string started_at = {};
  std::string updated_at = {};
  std::optional<std::string> terminal_at = std::nullopt;
  std::optional<std::string> summary = std::nullopt;
  bool summary_truncated = false;
  std::optional<std::string> error = std::nullopt;
  bool error_truncated = false;
  std::optional<std::string> error_category = std::nullopt;
  std::optional<std::string> stop_reason = std::nullopt;
  bool stop_reason_truncated = false;
  std::size_t provider_iterations = 0;
  std::size_t tool_calls = 0;
  std::size_t tool_iterations = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SubagentJobHistoryView
{
  SubagentJobHistoryRecord record = {};
  bool unmatched_start = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string_view to_string(SubagentJobHistoryPhase value) noexcept;
[[nodiscard]] std::string_view to_string(SubagentJobHistoryMode value) noexcept;
[[nodiscard]] std::string_view to_string(SubagentJobHistoryExecution value) noexcept;

[[nodiscard]] bool session_metadata_has_subagent_job_history(SessionEntry const& entry);
[[nodiscard]] bool valid_subagent_job_history_object(std::string_view object);
[[nodiscard]] ava::core::Result<SessionEntry> make_subagent_job_history_entry(SubagentJobHistoryRecord record);
[[nodiscard]] ava::core::Result<std::optional<SubagentJobHistoryRecord>> parse_subagent_job_history_entry(SessionEntry const& entry);
// Display projection only. Records whose parent_session_id does not match are
// ignored so forks cannot inherit control. Last max_jobs unique jobs are kept.
[[nodiscard]] std::vector<SubagentJobHistoryView> project_subagent_job_history(std::string_view parent_session_id, std::vector<SessionEntry> const& entries,
                                                                               std::size_t max_jobs = kMaxProjectedSubagentJobHistory);

}  // namespace ava::session
