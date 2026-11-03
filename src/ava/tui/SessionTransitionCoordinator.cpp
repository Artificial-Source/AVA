#include "sys.h"
#include "Composer.h"
#include "SelectorHost.h"
#include "SessionTransitionCoordinator.h"
#include "runtime_render_internal.h"
#include "runtime_state_internal.h"
#include "runtime_subagent_workspace_internal.h"
#include "runtime_transcript_search_internal.h"

#include <utility>

namespace ava::tui {

SessionTransitionCoordinator::SessionTransitionCoordinator(RuntimePresentationState& presentation, Composer& draft, RuntimeRenderer& renderer,
                                                           TranscriptSearchController& search, RuntimeSubagentWorkspaceController& workspace,
                                                           SelectorHost& selectors)
    : presentation_(presentation), draft_(draft), renderer_(renderer), search_(search), workspace_(workspace), selectors_(selectors)
{
}

bool SessionTransitionCoordinator::apply(std::string committed_session_id)
{
  if (!presentation_.prepare_session_transition(committed_session_id))
    return false;

  search_.reset_for_session_transition();
  workspace_.reset_for_session_transition();
  draft_.reset_for_session_transition();
  renderer_.reset_for_session_transition();
  presentation_.reset_session_projection(selectors_.kind() == ActiveSelectList::Overview);
  selectors_.close();
  presentation_.finish_session_transition(std::move(committed_session_id));
  return true;
}

bool SessionTransitionCoordinator::apply_and_refresh(TuiFrontEnd& frontend, std::string committed_session_id)
{
  auto const changed = apply(std::move(committed_session_id));
  presentation_.refresh_after_backend_action(frontend);
  return changed;
}

}  // namespace ava::tui
