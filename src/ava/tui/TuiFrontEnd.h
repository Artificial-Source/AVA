#pragma once

#include "ava/frontend/FrontEnd.h"
// For OLD API
#include "ava/tui/runtime.h"    // TuiRuntimeOptions

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public frontend::FrontEnd
{
 private:
  app::runtime::session_ts& unlocked_session_;
  TuiRuntimeOptions options_;

 public:
  TuiFrontEnd(app::runtime::session_ts& unlocked_session, TuiRuntimeOptions&& options) :
    unlocked_session_(unlocked_session), options_(std::move(options)) { }

  // Reads the backend's currently committed session identity. The application
  // acquires and releases the backend Session lock inside this callback; the
  // TUI calls it only at serialized action/completion boundaries and never
  // while holding its UI lock.
  std::string read_committed_session_id();

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
