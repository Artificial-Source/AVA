#pragma once

#include "ava/frontend/FrontEnd.h"
// For OLD API
#include "ava/tui/runtime.h"    // TuiRuntimeOptions, TuiCatalogDelivery

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public frontend::FrontEnd
{
 private:
  TuiRuntimeOptions options_;

 public:
  TuiFrontEnd(TuiRuntimeOptions&& options) : options_(std::move(options)) { }

  // Get the currently active session ID.
  virtual std::string read_committed_session_id() const = 0;

  // Pending-review catalog and todo transports. Pull and apply only at
  // serialized action/completion boundaries, never during rendering, and
  // like read_committed_session_id never while holding the UI lock.
  virtual TuiCatalogDelivery read_catalog_delivery() = 0;

  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run();

  // OLD API
  [[nodiscard]] int run_interactive_composer();

  // Access the runtime callbacks and presentation defaults owned by this frontend.
  TuiRuntimeOptions& options() { return options_; }
  TuiRuntimeOptions const& options() const { return options_; }

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(frontend::FrontEnd)
};

} // namespace ava::tui
