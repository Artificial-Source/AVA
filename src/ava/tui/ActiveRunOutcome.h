#pragma once

#include "ava/debug/print_members_on.h"

namespace ava::tui {
class ActiveRunOutcome final
{
 public:
  ActiveRunOutcome(bool break_loop, bool terminal_write_failed, bool terminal_signal_received)
      : break_loop_(break_loop), terminal_write_failed_(terminal_write_failed), terminal_signal_received_(terminal_signal_received)
  {
  }
  [[nodiscard]] bool break_loop() const { return break_loop_; }
  [[nodiscard]] bool terminal_write_failed() const { return terminal_write_failed_; }
  [[nodiscard]] bool terminal_signal_received() const { return terminal_signal_received_; }
  AVA_DEBUG_PRINT_MEMBERS_ON
 private:
  bool break_loop_;
  bool terminal_write_failed_;
  bool terminal_signal_received_;
};
} // namespace ava::tui
