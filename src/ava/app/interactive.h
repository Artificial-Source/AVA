#pragma once

#include "ava/app/runtime/Session.h"
#include "ava/app/runtime/session_ts.h"

namespace ava::app {

[[nodiscard]] int run_interactive(runtime::session_ts& unlocked_session);

}  // namespace ava::app
