#include "sys.h"
#include "TuiSession.h"
#include "TuiTerminalInputHandler.h"

namespace ava::tui {

void TuiTerminalInputHandler::input_ready(int UNUSED_ARG(fd), utils::threading::EventLoop::ReadyEvents ready_events)
{
  session_->terminal_input_ready(ready_events);
}

} // namespace ava::tui
