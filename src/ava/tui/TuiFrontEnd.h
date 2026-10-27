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

  // Get the currently active session ID.
  virtual std::string read_committed_session_id() = 0;

  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run();

  // OLD API
  [[nodiscard]] int run_interactive_composer();

  // Accessor.

  TuiRuntimeOptions const& options() const { return options_; }

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(frontend::FrontEnd)
};

} // namespace ava::tui
