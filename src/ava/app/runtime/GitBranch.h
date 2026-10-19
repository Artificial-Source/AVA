#pragma once

#include "ava/app/runtime/UnlockedData.h"
#include "ava/app/runtime/session_ts.h"

#include <filesystem>
#include <string>

namespace ava::app::runtime {

using GitBranch_ts = UnlockedData<std::string>;

// Read the workspace's textual Git HEAD. Returns an empty string when HEAD is unavailable.
[[nodiscard]] std::string observe_git_branch(std::filesystem::path const& workspace);

// Refresh the canonical branch without holding the session lock during filesystem access.
// Returns true only when the observed value was published to the same session workspace.
[[nodiscard]] bool refresh_git_branch(session_ts& unlocked_session);

} // namespace ava::app::runtime
