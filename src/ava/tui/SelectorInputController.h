// Classifies selector input and requests catalog changes through the frontend.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"

#include <string>
#include <utility>
#include <variant>

namespace ava::tui {

class SelectorInputActions;

class RuntimeRenderer;
class SelectorHost;
namespace terminal {
class Context;
}
namespace runtime_input {
struct RuntimeInput;
}

// TUI-thread catalog result; Session handles any requested work after selector dispatch.
enum class SelectorInputRequest
{
  // This owner did not handle the input; the host can retain the current view.
  Pass,
  // The owner already applied the change or its feedback; Session performs final rendering.
  Handled,
  // Painting failed before a synchronous frontend operation; Session stops dispatch.
  RenderFailed,
  // A successful model switch requests thinking choices for the new model.
  OpenReasoning
};

// TUI-thread tag tells Session whether result text is a draft, success feedback or an error.
enum class SelectorInputTextKind
{
  // Transfer editor text after retiring its selector; no command executes here.
  DraftCommand,
  // Success feedback follows reconciliation with the backend committed session identity.
  OpenedSession,
  // An action may have committed before failing; reconcile identity before publishing this error.
  ReconcileSessionError
};

// Owned values remain valid after selector replacement; this call-scoped handoff retains no row borrows.
using SelectorInputOutcome = std::variant<SelectorInputRequest, std::pair<SelectorInputTextKind, std::string>>;

// Session owns this TUI-thread catalog policy, used by SelectorHost dispatch.
// Settings, session browsing and summary lifetimes have dedicated owners. All
// collaborators outlive it; synchronous frontend calls return owned results before
// rows are replaced. It owns no workers or locks and needs no asynchronous teardown.
class SelectorInputController final
{
 public:
  // TUI thread: bind input classification, feedback and frontend callbacks without loading a
  // catalog. All collaborators outlive this controller; dispatch reads their current views.
  SelectorInputController(SelectorInputActions& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                          terminal::Context& terminal_context);
  // Not copyable: keep one controller bound to the session's selector and feedback owners.
  SelectorInputController(SelectorInputController const&) = delete;
  SelectorInputController& operator=(SelectorInputController const&) = delete;

  // TUI thread: require an open view and classify hit-testing/bindings without changing it.
  // Settings pointer hits highlight first; scoped-model bindings precede generic list keys.
  [[nodiscard]] SelectListInputResult classify(runtime_input::RuntimeInput const& input) const;
  // TUI thread: handle catalog actions; Session applies returned draft/session changes and renders.
  // ReconcileSessionError must reconcile identity before publishing its text.
  [[nodiscard]] SelectorInputOutcome handle(SelectListInputResult const& input);

  // Borrowed owners print as type/address; their content is never traversed.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI thread: keep scoped edits in the current list; true means this action was handled.
  bool handle_scoped_models(SelectListInputResult const& input);
  // TUI thread: publish fresh rows while preserving selected identity/query, or keep
  // the old view and beep on failure. The caller supplies a completed frontend result.
  void apply_scoped_update(core::Result<SelectListView> updated, SelectListInputResult const& input, std::string status);

  // Frontend-owned narrow selector interface; outlives this TUI-thread controller.
  SelectorInputActions& frontend_;
  // Borrowed from Session; display state; receives status and refreshed backend values.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; selector owner; row borrows cannot survive a host mutation.
  SelectorHost& selectors_;
  // Borrowed from Session; renderer paints pending status before blocking frontend callbacks.
  RuntimeRenderer& renderer_;
  // Terminal outlives the controller; only the TUI thread emits rejection bells here.
  terminal::Context& terminal_context_;
};

} // namespace ava::tui
