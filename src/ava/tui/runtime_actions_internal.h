// Coordinates local TUI actions and calls to frontend services.
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
// Every member, including timer callbacks, is TUI-thread-only. Frontend callbacks
// perform backend operations; this controller updates display state through its owner.
// All borrowed owners outlive it, and workers never access it. The final timer member
// unregisters before the state it uses is destroyed; its loop must outlive it.
class RuntimeActionController final
{
 public:
  // TUI thread: bind stable session owners; construction marks an initial display check due
  // without invoking frontend callbacks or painting.
  RuntimeActionController(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state, RuntimeRenderer& renderer,
                          SelectorHost& selectors, SessionSelector& session_selector, PendingAttachments& attachments);
  // Not copyable: one controller handles local actions, and the timer targets its stable address.
  RuntimeActionController(RuntimeActionController const&) = delete;
  RuntimeActionController& operator=(RuntimeActionController const&) = delete;

  // TUI thread: attach the 500 ms timer once; it only marks display reload work due.
  // The loop outlives this stable owner; maybe_reload_display_settings performs the check.
  // Session supplies its control timer's epoch so their deadlines coincide every fifth tick.
  void attach_loop(TuiEventLoop& loop, TuiTimer::Clock::time_point epoch = TuiTimer::Clock::now());
  // TUI thread: consume initial or timer-marked work once; repeated calls without
  // a due flag do not check. TerminalFailure means rendering has already failed.
  // Applied updates display values without requesting a final frame, so callers can
  // rebase/reapply any settings preview before requesting that frame.
  [[nodiscard]] DisplaySettingsReloadPollOutcome maybe_reload_display_settings();
  // TUI thread: reset draft interaction and transient status, then clear text. Return
  // whether the editor changed so routing can decide whether Interrupt still applies.
  bool clear_draft_for_interrupt();
  // TUI thread: paint pending status, release terminal protocols and call the blocking
  // frontend editor callback. Restore terminal/geometry before feedback; return paint success.
  [[nodiscard]] bool open_external_editor();
  // TUI thread: release terminal protocols before suspending the process group, restore
  // them after resume/failure, then paint feedback. Return paint success, not suspension
  // success. Session must permit this handoff.
  [[nodiscard]] bool suspend_to_background();
  // TUI thread: stage an already imported reference with an optional permitted preview
  // then request feedback rendering. Importing and validating the image stays with the frontend.
  // True permits later rendering; it does not prove a queued frame was painted.
  [[nodiscard]] bool queue_pending_image_attachment(ava::session::ImageAttachmentRef const& imported, std::string label, std::string status);
  // TUI thread: paint pending status before asking the frontend to import an image; only
  // a successful image result is staged for the next prompt. False reports a known paint
  // failure; true may leave the final feedback frame queued.
  [[nodiscard]] bool paste_clipboard_image();
  // TUI thread: request clipboard copying from the current transcript and publish delivery
  // feedback. Preserve transcript content and selection; return whether the frame request succeeded.
  [[nodiscard]] bool copy_latest_assistant_message();
  // TUI thread: ask the frontend to choose the next reasoning level and show its feedback.
  // The caller performs the final render.
  void cycle_reasoning();
  // TUI thread: toggle only presentation visibility and move the transcript to live tail.
  // The caller paints the updated display state.
  void toggle_thinking_visibility();
  // TUI thread: disarm draft Escape/archive confirmation, open fresh model rows and
  // request a frame at live tail. Return whether the request succeeded. The caller retires
  // any prior feature-specific preview.
  [[nodiscard]] bool open_model_selector();
  // TUI thread: open choices for the current model when available. A chained request
  // silently keeps successful model feedback if no reasoning selector is needed. Return
  // true when unavailable, otherwise return whether the frame request succeeded.
  [[nodiscard]] bool open_reasoning_selector(bool chained_from_model_selection = false);
  // TUI thread: open current scoped-model rows after retiring Escape/archive intent;
  // request the live-tail frame without changing backend model scope yet. Return whether
  // the frame request succeeded.
  [[nodiscard]] bool open_scoped_model_selector();
  // TUI thread: close an open overview or read fresh backend facts for a new popup.
  // The popup retains its display data until closed; Enter/Escape do not mutate backend state.
  // Return whether the frame request succeeded.
  [[nodiscard]] bool toggle_startup_overview();
  // TUI thread: ask the frontend for forkable rows, borrowing initial_query only
  // for the call. Show a selector on success or error feedback on failure; return whether
  // the frame request succeeded.
  [[nodiscard]] bool open_fork_user_turn_selector(std::string_view initial_query = {});
  // TUI thread: obtain owned, copyable user-turn rows from the frontend, then request
  // a frame. Return whether that frame request succeeded.
  [[nodiscard]] bool open_copy_user_turn_selector(std::string_view initial_query = {});
  // TUI thread: cycle backend model and refresh committed facts on success; publish
  // errors without replacing those facts. Caller performs final rendering.
  void cycle_model(bool forward);

  // Generated printing uses addresses for borrowed owners; it never traverses their content.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI timer callback: mark work due without calling the frontend or painting;
  // Session services the flag at its ordered display-reload phase.
  void mark_display_reload_due(TuiTimer::Clock::time_point now);
  // Borrowed from the app; supplies callbacks for local settings, catalogs, clipboard and editor work.
  TuiFrontEnd& frontend_;
  // Borrowed from Session; display state, read as const and updated through its methods.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; editor; actions reset interaction before terminal handoff or new draft work.
  Composer& draft_state_;
  // Borrowed from Session; renderer; pending status is painted before blocking handoffs.
  RuntimeRenderer& renderer_;
  // Borrowed from Session; sole list owner; actions replace the displayed selector here.
  SelectorHost& selectors_;
  // Borrowed from Session; archive confirmation owner; new selector actions disarm stale intent.
  SessionSelector& session_selector_;
  // Borrowed from Session; staging owner; imported references and badges move together.
  PendingAttachments& attachments_;
  // TUI-thread last reported reload failure; suppress repeated bells until a successful check.
  std::optional<ava::core::NoLogString> last_display_reload_error_;
  // TUI-thread discovery flag, initially true for the first check. Timer sets it; service
  // consumes it before calling the frontend, so turns without a due flag do no extra work.
  bool display_reload_due_ = true;
  // TUI-thread repeating registration, destroyed first to retire callbacks before other state.
  // The loop outlives this controller and its registration.
  TuiTimer display_reload_timer_;
};

}  // namespace ava::tui
