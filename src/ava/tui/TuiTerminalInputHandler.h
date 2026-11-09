#pragma once

#include "ava/debug/print_members_on.h"
#include "utils/threading/EventLoop.h"

namespace ava::tui {

class TuiSession;

// TuiTerminalInputHandler
//
// Receives the terminal input events of the TUI thread's event loop.
//
// For now this is a shell around the TuiSession that still does all the work.
class TuiTerminalInputHandler
{
 private:
  //FIXME: this should not be here.
  TuiSession* session_; // The object that still does the actual work.

 public:
  //FIXME: setting a TuiSession* is a hack. This function should be removed.
  void set_tui_session(TuiSession* session) { session_ = session; }

  // Handle that the terminal input fd is ready, with ready being the reported conditions, which may include a hangup or error.
  //
  // Only records a hangup or error; the input itself is still read by TuiSession::flush().
  void input_ready(int fd, utils::threading::EventLoop::ReadyEvents ready);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui
