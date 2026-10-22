#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/project_trust.h"

namespace ava::app::runtime {

// Trust state.
//
// The project trust decision loaded for this session. Mutated when the user
// adjusts trust (/trust commands); otherwise stable for the session lifetime.
struct TrustState
{
  ProjectTrustState project_trust;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::app::runtime
