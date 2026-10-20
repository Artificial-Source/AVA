#pragma once

#include "ava/frontend/UnlockedData.h"
#include "ava/session/Stats.h"

namespace ava::app::runtime {

// Canonical session statistics, including cumulative provider usage and the paired active-context estimates.
using SessionStats_ts = frontend::UnlockedData<session::Stats>;

} // namespace ava::app::runtime
