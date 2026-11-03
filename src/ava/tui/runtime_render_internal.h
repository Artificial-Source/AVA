// Coordinates TUI-thread layout, immutable view access, frame scheduling, and terminal painting.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TranscriptView.h"
#include "ava/tui/TuiTimer.h"
#include "ava/tui/composer_internal.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

namespace ava::tui {

class RuntimePresentationState;

class TuiFrontEnd;
class TuiEventLoop;

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

  // Same-direction events inside the interval are dropped. An opposite-direction
  // event is accepted immediately and becomes the new accepted direction/time.
  // TUI thread: retain the last accepted direction/time; dropped repeats leave that baseline unchanged.
  [[nodiscard]] bool accept(WheelDirection direction, Clock::time_point now = Clock::now());
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

// TUI thread: throttle wheel keys through the borrowed governor; any other key resets burst history and passes
// through.
[[nodiscard]] bool runtime_wheel_input_accepted(WheelBurstGovernor& governor, terminal::Key key,
                                                WheelBurstGovernor::Clock::time_point now = WheelBurstGovernor::Clock::now());

// TUI-thread invalidation strength; Full supersedes Footer when requests coalesce.
enum class FrameRenderKind
{
  // Only processing chrome needs updating when no competing surface requires a complete frame.
  Footer,
  // Rebuild the visible frame, including transcript, editor, and modal surfaces.
  Full,
};

// Renderer-owned TUI-thread value that coalesces invalidations and latches paint failure. It owns no timer:
// RuntimeRenderer mirrors its deadline into the loop and consumes work only at paint/handoff.
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
  // TUI thread: copy the pending wake deadline for timer registration; no pending request has no deadline.
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
  Requested, // Defer clearing detached chrome until the next paint.
  LocalCommand, // Clear the count/sidebar now but retain pending layout work.
  Navigation, // Explicit navigation also discards pending detached work.
};

// Deferred output preserves the painted viewport; a user expansion needs the new layout immediately.
// TUI-thread completion policy selected after mutation; explicit expansion restores layout immediately.
enum class TranscriptUpdateMode
{
  // Preserve painted detached authority until navigation or a synchronization point.
  Deferred,
  // Prepare the changed layout immediately to preserve the reader's anchor after expansion/collapse.
  Immediate
};

// Session-owned, nonmoving TUI-thread coordinator for layout caches, TranscriptView, and painting. Feature
// owners mutate viewport only through its named operations and read const views. The session serializes all
// access; there is no UI mutex. Borrowed frontend, projection, Composer, Context, and attached loop outlive it;
// member timers retire before callback targets are destroyed.
class RuntimeRenderer final
{
 public:
  // Snapshot/sidebar/Composer, frontend and optional Context outlive this renderer.
  // Render the snapshot and apply cursor settings through Context when provided.
  // TUI thread: bind previously constructed display owners; nullable frontend/Context follow the renderer
  // helpers' existing contracts.
  RuntimeRenderer(TuiFrontEnd* frontend, RuntimePresentationState& presentation, Composer& draft_state, terminal::Context* terminal_context = nullptr);
  // Single ownership keeps caches and timer callbacks bound to one stable renderer.
  RuntimeRenderer(RuntimeRenderer const&) = delete;
  // Do not replace the presentation bindings or duplicate pending frame authority.
  RuntimeRenderer& operator=(RuntimeRenderer const&) = delete;
  // Timer callbacks address this object, so moving after attachment is forbidden.
  RuntimeRenderer(RuntimeRenderer&&) = delete;
  // A registered callback target and its borrowed lifetime bindings must keep their addresses.
  RuntimeRenderer& operator=(RuntimeRenderer&&) = delete;

  // Attach once before live dispatch. The loop outlives this nonmoving owner.
  void attach_loop(TuiEventLoop& loop);
  // TUI thread: before attachment paint synchronously for startup; after attachment request an urgent full
  // paint that thaws detached layout at the batch boundary.
  [[nodiscard]] bool render();
  // TUI thread: coalesce invalidation and synchronize deadlines without painting, including before attachment.
  // Return false only for an already latched paint failure.
  [[nodiscard]] bool request_render(FrameRenderKind kind = FrameRenderKind::Full);
  // Composition calls this after each application/input batch and before wait.
  // It also registers any changed view-owned absolute deadlines.
  [[nodiscard]] bool paint_ready_frame();
  // Explicit synchronous terminal/authority handoffs only, never timer callbacks.
  [[nodiscard]] bool paint_for_handoff();
  // TUI thread: expose the permanent paint-failure latch to session control without attempting more output.
  [[nodiscard]] bool render_failed() const;
  // TUI thread before navigation: publish reserved-row chrome, rebuild as needed, then restore viewport and
  // selection from the same deferred anchor.
  void synchronize_detached_transcript_layout();

  // TUI thread: validate drawn authority before synchronous selection/header callbacks, then reconcile timers;
  // callbacks must keep renderer and view alive.
  [[nodiscard]] TranscriptSelectionMouseResult handle_transcript_selection_mouse(InputEvent const& event, std::function<bool(std::size_t)> const& toggle_tool,
                                                                                 std::function<bool(std::size_t)> const& toggle_thinking);
  // TUI thread: renew indicator visibility, request a full frame, and mirror its expiry into the loop.
  void show_transcript_position_indicator(TranscriptPositionIndicatorState::Clock::time_point now = TranscriptPositionIndicatorState::Clock::now());
  // TUI thread: copy from the existing layout authority and reconcile interaction deadlines; never rebuild the
  // copied layout implicitly.
  [[nodiscard]] bool copy_transcript_selection();
  // TUI thread: clear range and item-shift debt, then cancel obsolete selection deadlines.
  void clear_transcript_selection();
  // Ends Selecting/HeaderArmed and draft mouse-select without discarding a committed
  // transcript range. Used on Shift cancel, suspend/editor handoff, and mouse rearm.
  void cancel_pointer_interaction();
  // TUI thread before replacement session data: clear viewport/selection, invalidate old layout/frame caches,
  // and reset wheel history.
  void reset_for_session_transition();

  // TUI thread: renderer prepares/clamps layout and synchronizes timers around all
  // viewport changes. Readers may borrow the const view until the renderer dies.
  // TUI thread: synchronize detached data and prepare maximum before scrolling; positive rows move toward older
  // history.
  void scroll_transcript(std::ptrdiff_t rows);
  // TUI thread: clamp explicit offset against prepared layout and discard deferred navigation debt before
  // synchronizing timers.
  void navigate_transcript(std::size_t offset);
  // TUI thread: choose the named chrome/anchor cleanup policy, then reconcile deadlines without painting.
  void show_live_tail(LiveTailReason reason);
  // TUI thread after transcript/search mutation: consume the matching begin token using immediate anchor
  // restoration or deferred output preservation.
  void complete_transcript_update(TranscriptUpdate const& update, std::ptrdiff_t shift, std::size_t event_count,
                                  TranscriptUpdateMode mode = TranscriptUpdateMode::Deferred);
  // Fallback completion has no deferred viewport; remap live selection after eviction.
  // TUI thread after fallback cap eviction: remap live selection at its next authority preparation and
  // synchronize deadlines.
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
  // Diagnostic/layout borrows expire at the next preparation or paint that mutates
  // the corresponding cache; callers must not retain them across those operations.
  // TUI thread: borrow current layout without refreshing it; validity/content can change on the next
  // preparation or paint.
  [[nodiscard]] detail::TranscriptLayoutCache const& prepared_transcript_layout() const noexcept;
  // TUI thread: borrow renderer-owned completion authority until the next refresh/paint mutates it.
  [[nodiscard]] detail::CompletionMatchCache const& completion_cache() const noexcept;
  // TUI thread: inspect last paint rows; the borrow is invalidated by a later paint or session reset.
  [[nodiscard]] detail::ScreenRowCache const& painted_frame() const noexcept;
  // TUI thread: rebuild from published Composer/catalog facts; returned model pointer expires on cache refresh
  // or renderer destruction.
  [[nodiscard]] detail::CompletionMatchModel const* refresh_completion_cache();
  // TUI thread: refresh completion authority before clamping an owner's selected index.
  [[nodiscard]] std::size_t clamp_completion(std::size_t selected);
  // TUI thread: refresh matches and return an owned rejection explanation for the selected completion.
  [[nodiscard]] std::optional<std::string> selected_completion_disabled_reason(std::size_t selected);
  // TUI thread: refresh matches and copy insertion data so Composer need not retain cache references.
  [[nodiscard]] FileReferenceSelectionText selected_completion_text(std::size_t selected);
  // TUI thread: hit-test the published file-reference palette using renderer cache authority; absent means no
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
  // TUI thread paint stage: publish geometry/chrome, choose frozen authority, project selection, then emit the
  // cached full frame.
  [[nodiscard]] bool render_full(bool freeze_transcript_layout);
  // TUI thread: apply cursor settings and dispatch frame strength; footer painting is skipped when a modal/full
  // surface owns the screen.
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
  // TUI timer callback: use valid selection authority to advance one edge-scroll step and invalidate a frame
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
  // TUI-thread row authority retained across detached paints; selection reads the same cache rather than
  // independently rendering.
  detail::TranscriptLayoutCache transcript_layout_cache_;
  // TUI-thread last-painted rows used to minimize terminal output; session reset invalidates them.
  detail::ScreenRowCache screen_row_cache_;
  // TUI-thread wheel history shared by all navigation paths, cleared on session reset.
  WheelBurstGovernor wheel_governor_;
  // Borrowed application frontend used by rendering helpers; its lifetime covers the renderer and no worker
  // accesses it here.
  TuiFrontEnd* frontend_;
  // TUI-thread invalidation/deadline authority; timer callbacks wake the loop while the paint stage consumes
  // work.
  FrameScheduler frame_scheduler_;
  // TUI-thread one-way attachment marker; render() switches from startup painting to batched invalidation once
  // set.
  bool loop_attached_ = false;
  // Full urgent invalidation synchronizes detached layout; scheduled paints keep
  // the existing compatible frozen authority. Footer invalidation cannot thaw it.
  // TUI-thread urgent full-frame request set by render(); paint consumes it to synchronize detached layout
  // immediately.
  bool pending_thaw_ = false;
  // TUI-thread copy of the registered frame deadline; prevents rearming an expired wake before paint consumes
  // it.
  std::optional<FrameScheduler::Clock::time_point> armed_frame_deadline_;
  // TUI-thread copy used to change indicator registration only when its semantic expiry changes.
  std::optional<FrameScheduler::Clock::time_point> armed_indicator_deadline_;
  // TUI-thread copy used to mirror selection edge-drag cadence without redundant timer rearming.
  std::optional<FrameScheduler::Clock::time_point> armed_autoscroll_deadline_;
  // Handles retire before callback target state and the earlier-lived loop.
  // TUI-thread registration supplies wake readiness only; destruction retires its callback before
  // scheduler/view teardown.
  TuiTimer frame_timer_;
  // TUI-thread registration expires position chrome; destroyed before the indicator owner and earlier-lived
  // event loop.
  TuiTimer indicator_timer_;
  // TUI-thread registration drives held edge drags; destruction retires callbacks before TranscriptView is
  // destroyed.
  TuiTimer autoscroll_timer_;
};

} // namespace ava::tui
