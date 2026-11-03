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

enum class SelectorInputRequest
{
  Pass,
  Handled,
  RenderFailed,
  OpenReasoning
};

enum class SelectorInputTextKind
{
  DraftCommand,
  OpenedSession,
  ReconcileSessionError
};

using SelectorInputOutcome = std::variant<SelectorInputRequest, std::pair<SelectorInputTextKind, std::string>>;

// Catalog selector behavior shared by the idle input routes. Settings, session
// browsing and branch-summary lifetimes remain in their dedicated owners.
class SelectorInputController final
{
 public:
  // All borrowed collaborators outlive this TUI-thread-owned controller.
  SelectorInputController(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                          terminal::Context& terminal_context);
  SelectorInputController(SelectorInputController const&) = delete;
  SelectorInputController& operator=(SelectorInputController const&) = delete;

  [[nodiscard]] SelectListInputResult classify(runtime_input::RuntimeInput const& input) const;
  // Composition applies draft/committed-session outcomes and the final render.
  // ReconcileSessionError must reconcile identity before publishing its text.
  [[nodiscard]] SelectorInputOutcome handle(SelectListInputResult const& input);

  // Borrows full catalog/presentation/renderer aggregates containing arbitrary
  // transcript and stashed-prompt text; generated printing must not expose them.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  bool handle_scoped_models(SelectListInputResult const& input);
  void apply_scoped_update(core::Result<SelectListView> updated, SelectListInputResult const& input, std::string status);

  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  SelectorHost& selectors_;
  RuntimeRenderer& renderer_;
  terminal::Context& terminal_context_;
};

} // namespace ava::tui
