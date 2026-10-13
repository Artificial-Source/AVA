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
void TuiFrontEnd::data_changed(app::frontend::DataListIndex index)
{
  DoutEntering(dc::notice, "TuiFrontEnd::data_changed(" << index << ")");
  app::frontend::Data const* data = get_data(index);

  if (dynamic_cast<app::runtime::ModelSelection_ts const*>(data) != nullptr)
  {
    model_selection_.dirty();
  }
}

} // namespace ava::tui
