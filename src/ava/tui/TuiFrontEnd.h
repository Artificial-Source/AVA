#pragma once

#include "ava/frontend/FrontEnd.h"
// For OLD API
#include "ava/tui/runtime.h"    // TuiRuntimeOptions

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public frontend::FrontEnd
{
 public:
  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run(TuiRuntimeOptions options);

  // OLD API
  [[nodiscard]] int run_interactive_composer(TuiRuntimeOptions options);

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::tui
