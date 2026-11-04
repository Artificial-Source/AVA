// Supplies confirmed Settings actions and Settings display facts to the TUI feature.
// See docs/engineering/tui-architecture.md for frontend and feature lifetimes.
#pragma once

#include "keybindings.h"
#include "runtime.h"
#include "ava/debug/print_members_on.h"

#include <array>
#include <string>

namespace ava::tui {

// The application frontend implements this interface; SettingsDialog and its row builder borrow it.
// All calls run synchronously on the TUI thread. The frontend outlives the feature
// and any work it starts. Arguments are borrowed for each call; returned values
// own their data unless a method says otherwise. Backend validation and persistence
// stay in the application implementation; this interface owns no state or worker.
class SettingsDialogActions
{
 public:
  // Destroy with the frontend after all feature borrows and application work have
  // retired. Destroying the interface itself does not cancel or join any work.
  virtual ~SettingsDialogActions() = default;

  // Apply settings token value and return user-facing status or command error.
  virtual core::Result<std::string> on_settings_selected(std::string_view value) = 0;

  // Snapshot project trust for settings presentation without exposing operations that change trust.
  virtual ProjectTrustSnapshot read_project_trust() const = 0;

  // Load and validate application keybindings without mutating TUI state.
  virtual core::Result<TuiKeyBindings> on_reload_key_bindings() const = 0;

  // Persists and applies a recognized display command or settings action on the TUI thread.
  virtual core::Result<TuiLocalDisplaySettingsResult> on_local_display_settings_action(std::string_view action) = 0;

  // TUI thread: borrow the current map until frontend destruction. Applying a new
  // map preserves the map's address; callers must finish dispatch before updates.
  virtual TuiKeyBindings const& key_bindings() const = 0;

  // TUI thread when building the Model section only: refresh and copy provider,
  // model and reasoning labels in that order. Refresh consumes their change flags,
  // so other Settings sections must not perform this read speculatively.
  virtual std::array<std::string, 3> read_settings_model_labels() = 0;

  // The interface has no state; debug output identifies only its type and address.
  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

} // namespace ava::tui
