#include "sys.h"
#include "DebugInit.h"
#ifdef DEBUGGLOBAL
#include "utils/GlobalObjectManager.h"
#endif

#include <cstdlib>
#include <cstring>
#include <iostream>
#include "debug.h"

namespace ava::core {

// Initialize libcwd for the process only when debug output was explicitly
// requested. AVA_DEBUG_OUTPUT=1 opts an ordinary process into developer
// diagnostics; libcwd output otherwise stays off by default, even in CWDEBUG
// builds. AVA_NO_DEBUG_OUTPUT is a hard suppression control that wins when
// both are set. The test executable may override suppression only after it
// has installed a validated private output stream; initialization diagnostics
// then cannot leak to its stdout or stderr protocol. When initialization is
// skipped, the usual GlobalObjectManager and iostream preamble still runs.
//
// This does not silence LIBCWD_ASSERT / the dc::core ("COREDUMP") channel,
// which libcwd configures in static initializers.
DebugInit::DebugInit(bool always_initialize)
{
  if (!always_initialize)
  {
    char const* debug_output = std::getenv("AVA_DEBUG_OUTPUT");
    bool const opted_in = debug_output != nullptr && std::strcmp(debug_output, "1") == 0;
    if (!opted_in || std::getenv("AVA_NO_DEBUG_OUTPUT") != nullptr)
    {
      // Run the GlobalObjectManager and iostream preamble that must execute on every
      // debug_init path, including the paths that leave libcwd output off.
#ifdef DEBUGGLOBAL
      if (!Singleton<GlobalObjectManager>::instantiate().is_after_global_constructors())
        GlobalObjectManager::main_entered();
#endif

#ifdef NO_SYNC_WITH_STDIO_FALSE
#warning "NO_SYNC_WITH_STDIO_FALSE is now the default."
#endif
#ifdef SYNC_WITH_STDIO_FALSE
      std::ios::sync_with_stdio(false);
#endif

      // Leave libcw_do in its default off state.
      return;
    }

    // Fall-through to normal initialization of libcwd.
  }

  Debug(::NAMESPACE_DEBUG::init());
  Debug(libcw_do.always_flush_on());
  Dout(dc::notice, "Debug output turned on.");
}

} // namespace ava::core
