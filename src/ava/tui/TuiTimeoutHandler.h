#pragma once

#include "TuiTimerRegistry.h"
#include "ava/debug/print_members_on.h"

#include <optional>

namespace ava::tui {

class TuiSession;

// TuiTimeoutHandler
//
// Receives the timeout events of the TUI thread's event loop and knows when the next one is due.
//
// It tells the caller when it wants to be called back, without knowing how the caller waits for that moment.
//
// For now this is a shell around the TuiSession that still does all the work.
class TuiTimeoutHandler
{
 private:
  //FIXME: this should not be here.
  TuiSession* session_; // The object that still does the actual work.

 public:
  //FIXME: setting a TuiSession* is a hack. This function should be removed.
  void set_tui_session(TuiSession* session) { session_ = session; }

  //===========================================================================
  // The member functions below may only be called by the tui-thread.
  // Therefore they must be non-const: otherwise they become publically accessible through TuiFrontEnd.

  // Return when expire must be called next, or nothing when no timer is armed.
  //
  // A deadline in the past means that the caller should not sleep at all.
  [[nodiscard]] std::optional<TuiTimerRegistry::Clock::time_point> next_deadline() noexcept;

  // Run the callbacks of all timers that are due at now.
  void expire(TuiTimerRegistry::Clock::time_point now);

  // End of tui-thread only functions.
  //===========================================================================

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui
