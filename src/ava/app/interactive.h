#pragma once

#include "ava/app/ExitCode.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime/session_ts.h"

namespace ava::app {

[[nodiscard]] ExitCode run_interactive(runtime::session_ts& unlocked_session);

}  // namespace ava::app
