#include "sys.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/runtime.h"

#include <utility>

namespace ava::tui {

int TuiFrontEnd::run()
{
  // FIXME: Delegate to the existing runtime until its UI state and orchestration have been moved into this frontend.
  return run_interactive_composer();
}

} // namespace ava::tui
