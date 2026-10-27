#include "sys.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/runtime.h"
#include "ava/app/runtime/Session.h"

#include <utility>

namespace ava::tui {

int TuiFrontEnd::run()
{
  // FIXME: Delegate to the existing runtime until its UI state and orchestration have been moved into this frontend.
  return run_interactive_composer();
}

std::string TuiFrontEnd::read_committed_session_id()
{
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
  return session_r->store.options_r()->session_id;
}

} // namespace ava::tui
