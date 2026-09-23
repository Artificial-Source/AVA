#pragma once

#include "ava/app/commands.h"
#include "ava/app/runtime.h"
#include "ava/session/subagent_job_history.h"
#include "ava/core/result.h"

#include <string_view>
#include <vector>

namespace ava::app {

// Frontend-neutral eligibility parser for the narrow active-run TUI command
// lane. It never classifies other slash commands or opens modal state.
[[nodiscard]] std::optional<std::string_view> active_jobs_command_arguments(std::string_view submitted) noexcept;

[[nodiscard]] ava::core::Result<CommandResult> run_jobs_command_1(runtime::session_ts& unlocked_session, std::string_view arguments = {});
[[nodiscard]] ava::core::Result<CommandResult> run_jobs_command(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator,
                                                                std::string_view parent_session_id, std::string_view arguments, bool active_run,
                                                                std::vector<ava::session::SubagentJobHistoryView> historical = {});

}  // namespace ava::app
