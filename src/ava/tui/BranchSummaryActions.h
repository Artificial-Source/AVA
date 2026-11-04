// Supplies generation-checked branch-summary commands and snapshots to the TUI feature.
// See docs/engineering/tui-architecture.md for frontend and feature lifetimes.
#pragma once

#include "keybindings.h"
#include "runtime.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

// The application frontend implements this interface; BranchSummaryOperation borrows it.
// All calls run synchronously on the TUI thread. The frontend outlives the feature
// and any work it starts. Arguments are borrowed for each call; returned values
// own their data unless a method says otherwise. Backend validation and persistence
// stay in the application implementation; this interface owns no state or worker.
class BranchSummaryActions
{
 public:
  // Destroy with the frontend after all feature borrows and application work have
  // retired. Destroying the interface itself does not cancel or join any work.
  virtual ~BranchSummaryActions() = default;

  // Prepare a summary for session_id and return its path-free operation snapshot.
  virtual core::Result<TuiBranchSummarySnapshot> on_branch_summary_prepare(std::string_view session_id) const = 0;

  // TUI thread: read the application operation without joining its worker; the returned value is a presentation copy.
  virtual TuiBranchSummarySnapshot branch_summary_snapshot() const = 0;

  // Confirm generation; false means it was not confirmable and errors preserve backend detail boundaries.
  virtual core::Result<bool> on_branch_summary_confirm(std::uint64_t generation) const = 0;

  // Cancel generation; false means it was not cancelable and errors preserve backend detail boundaries.
  virtual core::Result<bool> on_branch_summary_cancel(std::uint64_t generation) const = 0;

  // Refresh the session catalog after summary completion or return its refresh error.
  virtual core::Result<SelectListView> on_branch_summary_refresh_catalog() = 0;

  // TUI thread: borrow the current map until frontend destruction. Applying a new
  // map preserves the map's address; callers must finish dispatch before updates.
  virtual TuiKeyBindings const& key_bindings() const = 0;

  // The interface has no state; debug output identifies only its type and address.
  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

} // namespace ava::tui
