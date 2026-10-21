#pragma once

#include "ava/frontend/AgentSelection.h"
#include "ava/frontend/Directories.h"
#include "ava/frontend/FrontEnd.h"
#include "ava/frontend/GitBranch.h"
#include "ava/frontend/ModelSelection.h"
#include "ava/frontend/PromptModeAndContextSources.h"
#include "ava/frontend/SessionIdentity.h"
#include "ava/frontend/SessionStats.h"

// For OLD API
#include "ava/tui/runtime.h"    // TuiRuntimeOptions

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public frontend::FrontEnd
{
 private:
  frontend::AgentSelection agent_selection_;
  frontend::ModelSelection model_selection_;
  frontend::PromptModeAndContextSources prompt_metadata_;
  frontend::SessionStats session_stats_;
  frontend::Directories directories_;
  frontend::GitBranch git_branch_;
  frontend::SessionIdentity session_identity_;

 public:
  // Explicitly connect session percentages to this frontend's model projection.
  TuiFrontEnd();

  // Implement virtual function of FrontEnd.
  void initialize_frontend_state(app::runtime::session_ts::crat const& session_r) override;

  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run(TuiRuntimeOptions options);

  // Schedule a refresh on the frontend of the registered Data object identified by `index`.
  // Called from Data::notify() on the notifying backend thread when that object's data was changed.
  void data_changed(frontend::DataListIndex index) override;

  // Accessors.

  frontend::AgentSelection& agent_selection() { return agent_selection_; }
  frontend::ModelSelection& model_selection() { return model_selection_; }
  frontend::ModelSelection const& model_selection() const { return model_selection_; }
  frontend::PromptModeAndContextSources& prompt_metadata() { return prompt_metadata_; }
  frontend::SessionStats& session_stats() { return session_stats_; }
  frontend::SessionStats const& session_stats() const { return session_stats_; }
  frontend::Directories& directories() { return directories_; }
  frontend::GitBranch& git_branch() { return git_branch_; }
  frontend::SessionIdentity& session_identity() { return session_identity_; }

  // OLD API
  [[nodiscard]] int run_interactive_composer(TuiRuntimeOptions options);

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::tui
