// Owns selector results until TuiSession performs the requested cross-feature work.
// See docs/engineering/tui-architecture.md for selector closure and session reconciliation.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <ostream>
#include <variant>

namespace ava::tui {

// TUI-thread requests that need no payload. SelectorHost returns them after
// resolving local selector state; Session performs them before requesting paint.
enum class SelectorDispatchRequest
{
  None, // The owning selector completed the action.
  Models, // Open model selection after Settings has retired.
  ScopedModels, // Open scoped model selection after Settings has retired.
  Reasoning, // Open reasoning choices from Settings.
  CurrentReasoning, // Open reasoning choices for the just-selected model.
  Hotkeys, // Replace Settings with the keybinding selector.
  RenderFailed, // Painting failed before a synchronous frontend operation.
  Beep, // The feature already published the reason; Session emits the bell.
};

// Owned TUI-thread handoff from a retired selector to Composer. Session applies
// completion resets, then feedback; neither string borrows the old selector.
struct SelectorDraftCompletion
{
  // Text to install with completion acceptance semantics, not command reset semantics.
  ava::core::NoLogString command;
  // Feature-selected feedback published after installing the text.
  ava::core::NoLogString status;
  // Generated diagnostics omit both content strings.
  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned TUI-thread command request after session-list rename/label selection.
// Session installs it through the draft controller, which resets command interaction.
struct SelectorDraftCommand
{
  // Copied command survives closing the source selector; Session consumes it once.
  ava::core::NoLogString command;
  // Generated diagnostics omit command contents.
  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned TUI-thread feedback after a backend session operation succeeded. Session
// reconciles the committed session identity before displaying this message.
struct SelectorOpenedSession
{
  // Status text from the completed action, not an identity used to select a session.
  ava::core::NoLogString feedback;
  // Generated diagnostics omit feedback that may contain paths or titles.
  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned TUI-thread error after a session action. The backend may have committed
// an identity before failing, so Session reconciles it before feedback and a bell.
struct SelectorSessionError
{
  // Formatted error copied before the source result dies; Session displays it once.
  ava::core::NoLogString feedback;
  // Generated diagnostics omit provider/path content in the error.
  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned TUI-thread handoff to BranchSummaryOperation. The mandatory saved view
// restores the selector after progress replaces it; no row borrow escapes dispatch.
struct SelectorParentSummary
{
  // Validated source identity copied while its source row still exists.
  ava::core::NoLogString selected_source;
  // Complete prior view moved into the summary owner before it publishes progress.
  SelectListView previous_view;
  // Source row index paired with the saved view; dispatch validates the chosen row.
  std::size_t selected_index;
  // TUI-thread diagnostics reveal only row position, never source identity or view text.
  void print_on(std::ostream& output) const { output << "{selected_index:" << selected_index << '}'; }
  // The handwritten printer avoids recursively traversing the sensitive saved view.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// A result owns exactly the fields for its alternative. SelectorHost constructs it
// on the TUI thread; Session consumes it after local preview/view retirement and
// before painting. No selector or backend object must outlive these owned values.
// Payload records share this header because they are the alternatives of this one contract.
using SelectorDispatchResult =
    std::variant<SelectorDispatchRequest, SelectorDraftCompletion, SelectorDraftCommand, SelectorOpenedSession, SelectorSessionError, SelectorParentSummary>;

} // namespace ava::tui
