#pragma once

#include "ava/app/frontend/FrontEnd.h"
#include "ava/app/frontend/ModelSelection.h"
#include "ava/app/frontend/SessionStats.h"

// For OLD API
#include "ava/tui/runtime.h"    // TuiRuntimeOptions

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public app::frontend::FrontEnd
{
 private:
  app::frontend::ModelSelection model_selection_;
  app::frontend::SessionStats session_stats_;

 public:
  TuiFrontEnd() : model_selection_(this), session_stats_(this) { }

  // Implement virtual function of FrontEnd.
  void initialize_frontend_state(app::runtime::session_ts::crat const& session_r) override
  {
    // Initialize our frontend state objects.
    model_selection_.initialize(&session_r->model_selection());
    session_stats_.initialize(&session_r->session_stats(), &session_r->system_prompt_token_estimate());
  }

  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run(TuiRuntimeOptions options);

  // Schedule a refresh on the frontend of the registered Data object identified by `index`.
  // Called from Data::notify() on the notifying backend thread when that object's data was changed.
  void data_changed(app::frontend::DataListIndex index) override;

  // Accessors.

  app::frontend::ModelSelection& model_selection() { return model_selection_; }
  app::frontend::ModelSelection const& model_selection() const { return model_selection_; }
  app::frontend::SessionStats& session_stats() { return session_stats_; }
  app::frontend::SessionStats const& session_stats() const { return session_stats_; }

  // OLD API
  [[nodiscard]] int run_interactive_composer(TuiRuntimeOptions options);

  // No members yet.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::tui
