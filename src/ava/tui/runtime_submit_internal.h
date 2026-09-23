#pragma once

#include <optional>
#include <string>
#include "debug.h"

namespace ava::tui {

enum class ActiveSelectList;
class RuntimeActionController;
class RuntimeActiveRunController;
struct RuntimeDraftState;
class RuntimeNavigationController;
class RuntimePresentationState;
class RuntimePromptStashController;
class RuntimeRenderer;
class RuntimeSubagentWorkspaceController;
class TranscriptSearchController;
struct TuiRuntimeOptions;

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
  RuntimeSubmitController(TuiRuntimeOptions& options, RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                          RuntimeNavigationController& navigation, RuntimeActionController& action_controller,
                          RuntimeActiveRunController& active_run_controller, RuntimePromptStashController& prompt_stash,
                          TranscriptSearchController& transcript_search, RuntimeSubagentWorkspaceController& subagent_workspace,
                          ActiveSelectList& active_select_list);
  RuntimeSubmitController(RuntimeSubmitController const&) = delete;
  RuntimeSubmitController& operator=(RuntimeSubmitController const&) = delete;

  [[nodiscard]] RuntimeSubmitOutcome submit(std::optional<std::string> forced_submission = std::nullopt);

 private:
  TuiRuntimeOptions& options_;
  RuntimePresentationState& presentation_state_;
  RuntimeDraftState& draft_state_;
  RuntimeRenderer& renderer_;
  RuntimeNavigationController& navigation_;
  RuntimeActionController& action_controller_;
  RuntimeActiveRunController& active_run_controller_;
  RuntimePromptStashController& prompt_stash_;
  TranscriptSearchController& transcript_search_;
  RuntimeSubagentWorkspaceController& subagent_workspace_;
  ActiveSelectList& active_select_list_;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::tui
