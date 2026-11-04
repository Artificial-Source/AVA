// Supplies model choices, scoped models and user-turn selection to the TUI feature.
// See docs/engineering/tui-architecture.md for frontend and feature lifetimes.
#pragma once

#include "keybindings.h"
#include "runtime.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

// The application frontend implements this interface; SelectorInputController borrows it.
// All calls run synchronously on the TUI thread. The frontend outlives the feature
// and any work it starts. Arguments are borrowed for each call; returned values
// own their data unless a method says otherwise. Backend validation and persistence
// stay in the application implementation; this interface owns no state or worker.
class SelectorInputActions
{
 public:
  // Destroy with the frontend after all feature borrows and application work have
  // retired. Destroying the interface itself does not cancel or join any work.
  virtual ~SelectorInputActions() = default;

  // Resolve and commit provider/model value and return user-facing status or resolution error.
  virtual core::Result<std::string> on_model_selected(std::string_view value) = 0;

  // Commit level, where nullopt restores provider default, and return user-facing status.
  virtual core::Result<std::string> on_reasoning_selected(std::optional<std::string> level) = 0;

  // Fork from public user entry_id and return committed-session status or a truthful failure.
  virtual core::Result<std::string> on_fork_user_turn_selected(std::string_view entry_id) = 0;

  // Re-read public user entry_id text at action time or return session storage errors.
  virtual core::Result<std::string> on_read_user_turn_text(std::string_view entry_id) const = 0;

  // Toggle scoped model value against previous and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_toggled(SelectListView const& previous, std::string_view value) const = 0;

  // Enable visible scoped-model values and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_enable_all(SelectListView const& previous, std::vector<std::string> values) const = 0;

  // Clear visible scoped-model values and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_clear_all(SelectListView const& previous, std::vector<std::string> values) const = 0;

  // Toggle the provider represented by value and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_toggle_provider(SelectListView const& previous, std::string_view value) const = 0;

  // Reorder value up or down and return the rebuilt selector or validation error.
  virtual core::Result<SelectListView> on_scoped_model_reorder(SelectListView const& previous, std::string_view value, bool up) const = 0;

  // Persist the staged scoped-model cycle and return user-facing status or persistence error.
  virtual core::Result<std::string> on_scoped_model_save() const = 0;

  // TUI thread: borrow the current map until frontend destruction. Applying a new
  // map preserves the map's address; callers must finish dispatch before updates.
  virtual TuiKeyBindings const& key_bindings() const = 0;

  // The interface has no state; debug output identifies only its type and address.
  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

} // namespace ava::tui
