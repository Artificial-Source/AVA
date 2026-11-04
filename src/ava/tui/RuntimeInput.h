// Owns one decoded terminal event for startup replay or TUI input dispatch.
// See docs/engineering/tui-architecture.md for input batching and prompt quarantine.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/terminal.h"
#include "ava/core/NoLogString.h"

namespace ava::tui::runtime_input {

// Value owned by the startup queue, a polling result or the current TUI turn and
// borrowed by feature handlers only during dispatch. Built and consumed on the
// TUI thread; its strings own their storage, so no terminal buffer must outlive it.
// Resize and bracketed-paste flags preserve events that lack an ordinary key.
struct RuntimeInput
{
  // Decoder-provided key/mouse metadata used by modal dispatch on the TUI thread;
  // Character and Space include text for handlers that consume only InputEvent.
  InputEvent event;
  // Owned UTF-8 insertion payload set by the decoder and read by the composer on
  // the TUI thread; NoLogString keeps user input out of generated debug output.
  ava::core::NoLogString text;
  // Decoder sets this for one assembled paste; text handlers insert it as one edit,
  // and shortcut handlers avoid treating its first character as a jump command.
  bool bracketed_paste = false;
  // Decoder marks terminal resize even with an Unknown key; TuiSession must
  // retain it as actionable work rather than discard it as an unsupported key.
  bool resize = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::runtime_input
