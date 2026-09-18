#pragma once

#include "ava/debug/print_members_on.h"

#include <string>

namespace ava::tui::terminal {

class Context;

// Negotiates enhanced keyboard reporting for one terminal Context and restores every mode it may have requested.
//
// Construction is inert. Call start once before normal ncurses input processing, then replay take_buffered_input() through the future
// input loop so bytes unrelated to negotiation are not lost. This object and its Context must have nested lifetimes.
class KeyboardInputMode final
{
 private:
  Context* context_ = nullptr;             // Non-owning active Context, or null while inactive.
  bool kitty_push_requested_ = false;      // Whether a Kitty push write may have activated a stack entry.
  bool modify_other_keys_requested_ = false; // Whether a modifyOtherKeys write may have activated level 2; this is not confirmation.
  std::string buffered_input_;             // Non-protocol bytes consumed from the raw descriptor during negotiation.

 public:
  // Create an inactive mode manager without reading from or writing to a terminal.
  KeyboardInputMode() = default;

  // Keep terminal-mode restoration bound to one stable object and Context lifetime; copying and moving are unsupported.
  KeyboardInputMode(KeyboardInputMode const&) = delete;
  KeyboardInputMode& operator=(KeyboardInputMode const&) = delete;
  KeyboardInputMode(KeyboardInputMode&&) = delete;
  KeyboardInputMode& operator=(KeyboardInputMode&&) = delete;

  // Best-effort restore any mode that start may have activated; safe to call repeatedly and performs no work while inactive.
  ~KeyboardInputMode() noexcept;

  // Start one bounded negotiation on `context`, preferring Kitty disambiguation flag 1 and otherwise requesting modifyOtherKeys level 2.
  //
  // The call performs terminal I/O and waits up to a short fixed deadline. Complete Kitty and device-attributes replies are consumed;
  // all other bytes are retained for take_buffered_input(). A second call before stop is a programmer contract violation.
  void start(Context& context);

  // Disable requested modifyOtherKeys state, then pop a possibly successful Kitty push, using best-effort writes.
  //
  // The operation is noexcept and idempotent. State is cleared even when terminal output fails, so a second call emits nothing.
  void stop() noexcept;

  // Move out and clear bytes read during negotiation that were not complete expected protocol replies.
  //
  // Ordinary text, unrelated escape sequences, malformed replies, and incomplete sequences at the deadline are preserved byte-for-byte
  // so a future input loop can replay them. Calling this again without another negotiation returns an empty string.
  std::string take_buffered_input();

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
