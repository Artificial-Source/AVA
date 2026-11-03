// Routes idle submissions to local TUI features or the ActiveRun worker owner.
#pragma once

#include "ava/debug/print_members_on.h"

#include <optional>
#include <string>

namespace ava::tui {

class SelectorHost;
class SettingsDialog;
class SessionSelector;
class RuntimeActionController;
class ActiveRun;
class Composer;
class RuntimeNavigationController;
class RuntimePresentationState;
class RuntimePromptStashController;
class RuntimeRenderer;
class RuntimeSubagentWorkspaceController;
class TranscriptSearchController;
class TuiFrontEnd;

// TUI loop decision returned after synchronous submission routing.
enum class RuntimeSubmitDisposition
{
  NoAction, // Empty input produced no submission or local UI action.
  ContinueLoop, // Completion, local action or worker launch consumed this input.
  BreakLoop // The caller must leave dispatch and perform session cleanup.
};

// TUI-call-owned control result consumed by TuiSession. Flags preserve why dispatch
// must end; this value owns no worker or borrowed UI resources.
struct RuntimeSubmitOutcome
{
  // Router chooses whether the input was empty, consumed or ended terminal service.
  RuntimeSubmitDisposition disposition = RuntimeSubmitDisposition::NoAction;
  // Router propagates a failed rendering/action result for session exit status.
  bool terminal_write_failed = false;
  // Reserved outcome evidence for actual signal observation; ordinary quit is distinct.
  bool terminal_signal_received = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Session-owned TUI-thread router for idle Submit. It resolves completion before
// consuming a draft, handles local UI commands, then delegates worker lifetime to
// ActiveRun. Every borrowed collaborator outlives it; no member crosses threads.
class RuntimeSubmitController final
{
 public:
  // TUI thread: borrow session owners after they are constructed; this router never
  // retains the call-scoped submission or assumes ownership of their resources.
  RuntimeSubmitController(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state, RuntimeRenderer& renderer,
                          RuntimeNavigationController& navigation, RuntimeActionController& action_controller, ActiveRun& active_run_controller,
                          RuntimePromptStashController& prompt_stash, TranscriptSearchController& transcript_search,
                          RuntimeSubagentWorkspaceController& subagent_workspace, SelectorHost& selectors, SettingsDialog& settings_dialog,
                          SessionSelector& session_selector);
  // Keep one routing boundary responsible for draft consumption and history recording.
  RuntimeSubmitController(RuntimeSubmitController const&) = delete;
  // Session collaborator references must not be replaced during dispatch.
  RuntimeSubmitController& operator=(RuntimeSubmitController const&) = delete;

  // TUI thread while idle: accept completion first, then consume owned draft text.
  // An optional forced command bypasses slash selection but other visible completion
  // gates still run. Record history before local handling or ActiveRun launch.
  [[nodiscard]] RuntimeSubmitOutcome submit(std::optional<std::string> forced_submission = std::nullopt);

  // Generated printing uses addresses for borrowed owners; it never traverses their content.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Session borrow for local settings, attachment and backend action capabilities.
  TuiFrontEnd& frontend_;
  // Session projection receives status, local output and settings only on the TUI thread.
  RuntimePresentationState& presentation_state_;
  // Session editor owns completion state and history; submit consumes its draft once.
  Composer& draft_state_;
  // Session renderer supplies completion/layout context and reports frame failures.
  RuntimeRenderer& renderer_;
  // Session navigation validates file/path selections and locates transcript details.
  RuntimeNavigationController& navigation_;
  // Session action boundary owns local selector, attachment and display operations.
  RuntimeActionController& action_controller_;
  // Session worker owner receives submissions only after local routing declines them.
  ActiveRun& active_run_controller_;
  // Session stash owner handles local stash commands without creating a worker.
  RuntimePromptStashController& prompt_stash_;
  // Session search owner opens a query view for local /search handling.
  TranscriptSearchController& transcript_search_;
  // Session view owner opens jobs while backend job ownership stays elsewhere.
  RuntimeSubagentWorkspaceController& subagent_workspace_;
  // Session selector host publishes local keybinding lists with one active-list owner.
  SelectorHost& selectors_;
  // Session settings owner retains preview/commit lifetime when its dialog opens.
  SettingsDialog& settings_dialog_;
  // Session selection owner handles history navigation before backend submission.
  SessionSelector& session_selector_;
};

}  // namespace ava::tui
