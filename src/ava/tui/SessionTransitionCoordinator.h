// Orders presentation cleanup after the backend commits a different session.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "ava/debug/print_members_on.h"

#include <string>

namespace ava::tui {

class RuntimePresentationState;
class Composer;
class PendingAttachments;
class TuiSessionGrantRegistry;
class RuntimeRenderer;
class TranscriptSearchController;
class RuntimeSubagentWorkspaceController;
class SelectorHost;
class TuiFrontEnd;

// Session owns transition ordering shared by idle selection and active-run completion.
// Every member is TUI-thread-only and borrows owners that outlive this coordinator.
// The backend must already have committed the session change; equal identities preserve
// local state. Cleanup finishes before display state gets the new identity. No worker
// or callback retains this coordinator, so destruction needs no join or lock.
class SessionTransitionCoordinator final
{
 public:
  // TUI thread: borrow every session-scoped owner before any transition. All references
  // remain valid through the coordinator lifetime and each synchronous cleanup.
  SessionTransitionCoordinator(RuntimePresentationState& presentation, Composer& draft, RuntimeRenderer& renderer, TranscriptSearchController& search,
                               RuntimeSubagentWorkspaceController& workspace, SelectorHost& selectors, PendingAttachments& attachments,
                               TuiSessionGrantRegistry& grants);
  // Not copyable: one coordinator orders cleanup across the same session owners.
  SessionTransitionCoordinator(SessionTransitionCoordinator const&) = delete;
  SessionTransitionCoordinator& operator=(SessionTransitionCoordinator const&) = delete;

  // TUI thread: receive the backend's committed identity, then clean old presentation
  // before storing a different identity. Return true after cleanup; false preserves local state.
  [[nodiscard]] bool apply(std::string committed_session_id);
  // TUI thread: apply committed identity cleanup, then refresh frontend facts even
  // when identity matched. Return whether session-specific cleanup was necessary.
  [[nodiscard]] bool apply_and_refresh(TuiFrontEnd& frontend, std::string committed_session_id);

  // Generated reference printing reports type/address without traversing owners.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Borrowed from Session; display state; compare the old identity here and store the new one last.
  RuntimePresentationState& presentation_;
  // Borrowed from Session; draft owner; transition discards old draft, history and interaction state.
  Composer& draft_;
  // Borrowed from Session; rendering owner; transition retires viewport and selection from the old session.
  RuntimeRenderer& renderer_;
  // Borrowed from Session; search owner; reset matches before replacing transcript identity.
  TranscriptSearchController& search_;
  // Borrowed from Session; workspace owner; close any inspector tied to the previous session.
  RuntimeSubagentWorkspaceController& workspace_;
  // Borrowed from Session; selector owner; retire old rows after display cleanup.
  SelectorHost& selectors_;
  // Borrowed from Session; staged references; clear them before any new-session submission is possible.
  PendingAttachments& attachments_;
  // Borrowed from Session; frontend grants; revoke old-session handles before replacing visible identity.
  TuiSessionGrantRegistry& grants_;
};

}  // namespace ava::tui
