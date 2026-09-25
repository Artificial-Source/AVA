#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/commands.h"
#include "ava/app/runtime.h"
#include "ava/session/subagent_job_history.h"
#include "ava/core/error.h"
#include "ava/core/result.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

// Bounded display snapshot captured once for /jobs list/history/show fallback.
// load_error is set when session history could not be read; records stay empty.
struct JobsHistorySnapshot
{
  std::vector<ava::session::SubagentJobHistoryView> records = {};
  std::optional<ava::core::Error> load_error = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Value snapshot for the TUI active-run command callback. Holds a coordinator
// shared_ptr, parent session id, and history captured before the run starts.
struct JobsCommandBinding
{
  std::shared_ptr<ava::agent::SubagentCoordinator> coordinator;
  std::string parent_session_id;
  JobsHistorySnapshot history;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Frontend-neutral eligibility parser for the narrow active-run TUI command
// lane. It never classifies other slash commands or opens modal state.
[[nodiscard]] std::optional<std::string_view> active_jobs_command_arguments(std::string_view submitted) noexcept;

[[nodiscard]] JobsHistorySnapshot load_jobs_history_snapshot(runtime::session_ts const& unlocked_session, std::string_view parent_session_id);
[[nodiscard]] JobsCommandBinding capture_jobs_command_binding(runtime::session_ts& unlocked_session);

[[nodiscard]] ava::core::Result<CommandResult> run_jobs_command_1(runtime::session_ts& unlocked_session, std::string_view arguments = {});
[[nodiscard]] ava::core::Result<CommandResult> run_jobs_command(JobsCommandBinding const& binding, std::string_view arguments, bool active_run);
[[nodiscard]] ava::core::Result<CommandResult> run_jobs_command(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator,
                                                                std::string_view parent_session_id, std::string_view arguments, bool active_run,
                                                                std::vector<ava::session::SubagentJobHistoryView> historical = {},
                                                                std::optional<ava::core::Error> history_error = std::nullopt);

}  // namespace ava::app
