#include "sys.h"
#include "TuiFrontEnd.h"
#include "ava/app/runtime/Session.h"

namespace ava::app {

std::string TuiFrontEnd::read_committed_session_id()
{
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
  return session_r->store.options_r()->session_id;
}

} // namespace ava::app
