// Classifies selector input and delegates catalog changes to frontend authority.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"

#include <string>
#include <utility>
#include <variant>

namespace ava::tui {

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
  // This owner did not claim the input; the host can retain the current view.
  Pass,
  // The owner already applied the change or its feedback; Session performs final rendering.
  Handled,
  // A required synchronous authority handoff paint failed; Session stops dispatch.
  RenderFailed,
  // A successful model switch requests thinking choices for the new model.
  OpenReasoning
};

// TUI-thread tag keeps owned result text distinct from commands or session authority.
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
  // TUI thread: bind input classification, feedback and authority without loading a
  // catalog. All collaborators outlive this controller; dispatch reads their current views.
  SelectorInputController(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                          terminal::Context& terminal_context);
  // One controller coordinates feedback and authority for the session; copying is forbidden.
  SelectorInputController(SelectorInputController const&) = delete;
  // Borrowed collaborator identities stay fixed throughout input dispatch.
  SelectorInputController& operator=(SelectorInputController const&) = delete;

  // TUI thread: require an open view and classify hit-testing/bindings without changing it.
  // Settings pointer hits highlight first; scoped-model bindings precede generic list keys.
  [[nodiscard]] SelectListInputResult classify(runtime_input::RuntimeInput const& input) const;
  // Composition applies draft/committed-session outcomes and the final render.
  // ReconcileSessionError must reconcile identity before publishing its text.
  [[nodiscard]] SelectorInputOutcome handle(SelectListInputResult const& input);

  // Borrowed owners print as type/address; their content is never traversed.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI thread: keep scoped edits in the current list; true means this action was claimed.
  bool handle_scoped_models(SelectListInputResult const& input);
  // TUI thread: publish fresh rows while preserving selected identity/query, or keep
  // the old view and beep on failure. The caller supplies already-completed authority work.
  void apply_scoped_update(core::Result<SelectListView> updated, SelectListInputResult const& input, std::string status);

  // Borrowed from Session; authority boundary for model/session callbacks and current bindings.
  TuiFrontEnd& frontend_;
  // Borrowed from Session; TUI projection; this owner publishes status and refreshes committed facts.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; selector owner; row borrows cannot survive a host mutation.
  SelectorHost& selectors_;
  // Borrowed from Session; renderer used to paint pending status before blocking authority callbacks.
  RuntimeRenderer& renderer_;
  // Terminal outlives the controller; only the TUI thread emits rejection bells here.
  terminal::Context& terminal_context_;
};

} // namespace ava::tui
