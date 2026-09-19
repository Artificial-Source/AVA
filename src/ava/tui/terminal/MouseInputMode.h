#pragma once

#include "ava/debug/print_members_on.h"

namespace ava::tui::terminal {

class Context;

// Own mouse reporting and bracketed-paste modes for one terminal Context.
//
// Construction is inert. Call start once after ncurses initializes the screen. Unsupported terminal control sequences are harmlessly
// ignored, so startup performs no capability probing. This object and its Context must have nested lifetimes.
class MouseInputMode final
{
 private:
  Context* context_ = nullptr;    // Non-owning active Context, or null while inactive.

 public:
  // Create an inactive mode manager without changing ncurses or writing terminal control sequences.
  MouseInputMode() = default;

  // Keep terminal-mode restoration bound to one stable object and Context lifetime; copying and moving are unsupported.
  MouseInputMode(MouseInputMode const&) = delete;
  MouseInputMode& operator=(MouseInputMode const&) = delete;
  MouseInputMode(MouseInputMode&&) = delete;
  MouseInputMode& operator=(MouseInputMode&&) = delete;

  // Best-effort disable mouse reporting and bracketed paste; safe to call repeatedly and performs no work while inactive.
  ~MouseInputMode() noexcept;

  // Enable ncurses mouse decoding, portable xterm mouse reporting, and bracketed paste on `context`.
  //
  // A second call before stop is a programmer contract violation. No terminal reply is requested or consumed.
  void start(Context& context);

  // Disable bracketed paste and mouse reporting, then clear ncurses mouse decoding state.
  //
  // The operation is noexcept and idempotent. State is cleared even when terminal output fails, so a second call emits nothing.
  void stop() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
