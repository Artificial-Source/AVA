#pragma once

#include "TuiTimer.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/core/NoLogString.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace ava::session {
struct ImageAttachmentRef;
} // namespace ava::session

namespace ava::tui {

class Composer;
class PendingAttachments;
class TuiFrontEnd;
class RuntimeRenderer;
class SessionSelector;
class TuiEventLoop;

class RuntimeActionController final
{
 public:
  RuntimeActionController(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state, RuntimeRenderer& renderer,
                          SelectorHost& selectors, SessionSelector& session_selector, PendingAttachments& attachments);
  RuntimeActionController(RuntimeActionController const&) = delete;
  RuntimeActionController& operator=(RuntimeActionController const&) = delete;

  // Attach automatic 500ms discovery once; the loop outlives this stable owner.
  void attach_loop(TuiEventLoop& loop);
  // Attached service consumes timer-marked work; unattached calls explicitly
  // request a check. TerminalFailure only when a required render fails.
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
  // Toggle the read-only startup overview select-list. No-op when no snapshot is present.
  [[nodiscard]] bool toggle_startup_overview();
  // Shared with snapshot-apply / prompt-preempt paths that must close or rebuild overview.
  [[nodiscard]] SelectorHost& selectors() noexcept { return selectors_; }
  [[nodiscard]] SelectorHost const& selectors() const noexcept { return selectors_; }
  [[nodiscard]] bool open_fork_user_turn_selector(std::string_view initial_query = {});
  [[nodiscard]] bool open_copy_user_turn_selector(std::string_view initial_query = {});
  void cycle_model(bool forward);

  // Generated printing uses addresses for borrowed owners; it never traverses their content.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  void mark_display_reload_due(TuiTimer::Clock::time_point now);
  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  Composer& draft_state_;
  RuntimeRenderer& renderer_;
  SelectorHost& selectors_;
  SessionSelector& session_selector_;
  PendingAttachments& attachments_;
  std::optional<ava::core::NoLogString> last_display_reload_error_;
  bool loop_attached_ = false;
  bool display_reload_due_ = true;
  TuiTimer display_reload_timer_;
};

}  // namespace ava::tui
