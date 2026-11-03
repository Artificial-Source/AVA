#pragma once

#include <optional>
#include <string>
#include "debug.h"

namespace ava::tui {

class SelectorHost;
class SettingsDialog;
class SessionSelector;
class RuntimeActionController;
class RuntimeActiveRunController;
class Composer;
class RuntimeNavigationController;
class RuntimePresentationState;
class RuntimePromptStashController;
class RuntimeRenderer;
class RuntimeSubagentWorkspaceController;
class TranscriptSearchController;
class TuiFrontEnd;

enum class RuntimeSubmitDisposition
{
  NoAction,
  ContinueLoop,
  BreakLoop
};

struct RuntimeSubmitOutcome
{
  RuntimeSubmitDisposition disposition = RuntimeSubmitDisposition::NoAction;
  bool terminal_write_failed = false;
  bool terminal_signal_received = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class RuntimeSubmitController final
{
 public:
  RuntimeSubmitController(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state, RuntimeRenderer& renderer,
                          RuntimeNavigationController& navigation, RuntimeActionController& action_controller,
                          RuntimeActiveRunController& active_run_controller, RuntimePromptStashController& prompt_stash,
                          TranscriptSearchController& transcript_search, RuntimeSubagentWorkspaceController& subagent_workspace, SelectorHost& selectors,
                          SettingsDialog& settings_dialog, SessionSelector& session_selector);
  RuntimeSubmitController(RuntimeSubmitController const&) = delete;
  RuntimeSubmitController& operator=(RuntimeSubmitController const&) = delete;

  [[nodiscard]] RuntimeSubmitOutcome submit(std::optional<std::string> forced_submission = std::nullopt);

 private:
  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  Composer& draft_state_;
  RuntimeRenderer& renderer_;
  RuntimeNavigationController& navigation_;
  RuntimeActionController& action_controller_;
  RuntimeActiveRunController& active_run_controller_;
  RuntimePromptStashController& prompt_stash_;
  TranscriptSearchController& transcript_search_;
  RuntimeSubagentWorkspaceController& subagent_workspace_;
  SelectorHost& selectors_;
  SettingsDialog& settings_dialog_;
  SessionSelector& session_selector_;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::tui
