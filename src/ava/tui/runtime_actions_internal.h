#pragma once

#include "ava/tui/runtime.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/core/NoLogString.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include "debug.h"

namespace ava::session {
struct ImageAttachmentRef;
} // namespace ava::session

namespace ava::tui {

struct RuntimeDraftState;
class TuiFrontEnd;
class RuntimeRenderer;

class RuntimeActionController final
{
 public:
  RuntimeActionController(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                          ActiveSelectList& active_select_list, std::optional<PendingSessionArchiveAction>& session_archive_confirmation);
  RuntimeActionController(RuntimeActionController const&) = delete;
  RuntimeActionController& operator=(RuntimeActionController const&) = delete;

  // Rate-limited poll. TerminalFailure only when a required render fails.
  // Applied hydrates the authoritative snapshot without a final render so callers can
  // rebase/reapply settings preview (if any) and paint exactly once afterward.
  [[nodiscard]] DisplaySettingsReloadPollOutcome maybe_reload_display_settings();
  bool clear_draft_for_interrupt();
  [[nodiscard]] bool open_external_editor();
  [[nodiscard]] bool suspend_to_background();
  [[nodiscard]] bool queue_pending_image_attachment(ava::session::ImageAttachmentRef const& imported, std::string label, std::string status);
  [[nodiscard]] bool paste_clipboard_image();
  [[nodiscard]] bool copy_latest_assistant_message();
  void cycle_reasoning();
  void toggle_thinking_visibility();
  [[nodiscard]] bool open_model_selector();
  [[nodiscard]] bool open_reasoning_selector(bool chained_from_model_selection = false);
  [[nodiscard]] bool open_scoped_model_selector();
  [[nodiscard]] bool open_session_selector();
  // Toggle the read-only startup overview select-list. No-op when no snapshot is present.
  [[nodiscard]] bool toggle_startup_overview();
  // Shared with snapshot-apply / prompt-preempt paths that must close or rebuild overview.
  [[nodiscard]] ActiveSelectList& active_select_list() noexcept { return active_select_list_; }
  [[nodiscard]] ActiveSelectList const& active_select_list() const noexcept { return active_select_list_; }
  [[nodiscard]] bool open_fork_user_turn_selector(std::string_view initial_query = {});
  [[nodiscard]] bool open_copy_user_turn_selector(std::string_view initial_query = {});
  void cycle_model(bool forward);

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  RuntimeDraftState& draft_state_;
  RuntimeRenderer& renderer_;
  ActiveSelectList& active_select_list_;
  std::optional<PendingSessionArchiveAction>& session_archive_confirmation_;
  std::chrono::steady_clock::time_point next_display_reload_check_;
  std::optional<ava::core::NoLogString> last_display_reload_error_;
};

}  // namespace ava::tui
