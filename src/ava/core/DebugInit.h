#pragma once
#include "debug.h"

#ifdef CWDEBUG
namespace ava::core {

struct DebugInit
{
  // Initialize libcwd only when debug output was explicitly requested:
  // AVA_DEBUG_OUTPUT=1 opts in, AVA_NO_DEBUG_OUTPUT always suppresses, and tests
  // pass true only after installing a validated private output stream.
  DebugInit(bool always_initialize);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::core
#endif
