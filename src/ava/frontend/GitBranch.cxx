#include "sys.h"
#include "FrontEnd.h"
#include "GitBranch.h"

namespace ava::frontend {

// Copy the last observed canonical branch without doing Git I/O during rendering.
void GitBranch::refresh()
{
  Data const* data = frontend_->get_data(index_);
  // Initialize with a stable registered GitBranch_ts before refreshing this projection.
  ASSERT(dynamic_cast<app::runtime::GitBranch_ts const*>(data) != nullptr);
  auto const* unlocked_git_branch = static_cast<app::runtime::GitBranch_ts const*>(data);
  app::runtime::GitBranch_ts::crat git_branch_r(*unlocked_git_branch);
  bool const changed = git_branch_snapshot_ != git_branch_r->name;
  git_branch_.changed_ = git_branch_.changed_ || changed;
  git_branch_snapshot_ = git_branch_r->name;
}

// Remember the canonical branch's registration index, rather than retaining a movable Session pointer.
void GitBranch::initialize(app::runtime::GitBranch_ts const* unlocked_git_branch)
{
  // Call initialize exactly once for each frontend projection.
  ASSERT(index_.undefined());
  index_ = unlocked_git_branch->index();
  dirty_ = true;
}

} // namespace ava::frontend
