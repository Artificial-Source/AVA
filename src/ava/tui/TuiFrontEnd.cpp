#include "sys.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/runtime.h"

#include <stdexcept>
#include <utility>

namespace ava::tui {

int TuiFrontEnd::run(TuiRuntimeOptions options)
{
  // FIXME: Delegate to the existing runtime until its UI state and orchestration have been moved into this frontend.
  return run_interactive_composer(std::move(options));
}

// Queue the notification and wake the frontend so it fetches the latest snapshot and updates the UI on the main thread.
void TuiFrontEnd::data_changed(app::frontend::DataListIndex)
{
  // FIXME: Data notification delivery is not implemented yet.
  // Reject notifications explicitly rather than silently losing a backend update.
  throw std::logic_error("TuiFrontEnd data notifications are not wired; do not register frontend Data objects yet");
}

} // namespace ava::tui
