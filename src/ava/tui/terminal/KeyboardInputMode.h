#pragma once

#include "utils/macros.h"
#include "ava/debug/print_members_on.h"

#include <string>

namespace ava::tui::terminal {

class Context;

// Negotiates enhanced keyboard reporting for one terminal Context and restores every mode it may have requested.
//
// Construction is inert. Call start once before normal ncurses input processing, then replay try_get_wch() through the
// future input loop so bytes unrelated to negotiation are not lost. This object and its Context must have nested lifetimes.
class KeyboardInputMode final
{
 private:
  Context* context_ = nullptr;                  // Non-owning active Context, or null while inactive.
  bool kitty_push_requested_ = false;           // Whether a Kitty push write may have activated a stack entry.
  bool modify_other_keys_requested_ = false;    // Whether a modifyOtherKeys write may have activated level 2; this is not confirmation.
  std::string buffered_input_;                  // Non-protocol bytes consumed from the raw descriptor during negotiation.
  std::string_view remaining_buffered_input_;   // A view of the remaining buffered input, initialized at the end of start.

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

  // Start bounded, device-attributes-fenced negotiations on `context`, preferring Kitty disambiguation flag 1 and otherwise requesting
  // and querying modifyOtherKeys level 2.
  //
  // Each phase performs terminal I/O and waits up to a short fixed deadline. Complete expected query and device-attributes replies are
  // consumed only through that phase's fence; all other bytes are retained for a later phase or try_get_wch. A second call before stop
  // is a programmer contract violation.
  void start(Context& context);

  // Disable requested modifyOtherKeys state, then pop a possibly successful Kitty push, using best-effort writes.
  //
  // The operation is noexcept and idempotent. State is cleared even when terminal output fails, so a second call emits nothing.
  void stop() noexcept;

  // Move out one byte read during negotiation that was not part of a complete expected protocol reply.
  // Ordinary text, unrelated escape sequences, malformed replies, and incomplete sequences at the deadline are
  // preserved byte-for-byte.
  bool try_get_wch(wint_t* wch)
  {
    if (AI_LIKELY(remaining_buffered_input_.empty()))
      return false;

    *wch = remaining_buffered_input_.front();
    remaining_buffered_input_.remove_prefix(1);
    return true;
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
