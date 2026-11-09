#pragma once

#include "ava/debug/print_members_on.h"

namespace ava::tui {

class TuiSession;

// TuiNotificationHandler
//
// Receives the software notification events of the TUI thread's event loop.
//
// For now this is a shell around the TuiSession that still does all the work.
class TuiNotificationHandler
{
 private:
  //FIXME: this should not be here.
  TuiSession* session_; // The object that still does the actual work.

 public:
  //FIXME: setting a TuiSession* is a hack. This function should be removed.
  void set_tui_session(TuiSession* session) { session_ = session; }

  // Handle that a producer notified the wakeup token of the session.
  //
  // Does nothing for now: TuiSession::flush() still checks every producer mailbox.
  void notified();

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui
