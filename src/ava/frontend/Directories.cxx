#include "sys.h"
#include "Directories.h"

namespace ava::frontend {

// Compare both displayed paths, then retain a full copy including additional writable directories.
// Only an actual path-text change marks the corresponding display string pending.
void Directories::refresh_snapshot()
{
  Data const* data = frontend_->get_data(index_);
  // Initialize with a stable registered Directories_ts before refreshing this projection.
  ASSERT(dynamic_cast<app::runtime::Directories_ts const*>(data) != nullptr);
  auto const* unlocked_directories = static_cast<app::runtime::Directories_ts const*>(data);
  app::runtime::Directories_ts::crat directories_r(*unlocked_directories);

  workspace_root_.changed_ = workspace_root_.changed_ || directories_snapshot_.workspace_dir != directories_r->workspace_dir;
  current_directory_.changed_ = current_directory_.changed_ || directories_snapshot_.current_dir != directories_r->current_dir;
  directories_snapshot_ = *directories_r;
}

// Connect to unlocked_directories at its registered final address and request an initial copy.
void Directories::initialize(app::runtime::Directories_ts const* unlocked_directories)
{
  // Call initialize exactly once for each frontend projection.
  ASSERT(index_.undefined());
  index_ = unlocked_directories->index();
  dirty_ = true;
}

// Format the snapshot's workspace root without doing backend directory discovery on the TUI thread.
RefreshResult<std::string const&> Directories::workspace_root_refresh()
{
  // Initialize before requesting the workspace-root display.
  ASSERT(!index_.undefined());
  if (dirty_.exchange(false))
    refresh_snapshot();
  bool const changed = workspace_root_.clear_changed();
  if (changed)
    workspace_root_.display_ = directories_snapshot_.workspace_dir.string();
  return {workspace_root_.display_, changed};
}

// Format the snapshot's current directory, which may differ from the workspace root.
RefreshResult<std::string const&> Directories::current_directory_refresh()
{
  // Initialize before requesting the current-directory display.
  ASSERT(!index_.undefined());
  if (dirty_.exchange(false))
    refresh_snapshot();
  bool const changed = current_directory_.clear_changed();
  if (changed)
    current_directory_.display_ = directories_snapshot_.current_dir.string();
  return {current_directory_.display_, changed};
}

} // namespace ava::frontend
