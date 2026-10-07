#pragma once

#include "ava/app/frontend/FrontEnd.h"

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public app::frontend::FrontEnd
{
 public:
  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run(TuiRuntimeOptions options);

  // Schedule a refresh on the frontend of the registered Data object identified by `index`.
  // Called from Data::notify() on the notifying backend thread when that object's data was changed.
  void data_changed(app::frontend::DataListIndex index) override;

  // No members yet.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::tui
