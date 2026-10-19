#include "sys.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/runtime.h"

#include <stdexcept>
#include <utility>

namespace ava::tui {

TuiFrontEnd::TuiFrontEnd() : model_selection_(this), prompt_metadata_(this), session_stats_(this, &model_selection_), directories_(this), git_branch_(this)
{
}

void TuiFrontEnd::initialize_frontend_state(app::runtime::session_ts::crat const& session_r)
{
  model_selection_.initialize(&session_r->model_selection());
  prompt_metadata_.initialize(&session_r->prompt_mode_and_context_sources());
  session_stats_.initialize(&session_r->session_stats());
  directories_.initialize(&session_r->directories());
  git_branch_.initialize(&session_r->git_branch());
}

int TuiFrontEnd::run(TuiRuntimeOptions options)
{
  // FIXME: Delegate to the existing runtime until its UI state and orchestration have been moved into this frontend.
  return run_interactive_composer(std::move(options));
}

// Record possible canonical changes from a notifying backend thread for the next frontend refresh.
// This callback does not itself wake the event loop or access the terminal.
void TuiFrontEnd::data_changed(app::frontend::DataListIndex index)
{
  DoutEntering(dc::frontend, "TuiFrontEnd::data_changed(" << index << ")");
  app::frontend::Data const* data = get_data(index);

  if (dynamic_cast<app::runtime::Directories_ts const*>(data) != nullptr)
  {
    directories_.dirty();
  }
  else if (dynamic_cast<app::runtime::ModelSelection_ts const*>(data) != nullptr)
  {
    model_selection_.dirty();
  }
  else if (dynamic_cast<app::runtime::PromptModeAndContextSources_ts const*>(data) != nullptr)
  {
    prompt_metadata_.dirty();
  }
  else if (dynamic_cast<app::runtime::SessionStats_ts const*>(data) != nullptr)
  {
    session_stats_.dirty();
  }
  else if (dynamic_cast<app::runtime::GitBranch_ts const*>(data) != nullptr)
  {
    git_branch_.dirty();
  }
}

} // namespace ava::tui
