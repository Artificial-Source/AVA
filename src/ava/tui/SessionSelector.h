// Owns session browsing and confirmation before frontend session actions.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

class SessionSelectorActions;

class SelectorHost;
class RuntimeRenderer;
namespace terminal {
class Context;
}

// Session owns this TUI-thread browsing and archive-confirmation policy. SelectorHost
// uses it while the session list is active. Frontend callbacks own persistence and
// session changes; this owner updates display data and returns outcomes. All borrowed
// owners outlive it. No worker refers to it, and destruction discards only local confirmation.
class SessionSelector final
{
 public:
  // TUI thread: start without archive intent; open loads the current catalog later.
  // Borrowed session collaborators outlive this selector and each synchronous frontend call.
  SessionSelector(SessionSelectorActions& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                  terminal::Context& terminal_context);
  // Not copyable: one stable selector owns the two-press archive confirmation.
  SessionSelector(SessionSelector const&) = delete;
  SessionSelector& operator=(SessionSelector const&) = delete;

  // TUI thread: cancel old archive intent, load current catalog and show the live tail.
  // Return the render request result, not whether a session was opened.
  [[nodiscard]] bool open();
  // TUI thread: disarm archive/restore after navigation or a change of selector ownership.
  void cancel_confirmation();
  // TUI thread: apply supported catalog sort/filter toggles and preserve the selected identity.
  void browse(SelectListInputResult const& input);
  // TUI thread: require two matching archive/restore intents for the same eligible row.
  // Current sessions cannot be archived; success refreshes rows and retires confirmation.
  void archive(SelectListInputResult const& input);
  // TUI thread: validate a rename/label row and copy its identity before closing the list.
  // Return owned command text for Composer, or keep the view and report unavailable.
  [[nodiscard]] std::optional<std::string> draft_command(SelectListInputResult const& input);
  // TUI thread: copy selected identity, retire the list and paint pending status before
  // the frontend opens it. The caller checks the backend's committed identity even on
  // failure: the return value alone cannot establish whether the session changed.
  [[nodiscard]] core::Result<std::string> resolve(SelectListInputResult const& input);
  // TUI thread: validate the row and paint before opening a parent/child session. No value
  // means no action was possible; an error still requires caller identity reconciliation.
  [[nodiscard]] std::optional<core::Result<std::string>> navigate_branch(SelectListInputResult const& input);

  // References print as type/address; the archive identity uses NoLogString.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI thread: retire archive intent and publish a refreshed list with the prior query
  // and row identity, followed by the action status.
  void preserve_selection(SelectListInputResult const& input, SelectListView view, std::string status);

  // Frontend-owned narrow session interface; outlives this TUI-thread selector.
  SessionSelectorActions& frontend_;
  // Borrowed from Session; display state; receives transient status updates.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; sole list storage; selected row pointers expire when it changes.
  SelectorHost& selectors_;
  // Borrowed from Session; renderer paints pending status before synchronous session handoffs.
  RuntimeRenderer& renderer_;
  // Terminal outlives this owner and emits TUI-thread rejection feedback.
  terminal::Context& terminal_context_;
  // TUI-thread first-press session identity and archive direction. A matching second
  // press executes; navigation, replacement and completion clear it.
  std::optional<PendingSessionArchiveAction> archive_confirmation_;
};

} // namespace ava::tui
