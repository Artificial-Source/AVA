// Supplies session browsing, archive and open actions to the TUI feature.
// See docs/engineering/tui-architecture.md for frontend and feature lifetimes.
#pragma once

#include "runtime.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

// The application frontend implements this interface; SessionSelector borrows it.
// All calls run synchronously on the TUI thread. The frontend outlives the feature
// and any work it starts. Arguments are borrowed for each call; returned values
// own their data unless a method says otherwise. Backend validation and persistence
// stay in the application implementation; this interface owns no state or worker.
class SessionSelectorActions
{
 public:
  // Destroy with the frontend after all feature borrows and application work have
  // retired. Destroying the interface itself does not cancel or join any work.
  virtual ~SessionSelectorActions() = default;

  // TUI thread: update application-owned sort preferences before rebuilding selector rows.
  virtual SelectListView on_session_selector_sort_cycle() = 0;

  // TUI thread: change catalog filtering before publishing the replacement selector rows.
  virtual SelectListView on_session_selector_named_filter_toggle() = 0;

  // TUI thread: rebuild row labels from the new path-display preference, retaining backend row identity.
  virtual SelectListView on_session_selector_path_display_toggle() = 0;

  // TUI thread: rebuild the displayed catalog after changing which archived sessions are visible.
  virtual SelectListView on_session_selector_archived_filter_toggle() = 0;

  // TUI thread: rebuild row labels after changing timestamp visibility without changing session identity.
  virtual SelectListView on_session_selector_label_timestamp_toggle() = 0;

  // Archive session_id and return the refreshed selector, or a command/catalog error.
  virtual core::Result<SelectListView> on_session_selector_archive(std::string_view session_id) = 0;

  // Unarchive session_id and return the refreshed selector, or a command/catalog error.
  virtual core::Result<SelectListView> on_session_selector_unarchive(std::string_view session_id) = 0;

  // Open the parent of session_id or return validation, lookup, open, or replacement errors.
  virtual core::Result<std::string> on_session_selector_branch_parent(std::string_view session_id) = 0;

  // Open the selected child of session_id or return validation, lookup, open, or replacement errors.
  virtual core::Result<std::string> on_session_selector_branch_child(std::string_view session_id) = 0;

  // Open session value and return user-facing status or validation/open/replacement errors.
  virtual core::Result<std::string> on_session_selected(std::string_view value) = 0;

  // Reset session-selector preferences and return the current application-owned session view.
  virtual SelectListView session_selector_view() = 0;

  // The interface has no state; debug output identifies only its type and address.
  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

} // namespace ava::tui
