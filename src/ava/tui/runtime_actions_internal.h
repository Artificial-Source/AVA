// Coordinates local TUI actions and explicit frontend authority handoffs.
// See docs/engineering/tui-architecture.md for the owner relationships.
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

// Session owns local action coordination used by idle and active input routing.
// Every member, including timer discovery, is TUI-thread-only. Frontend callbacks
// retain backend authority; this controller publishes through named operations.
// All borrowed owners outlive it. No UI mutex protects this state; the final timer
// unregisters before callback-visible members die, and its loop must outlive it.
class RuntimeActionController final
{
 public:
  // TUI thread: bind stable session owners; construction schedules an initial display check
  // without invoking frontend callbacks or painting.
  RuntimeActionController(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state, RuntimeRenderer& renderer,
                          SelectorHost& selectors, SessionSelector& session_selector, PendingAttachments& attachments);
  // Copying would duplicate display discovery and action ownership for one session.
  RuntimeActionController(RuntimeActionController const&) = delete;
  // Keep the address captured by the timer and all borrowed collaborators stable.
  RuntimeActionController& operator=(RuntimeActionController const&) = delete;

  // Attach automatic 500ms discovery once; the loop outlives this stable owner.
  void attach_loop(TuiEventLoop& loop);
  // TUI thread: consume initial or timer-marked work once; repeated calls without
  // a due flag do not check. TerminalFailure means a required error-status paint failed.
  // Applied hydrates the authoritative snapshot without a final render so callers can
  // rebase/reapply settings preview (if any) and paint exactly once afterward.
  [[nodiscard]] DisplaySettingsReloadPollOutcome maybe_reload_display_settings();
  // TUI thread: reset draft interaction and transient status, then clear text. Return
  // whether the editor changed so routing can decide whether Interrupt still applies.
  bool clear_draft_for_interrupt();
  // TUI thread: paint pending status, release terminal protocols, call the blocking editor
  // authority and restore terminal/geometry before reporting its result. Return paint success.
  [[nodiscard]] bool open_external_editor();
  // TUI thread: release terminal protocols before suspending the process group, restore
  // them after resume/failure, then paint feedback. Session must permit this handoff.
  [[nodiscard]] bool suspend_to_background();
  // TUI thread: stage an already imported reference with an optional permitted preview
  // then render feedback. Backend import authority remains with the frontend.
  [[nodiscard]] bool queue_pending_image_attachment(ava::session::ImageAttachmentRef const& imported, std::string label, std::string status);
  // TUI thread: paint pending status before importing through frontend authority; only
  // a successful image result is staged for the next prompt. Return render success.
  [[nodiscard]] bool paste_clipboard_image();
  // TUI thread: request clipboard copying from the current transcript and publish delivery
  // feedback. No transcript content or selection is mutated.
  [[nodiscard]] bool copy_latest_assistant_message();
  // TUI thread: ask backend authority to choose the next level and publish returned feedback.
  // The caller performs the final render.
  void cycle_reasoning();
  // TUI thread: toggle only presentation visibility and move the transcript to live tail.
  // The caller paints the resulting projection.
  void toggle_thinking_visibility();
  // TUI thread: disarm draft Escape/archive confirmation, open fresh model rows and
  // request a frame at live tail. Caller retires any prior feature-specific preview.
  [[nodiscard]] bool open_model_selector();
  // TUI thread: open choices for the current model when available. A chained request
  // silently keeps successful model feedback if no reasoning selector is needed.
  [[nodiscard]] bool open_reasoning_selector(bool chained_from_model_selection = false);
  // TUI thread: open current scoped-model rows after retiring Escape/archive intent;
  // request the live-tail frame without changing backend model scope yet.
  [[nodiscard]] bool open_scoped_model_selector();
  // TUI thread: close an open overview or read fresh backend facts for a new popup.
  // The popup owns its projection until closed; Enter/Escape do not mutate backend state.
  [[nodiscard]] bool toggle_startup_overview();
  // TUI thread: ask frontend authority for forkable rows, borrowing initial_query only
  // for the call. Publish a selector on success or error feedback on failure.
  [[nodiscard]] bool open_fork_user_turn_selector(std::string_view initial_query = {});
  // TUI thread: obtain copyable user-turn rows through frontend authority; only the
  // selector result owns text after the call. Return render success.
  [[nodiscard]] bool open_copy_user_turn_selector(std::string_view initial_query = {});
  // TUI thread: cycle backend model and refresh committed facts on success; publish
  // errors without replacing those facts. Caller performs final rendering.
  void cycle_model(bool forward);

  // Generated printing uses addresses for borrowed owners; it never traverses their content.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI timer callback: mark work due without calling frontend authority or painting;
  // Session services the flag at its ordered display-reload phase.
  void mark_display_reload_due(TuiTimer::Clock::time_point now);
  // Borrowed from the app; boundary for local settings, catalogs, clipboard and editor authority callbacks.
  TuiFrontEnd& frontend_;
  // Borrowed from Session; sole projection; const reads and named publications keep updates explicit.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; editor; actions reset interaction before terminal handoff or new draft work.
  Composer& draft_state_;
  // Borrowed from Session; renderer; pending status is painted before blocking handoffs.
  RuntimeRenderer& renderer_;
  // Borrowed from Session; sole list owner; new action views replace its projection.
  SelectorHost& selectors_;
  // Borrowed from Session; archive confirmation owner; new selector actions disarm stale intent.
  SessionSelector& session_selector_;
  // Borrowed from Session; staging owner; imported references and badges move together.
  PendingAttachments& attachments_;
  // TUI-thread last reported reload failure; suppress repeated bells until a successful check.
  std::optional<ava::core::NoLogString> last_display_reload_error_;
  // TUI-thread discovery flag, initially true for the first check. Timer sets it; service
  // consumes it before invoking reload authority so unchanged turns do no extra work.
  bool display_reload_due_ = true;
  // TUI-thread repeating registration, destroyed first to retire callbacks before other state.
  // The loop outlives this controller and its registration.
  TuiTimer display_reload_timer_;
};

}  // namespace ava::tui
