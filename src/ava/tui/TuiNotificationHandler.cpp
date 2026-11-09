#include "sys.h"
#include "TuiNotificationHandler.h"
#include "TuiSession.h"

namespace ava::tui {

void TuiNotificationHandler::notified()
{
  session_->notified();
}

} // namespace ava::tui
