#pragma once

#include "ava/app/frontend/UnlockedData.h"
#include "ava/session/Stats.h"

namespace ava::app::runtime {

// Canonical session statistics, including cumulative provider usage and the paired active-context estimates.
using SessionStats_ts = UnlockedData<session::Stats>;

} // namespace ava::app::runtime
