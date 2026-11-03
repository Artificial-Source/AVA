// Owns command-output input policy within the TUI.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "ava/debug/print_members_on.h"

namespace ava::tui {
class RuntimePresentationState;
struct InputEvent;

// A modal may consume a key without changing pixels, or close and let typing
// continue to Composer. Session performs any requested paint after dispatch.
enum class CommandOutputDisposition
{
  Pass, // No output modal, or typing has closed it and must reach the editor.
  Consumed, // Modal capture prevents a pointer/key from reaching underlying views.
  Redraw, // Dismissal or scrolling changed the published modal.
};

// Session owns this TUI-thread policy owner and calls it before editor dispatch.
// Presentation outlives it and retains the single output view. Modal publications
// finish before typing is forwarded; no mutable view escapes. There is no worker
// or lock, and destruction releases only the borrowed association.
class CommandOutput final
{
 public:
  // Borrow the projection until destruction; construction does not open a modal.
  explicit CommandOutput(RuntimePresentationState& presentation);
  // Called on the TUI thread after resize/wheel filtering and before editor keys.
  // Host prompts retain precedence. Pass may mean output was closed by typing.
  [[nodiscard]] CommandOutputDisposition handle_input(InputEvent const& event);

  // Generated printing uses addresses for borrowed owners; it never traverses their content.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Session-owned storage. Only named output/status publications change it here.
  RuntimePresentationState& presentation_;
};
} // namespace ava::tui
