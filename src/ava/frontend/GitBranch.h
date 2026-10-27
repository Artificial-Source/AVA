#pragma once

#include "Data.h"
#include "DisplayString.h"
#include "RefreshResult.h"
#include "ava/app/runtime/GitBranch.h"
#include "ava/core/NoLogString.h"

#include <atomic>
#include <string>
#include "debug.h"

namespace ava::frontend {

class FrontEnd;

// Cache a frontend-thread snapshot of the backend's last observed Git branch.
// Backend notifications only invalidate the snapshot; they do not read the filesystem.
class GitBranch
{
 private:
  FrontEnd const* frontend_;
  // Exact copy of canonical ava::app::runtime::GitBranch::name protected by GitBranch_ts.
  ava::core::NoLogString git_branch_snapshot_;
  DisplayString git_branch_;
  std::atomic_bool dirty_;
  DataListIndex index_;

  // Copy the registered branch under its read lock and preserve pending display changes.
  void refresh();

 public:
  // Construct a projection for frontend, which must outlive this object.
  explicit GitBranch(FrontEnd const* frontend) : frontend_(frontend) { }

  // Connect once to the stable registered unlocked_git_branch and request the initial copy.
  void initialize(app::runtime::GitBranch_ts const* unlocked_git_branch);

  // Mark the snapshot stale from a backend notification thread.
  void dirty() { dirty_ = true; }

  // Return the current branch display and consume its pending changed flag on the frontend thread.
  // An empty display means that no branch was observed at the most recent backend refresh.
  RefreshResult<std::string const&> git_branch_refresh()
  {
    // Initialize before requesting the branch display.
    ASSERT(!index_.undefined());
    if (dirty_.exchange(false))
      refresh();
    bool const changed = git_branch_.clear_changed();
    if (changed)
      git_branch_.display_ = git_branch_snapshot_;
    return {git_branch_.display_, changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::frontend
