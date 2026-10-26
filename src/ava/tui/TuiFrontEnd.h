#pragma once

#include "ava/frontend/FrontEnd.h"
// For OLD API
#include "ava/tui/runtime.h"    // TuiRuntimeOptions

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public frontend::FrontEnd
{
 private:
  TuiRuntimeOptions options_;

 public:
  TuiFrontEnd(TuiRuntimeOptions&& options) : options_(std::move(options)) { }

  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run();

  // OLD API
  [[nodiscard]] int run_interactive_composer();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::tui
