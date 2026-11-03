// Carries submission settlement decisions back to the TUI session.
#pragma once

#include "ava/debug/print_members_on.h"

namespace ava::tui {
// A TUI-thread value returned after settlement or shutdown. Named operations
// preserve simultaneous signal and I/O failures without positional bool arguments.
class ActiveRunOutcome final
{
 public:
  // Successful settlement leaves the session available for another submission.
  [[nodiscard]] static ActiveRunOutcome continue_session() { return {}; }
  // Normal quit or queue failure asks the session to end without inventing a signal.
  [[nodiscard]] static ActiveRunOutcome close_session()
  {
    ActiveRunOutcome result;
    result.break_loop_ = true;
    return result;
  }
  // Add terminal failure while preserving any signal already recorded in this value.
  [[nodiscard]] ActiveRunOutcome with_terminal_write_failure() const
  {
    auto result = *this;
    result.break_loop_ = true;
    result.terminal_write_failed_ = true;
    return result;
  }
  // Add actual signal observation while preserving any terminal failure.
  [[nodiscard]] ActiveRunOutcome with_terminal_signal() const
  {
    auto result = *this;
    result.break_loop_ = true;
    result.terminal_signal_received_ = true;
    return result;
  }
  // The session reads this after the worker and its prompt callbacks are retired.
  [[nodiscard]] bool break_loop() const { return break_loop_; }
  // The session retains I/O failure when choosing its final exit status.
  [[nodiscard]] bool terminal_write_failed() const { return terminal_write_failed_; }
  // Distinguish actual signal termination from ordinary descriptor closure.
  [[nodiscard]] bool terminal_signal_received() const { return terminal_signal_received_; }
  // Only control flags are retained; no submitted or backend content is printable here.
  AVA_DEBUG_PRINT_MEMBERS_ON
 private:
  // Factories alone establish the initial decision; instances contain no borrowed resources.
  ActiveRunOutcome() = default;
  bool break_loop_ = false; // Factory/modifiers request session exit on the TUI thread.
  bool terminal_write_failed_ = false; // Failure modifier records terminal I/O failure.
  bool terminal_signal_received_ = false; // Signal modifier records actual signal observation.
};
} // namespace ava::tui
