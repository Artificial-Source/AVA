// Coordinates TUI-thread layout, immutable view access, frame scheduling, and terminal painting.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TranscriptView.h"
#include "ava/tui/TuiTimerHandle.h"
#include "ava/tui/composer_internal.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

namespace ava::tui {

class RuntimePresentationState;

class TuiFrontEnd;
class TuiTimeoutHandler;

namespace terminal {
class Context;
}

// TUI thread: read application terminal dimensions, falling back to 80x24 before usable geometry is available.
[[nodiscard]] std::pair<std::size_t, std::size_t> terminal_size();

// TUI-thread wheel burst identity; reversing direction must take effect without waiting for throttling.
enum class WheelDirection
{
  // Older-history direction used to recognize repeated upward wheel reports.
  Up,
  // Live-tail direction; an immediate reversal resets the accepted burst timestamp.
  Down,
};

// Shared transcript mouse-wheel step for idle and active-run paths. Selectors,
// prompts, the sidebar drawer, and selection edge-autoscroll keep one-row steps.
inline constexpr std::size_t kTranscriptWheelScrollRows = 3;

// Renderer-owned TUI-thread value that limits repeated same-direction wheel input. It has no borrowed state or
// timers; accepting a reversal starts a new burst immediately.
class WheelBurstGovernor final
{
 public:
  // TUI-thread monotonic time keeps scheduling independent of wall-clock changes.
  using Clock = std::chrono::steady_clock;
  // Minimum TUI-thread spacing between accepted same-direction reports; dropped reports do not extend it.
  static constexpr auto kAcceptedEventInterval = std::chrono::milliseconds(40);

  // TUI thread: drop same-direction events inside the interval without extending it; accept a reversal
  // immediately and retain its direction/time as the new baseline.
  [[nodiscard]] bool accept(WheelDirection direction, Clock::time_point now = Clock::now());
  // TUI thread: throttle wheel key input at time now as accept() does; any other key resets the burst history.
  //
  // Returns true when key should be processed: always for non-wheel keys.
  [[nodiscard]] bool accept_input(terminal::Key key, Clock::time_point now = Clock::now());
  // TUI thread: discard burst history after non-wheel input or session transition so the next wheel is
  // accepted.
  void reset();

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI-thread timestamp of the last accepted report; reset removes throttling history.
  std::optional<Clock::time_point> last_accepted_at_ = std::nullopt;
  // TUI-thread direction paired with last_accepted_at_; an opposite report is accepted immediately.
  std::optional<WheelDirection> last_accepted_direction_ = std::nullopt;
};

// TUI-thread invalidation strength; Full supersedes Footer when requests coalesce.
enum class FrameRenderKind
{
  // Only the processing footer needs updating when no competing surface requires a complete frame.
  Footer,
  // Rebuild the visible frame, including transcript, editor, and modal surfaces.
  Full,
};

// Renderer-owned TUI-thread value that coalesces invalidations and latches paint failure. It owns no timer:
// RuntimeRenderer mirrors its deadline into the timer registry and consumes work only at paint/handoff.
class FrameScheduler final
{
 public:
  // TUI-thread monotonic time keeps scheduling independent of wall-clock changes.
  using Clock = std::chrono::steady_clock;
  // A 16 ms cadence measured from the last completed paint bounds ordinary repaint frequency.
  static constexpr auto kFrameInterval = std::chrono::milliseconds(16);

  // TUI thread: create a deadline from the last paint or strengthen pending work to Full; failure rejects new
  // work.
  void request(FrameRenderKind kind, Clock::time_point now);
  // TUI thread: indicate queued invalidation, independent of whether its deadline has arrived.
  [[nodiscard]] bool pending() const;
  // TUI thread: copy the pending wake deadline for arming a timer; no pending request has no deadline.
  [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept;
  // TUI thread paint stage: consume pending strength only when due; caller records the eventual paint result.
  [[nodiscard]] std::optional<FrameRenderKind> take_due(Clock::time_point now);
  // TUI thread urgent handoff: consume work regardless of cadence and retire its deadline.
  [[nodiscard]] std::optional<FrameRenderKind> take_pending();
  // TUI thread: record cadence origin and latch terminal failure before further requests or timer
  // synchronization.
  void paint_completed(Clock::time_point completed_at, bool succeeded);
  // TUI thread before synchronous handoff: remove stale invalidation without clearing the failure latch.
  void discard_pending();
  // TUI thread: report a latched paint failure so session shutdown can stop further terminal writes.
  [[nodiscard]] bool failed() const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI-thread coalesced invalidation; Full wins until take/discard consumes it.
  std::optional<FrameRenderKind> pending_kind_ = std::nullopt;
  // TUI-thread eligibility time created with the first pending request and cleared on consumption.
  std::optional<Clock::time_point> deadline_ = std::nullopt;
  // TUI-thread cadence origin set after each actual paint; unset until the first attempt.
  std::optional<Clock::time_point> last_paint_completed_ = std::nullopt;
  // TUI-thread terminal-write failure latch; no operation resets it or resumes painting.
  bool failed_ = false;
};

// TUI thread: callers choose the existing live-tail transition they intend.
enum class LiveTailReason
{
  Requested, // Defer clearing the detached count/sidebar until the next paint.
  LocalCommand, // Clear the count/sidebar now but retain pending layout work.
  Navigation, // Explicit navigation also discards pending detached work.
};

// TUI-thread policy after transcript mutation: preserve painted rows for incoming output, but prepare new
// layout immediately for a user expansion/collapse.
enum class TranscriptUpdateMode
{
  // Keep the painted detached layout until navigation or a synchronization point.
  Deferred,
  // Prepare the changed layout immediately to preserve the reader's anchor after expansion/collapse.
  Immediate
};

// Session-owned TUI-thread coordinator for layout caches, TranscriptView, and painting. Feature
// owners mutate viewport only through its named operations and read const views. The session serializes all
// access; there is no UI mutex. Borrowed frontend, projection, Composer, Context, and timer registry outlive it;
// member timers retire before callback targets are destroyed.
class RuntimeRenderer final
{
 public:
  // TUI thread: register timers in the earlier-lived registry, then borrow presentation,
  // Composer, frontend and optional Context. Registration never dispatches callbacks.
  // A null frontend is supported; a supplied Context applies cursor settings.
  RuntimeRenderer(TuiTimeoutHandler const& tui_timeout_handler, TuiFrontEnd* frontend, RuntimePresentationState& presentation, Composer& draft_state,
                  terminal::Context* terminal_context = nullptr);
  // Not copyable or movable: timer callbacks hold this address, and one owner manages caches and frames.
  RuntimeRenderer(RuntimeRenderer const&) = delete;
  RuntimeRenderer& operator=(RuntimeRenderer const&) = delete;
  RuntimeRenderer(RuntimeRenderer&&) = delete;
  RuntimeRenderer& operator=(RuntimeRenderer&&) = delete;

  // TUI thread: request an urgent full paint that thaws detached layout at the
  // batch boundary. Use paint_for_handoff only when synchronous output is required.
  [[nodiscard]] bool render();
  // TUI thread: coalesce invalidation and synchronize deadlines without painting.
  // Return false only for an already latched paint failure.
  [[nodiscard]] bool request_render(FrameRenderKind kind = FrameRenderKind::Full);
  // Composition calls this after each application/input batch and before wait.
  // It also registers any changed view-owned absolute deadlines.
  [[nodiscard]] bool paint_ready_frame();
  // Paint synchronously when a prompt or terminal handoff needs completed output; never call from a timer.
  [[nodiscard]] bool paint_for_handoff();
  // TUI thread: expose the permanent paint-failure latch to session control without attempting more output.
  [[nodiscard]] bool render_failed() const;
  // TUI thread before navigation: update the output count that reserves rows, rebuild as needed, then restore viewport and
  // selection from the same deferred anchor.
  void synchronize_detached_transcript_layout();

  // TUI thread: prepare selection layout before synchronous selection/header callbacks, then reconcile timers;
  // callbacks must keep renderer and view alive.
  [[nodiscard]] TranscriptSelectionMouseResult handle_transcript_selection_mouse(InputEvent const& event, std::function<bool(std::size_t)> const& toggle_tool,
                                                                                 std::function<bool(std::size_t)> const& toggle_thinking);
  // TUI thread: renew indicator visibility, request a full frame, and mirror its expiry into the timer registry.
  void show_transcript_position_indicator(TranscriptPositionIndicatorState::Clock::time_point now = TranscriptPositionIndicatorState::Clock::now());
  // TUI thread: copy from the retained layout cache and reconcile interaction deadlines. Copy does not
  // rebuild it, but earlier layout preparation may have changed it since the last paint.
  [[nodiscard]] bool copy_transcript_selection();
  // TUI thread: clear the range and pending item-index adjustments, then cancel obsolete selection deadlines.
  void clear_transcript_selection();
  // Ends Selecting/HeaderArmed and draft mouse-select without discarding a committed
  // transcript range. Used on Shift cancel, suspend/editor handoff, and mouse rearm.
  void cancel_pointer_interaction();
  // TUI thread before replacement session data: clear viewport/selection, invalidate old layout/frame caches,
  // and reset wheel history.
  void reset_for_session_transition();

  // TUI thread: synchronize deferred updates and prepare the scroll limit before scrolling; positive rows
  // move toward older history. Reconcile view timers after the move.
  void scroll_transcript(std::ptrdiff_t rows);
  // TUI thread: clamp explicit offset against prepared layout and discard deferred viewport updates before
  // synchronizing timers.
  void navigate_transcript(std::size_t offset);
  // TUI thread: choose when to clear the detached count/sidebar and saved anchor, then reconcile deadlines without painting.
  void show_live_tail(LiveTailReason reason);
  // TUI thread after transcript/search mutation: consume the matching begin token using immediate anchor
  // restoration or deferred output preservation.
  void complete_transcript_update(TranscriptUpdate const& update, std::ptrdiff_t shift, std::size_t event_count,
                                  TranscriptUpdateMode mode = TranscriptUpdateMode::Deferred);
  // TUI thread after fallback cap eviction, with no deferred viewport: queue live selection index
  // adjustments for its next layout preparation and synchronize deadlines.
  void note_transcript_eviction(std::ptrdiff_t shift);
  // TUI thread: borrow read-only viewport facts until this renderer dies; mutate through named renderer
  // transitions.
  [[nodiscard]] TranscriptView const& transcript() const noexcept;
  // TUI thread: forget throttle history when session or input policy changes.
  void reset_wheel_input();
  // TUI thread: share wheel throttling across idle/active handlers; non-wheel input terminates the burst.
  [[nodiscard]] bool accept_wheel_input(terminal::Key key, WheelBurstGovernor::Clock::time_point now = WheelBurstGovernor::Clock::now());
  // TUI thread: refresh geometry caches from published screen dimensions and return the maximum scroll;
  // invalidates prior layout borrows.
  [[nodiscard]] std::size_t prepare_transcript_layout();
  // TUI thread before transcript mutation: prepare layout and copy the content-relative viewport anchor.
  [[nodiscard]] detail::TranscriptViewportAnchor capture_transcript_viewport();
  // TUI thread before mutation: capture the first detached anchor and freeze sidebar; pair with completion
  // after search/transcript publication.
  [[nodiscard]] TranscriptUpdate begin_transcript_update();
  // TUI thread: borrow current layout without refreshing it; do not retain this reference across the next
  // preparation or paint, which may change its validity and rows.
  [[nodiscard]] detail::TranscriptLayoutCache const& prepared_transcript_layout() const noexcept;
  // TUI thread: borrow renderer-owned completion matches until the next refresh/paint mutates it.
  [[nodiscard]] detail::CompletionMatchCache const& completion_cache() const noexcept;
  // TUI thread: rebuild from published Composer/catalog facts; returned model pointer expires on cache refresh
  // or renderer destruction.
  [[nodiscard]] detail::CompletionMatchModel const* refresh_completion_cache();
  // TUI thread: refresh completion matches before clamping an owner's selected index.
  [[nodiscard]] std::size_t clamp_completion(std::size_t selected);
  // TUI thread: refresh matches and return an owned rejection explanation for the selected completion.
  [[nodiscard]] std::optional<std::string> selected_completion_disabled_reason(std::size_t selected);
  // TUI thread: refresh matches and copy insertion data so Composer need not retain cache references.
  [[nodiscard]] FileReferenceSelectionText selected_completion_text(std::size_t selected);
  // TUI thread: hit-test the published file-reference palette using cached completion matches; absent means no
  // matching row.
  [[nodiscard]] std::optional<std::size_t> file_reference_completion_at(std::size_t row, std::size_t column);
  // TUI thread: hit-test the published path palette; callers apply the choice to current Composer state
  // separately.
  [[nodiscard]] std::optional<std::size_t> path_completion_at(std::size_t row, std::size_t column);
  // TUI thread: derive drawer bounds from published projection/frontend rendering facts without mutating
  // viewport ownership.
  [[nodiscard]] std::size_t sidebar_drawer_max_scroll() const;

  // Renderer-owned caches and borrowed presentation contain arbitrary message
  // bodies; generated member printing must not expose them.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread paint stage: update geometry, sidebar, and output count; choose live or frozen layout;
  // prepare selection highlights; then write the cached full frame.
  [[nodiscard]] bool render_full(bool freeze_transcript_layout);
  // TUI thread: apply cursor settings and dispatch full/footer painting. Footer writes require processing
  // and no permission/question prompt, output modal, selector, workspace, or drawer.
  [[nodiscard]] bool paint(FrameRenderKind kind, bool freeze_transcript_layout);
  // TUI thread: refresh only live layout; deferred detached selection must remain bound to rows actually drawn.
  [[nodiscard]] bool prepare_transcript_selection_authority();
  // TUI thread after mutations: mirror semantic deadlines into member timers; latched paint failure cancels
  // every render-related timer.
  void synchronize_timers();
  // TUI timer callback: supplies the wake deadline only. The paint stage checks
  // the scheduler clock itself, including deadlines crossed during input decode.
  void frame_due(std::chrono::steady_clock::time_point now);
  // TUI timer callback: expire indicator, publish frame invalidation, and synchronize timers without painting.
  void indicator_due(std::chrono::steady_clock::time_point now);
  // TUI timer callback: use validated selection geometry to advance one edge-scroll step and invalidate a frame
  // without painting.
  void autoscroll_due(std::chrono::steady_clock::time_point now);

  // Borrowed session projection, outliving this renderer; only TUI-thread feature publications alter it.
  RuntimePresentationState& presentation_;
  // Borrowed editor, outliving this renderer; paint projects/clamps its current TUI-thread state.
  Composer& draft_state_;
  // Optional borrowed terminal context for cursor settings; when supplied it outlives the renderer and is
  // touched only on the TUI thread.
  terminal::Context* terminal_context_ = nullptr;
  // Exclusive TUI-thread viewport/selection owner; timers declared later retire before its destruction.
  TranscriptView transcript_;
  // TUI-thread derived matches reused for completion hit-testing and layout; refresh invalidates exposed
  // borrows.
  detail::CompletionMatchCache completion_cache_;
  // TUI-thread layout rows retained across detached paints; selection reads the same cache rather than
  // independently rendering.
  detail::TranscriptLayoutCache transcript_layout_cache_;
  // TUI-thread last-painted rows used to minimize terminal output; session reset invalidates them.
  detail::ScreenRowCache screen_row_cache_;
  // TUI-thread wheel history shared by all navigation paths, cleared on session reset.
  WheelBurstGovernor wheel_governor_;
  // Borrowed application frontend used by rendering helpers; its lifetime covers the renderer and no worker
  // accesses it here.
  TuiFrontEnd* frontend_;
  // TUI-thread pending frame kind and deadline; timer callbacks wake the loop while the paint stage consumes
  // work.
  FrameScheduler frame_scheduler_;
  // TUI-thread urgent full-frame request set by render(); the next batch paint synchronizes detached layout.
  // Ordinary scheduled paints preserve compatible frozen rows; footer requests never force synchronization.
  bool pending_thaw_ = false;
  // TUI-thread copy of the registered frame deadline; prevents rearming an expired wake before paint consumes
  // it.
  std::optional<FrameScheduler::Clock::time_point> armed_frame_deadline_;
  // TUI-thread copy used to rearm the indicator timer only when its semantic expiry changes.
  std::optional<FrameScheduler::Clock::time_point> armed_indicator_deadline_;
  // TUI-thread copy used to mirror selection edge-drag cadence without redundant timer rearming.
  std::optional<FrameScheduler::Clock::time_point> armed_autoscroll_deadline_;
  // TUI-thread wake timer; destroyed before its scheduler/view callback dependencies and the
  // earlier-lived registry.
  TuiTimerHandle frame_timer_;
  // TUI-thread timer hides the scroll-position indicator; destroyed before the indicator owner and earlier-lived
  // event loop.
  TuiTimerHandle indicator_timer_;
  // TUI-thread timer drives held edge drags; destruction retires callbacks before TranscriptView is
  // destroyed.
  TuiTimerHandle autoscroll_timer_;
};

} // namespace ava::tui
