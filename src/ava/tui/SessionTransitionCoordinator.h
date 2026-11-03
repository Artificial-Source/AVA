#pragma once

#include "ava/debug/print_members_on.h"

#include <string>

namespace ava::tui {

class RuntimePresentationState;
class Composer;
class RuntimeRenderer;
class TranscriptSearchController;
class RuntimeSubagentWorkspaceController;
class SelectorHost;
class TuiFrontEnd;

// Composition-level transition ordering shared by idle session selection and
// active-run completion. All borrowed owners outlive this coordinator; none of
// them gains backend session authority through this collaboration.
class SessionTransitionCoordinator final
{
 public:
  SessionTransitionCoordinator(RuntimePresentationState& presentation, Composer& draft, RuntimeRenderer& renderer, TranscriptSearchController& search,
                               RuntimeSubagentWorkspaceController& workspace, SelectorHost& selectors);
  SessionTransitionCoordinator(SessionTransitionCoordinator const&) = delete;
  SessionTransitionCoordinator& operator=(SessionTransitionCoordinator const&) = delete;

  // Read committed identity through the frontend before acquiring the UI lock.
  // A matching identity preserves all session-scoped presentation state.
  [[nodiscard]] bool apply(std::string committed_session_id);
  // Calls frontend boundary reads: invoke only without holding the UI lock.
  [[nodiscard]] bool apply_and_refresh(TuiFrontEnd& frontend, std::string committed_session_id);

  // Borrowed presentation/draft owners contain arbitrary messages, attachments
  // and permission aggregates; generated printing must not expose their contents.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  RuntimePresentationState& presentation_;
  Composer& draft_;
  RuntimeRenderer& renderer_;
  TranscriptSearchController& search_;
  RuntimeSubagentWorkspaceController& workspace_;
  SelectorHost& selectors_;
};

}  // namespace ava::tui
