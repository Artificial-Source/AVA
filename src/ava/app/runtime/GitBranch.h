#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/frontend/UnlockedData.h"
#include "ava/app/runtime/session_ts.h"

#include <filesystem>
#include <string>

namespace ava::app::runtime {

// The last observed textual Git HEAD; an empty name means that HEAD was unavailable.
// A dedicated value type keeps frontend notification routing distinct from unrelated observable strings.
struct GitBranch
{
  std::string name;

  bool operator==(GitBranch const&) const = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};
using GitBranch_ts = frontend::UnlockedData<GitBranch>;

// Read the workspace's textual Git HEAD. Returns an empty string when HEAD is unavailable.
[[nodiscard]] std::string observe_git_branch(std::filesystem::path const& workspace);

// Refresh the canonical branch without holding the session lock during filesystem access.
// Returns true only when the observed value was published to the same session workspace.
[[nodiscard]] bool refresh_git_branch(session_ts& unlocked_session);

} // namespace ava::app::runtime
