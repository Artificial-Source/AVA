// Orders presentation cleanup after the backend commits a different session.
// See docs/engineering/tui-architecture.md for the owner relationships.
#include "sys.h"
#include "Composer.h"
#include "PendingAttachments.h"
#include "SelectorHost.h"
#include "SessionTransitionCoordinator.h"
#include "runtime_render_internal.h"
#include "runtime_state_internal.h"
#include "runtime_subagent_workspace_internal.h"
#include "runtime_transcript_search_internal.h"
#include "session_grants.h"

#include <utility>

namespace ava::tui {

SessionTransitionCoordinator::SessionTransitionCoordinator(RuntimePresentationState& presentation, Composer& draft, RuntimeRenderer& renderer,
                                                           TranscriptSearchController& search, RuntimeSubagentWorkspaceController& workspace,
                                                           SelectorHost& selectors, PendingAttachments& attachments, TuiSessionGrantRegistry& grants)
    : presentation_(presentation),
      draft_(draft),
      renderer_(renderer),
      search_(search),
      workspace_(workspace),
      selectors_(selectors),
      attachments_(attachments),
      grants_(grants)
{
}

bool SessionTransitionCoordinator::apply(std::string committed_session_id)
{
  if (presentation_.screen().session_id == committed_session_id)
    return false;

  // Revoke old-session handles and staged content before resetting views. Search and
  // workspace teardown happen while the old identity is still available to their owners.
  static_cast<void>(grants_.clear_for_session_transition(presentation_.screen().session_id, committed_session_id));
  attachments_.clear();
  search_.reset_for_session_transition();
  workspace_.close();
  draft_.reset_for_session_transition();
  renderer_.reset_for_session_transition();
  // Clear old display data and selector rows before storing the committed identity last.
  // A reader after apply returns therefore cannot mistake old content for the new session.
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
