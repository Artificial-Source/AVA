#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/agent/subagent_coordinator.h"
#include "ava/tui/composer.h"
#include "ava/tui/runtime.h"
#include "ava/core/result.h"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace ava::app {

// Builds the interactive, path-free /jobs selector. Exact job ids remain only
// in SelectListItemView::value as control authority and are never rendered.
[[nodiscard]] ava::tui::SelectListView subagent_selector_view(std::vector<ava::agent::SubagentCoordinatorJobSnapshot> const& snapshots);
[[nodiscard]] ava::core::Result<ava::tui::SelectListView> subagent_selector_view(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator,
                                                                                 std::string_view parent_session_id);

// Pure control-truthfulness mappers. Accept coordinator Result/snapshot state
// only and return narrow TUI enums — never Error::format or backend text.
[[nodiscard]] ava::tui::SubagentWorkspaceCancelOutcome map_subagent_cancel_outcome(ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> const& result);
[[nodiscard]] ava::tui::SubagentWorkspacePromoteOutcome map_subagent_promote_outcome(
    ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> const& promote_result,
    std::optional<ava::agent::SubagentCoordinatorJobSnapshot> const& owner_bound_status = std::nullopt);

// Owner-bound workspace control seams used by the interactive TUI callbacks.
[[nodiscard]] ava::tui::SubagentWorkspaceCancelOutcome cancel_subagent_for_workspace(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator,
                                                                                     std::string_view parent_session_id, std::string_view job_id);
[[nodiscard]] ava::tui::SubagentWorkspacePromoteOutcome promote_subagent_for_workspace(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator,
                                                                                       std::string_view parent_session_id, std::string_view job_id);

}  // namespace ava::app
