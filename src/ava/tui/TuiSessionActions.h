// Supplies session mode, committed identity and diagram callbacks to the TUI feature.
// See docs/engineering/tui-architecture.md for frontend and feature lifetimes.
#pragma once

#include "keybindings.h"
#include "runtime.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

// The application frontend implements this interface; TuiSession borrows it.
// All calls run synchronously on the TUI thread. The frontend outlives the feature
// and any work it starts. Arguments are borrowed for each call; returned values
// own their data unless a method says otherwise. Backend validation and persistence
// stay in the application implementation; this interface owns no state or worker.
class TuiSessionActions
{
 public:
  // Destroy with the frontend after all feature borrows and application work have
  // retired. Destroying the interface itself does not cancel or join any work.
  virtual ~TuiSessionActions() = default;

  // TUI thread: return diagram callbacks that retain the application renderer's
  // resources. Removing a UI completion notifier does not stop or join its worker.
  virtual TuiMermaidRenderBridge mermaid_bridge() const = 0;

  // Toggle the committed Build/Plan mode or return the command error.
  virtual core::VoidResult on_toggle_mode() const = 0;

  // TUI thread: read committed identity after a backend action, so UI reset follows the actual session transition.
  virtual std::string read_committed_session_id() const = 0;

  // TUI thread: borrow the current map until frontend destruction. Applying a new
  // map preserves the map's address; callers must finish dispatch before updates.
  virtual TuiKeyBindings const& key_bindings() const = 0;

  // The interface has no state; debug output identifies only its type and address.
  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

} // namespace ava::tui
