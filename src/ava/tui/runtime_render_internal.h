#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TranscriptView.h"
#include "ava/tui/TuiTimer.h"
#include "ava/tui/composer_internal.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace ava::tui {

class RuntimePresentationState;

class TuiFrontEnd;
class TuiEventLoop;

namespace terminal {
class Context;
}

[[nodiscard]] std::pair<std::size_t, std::size_t> terminal_size();

enum class WheelDirection
{
  Up,
  Down,
};

// Shared transcript mouse-wheel step for idle and active-run paths. Selectors,
// prompts, the sidebar drawer, and selection edge-autoscroll keep one-row steps.
inline constexpr std::size_t kTranscriptWheelScrollRows = 3;

class WheelBurstGovernor final
{
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kAcceptedEventInterval = std::chrono::milliseconds(40);

  // Same-direction events inside the interval are dropped. An opposite-direction
  // event is accepted immediately and becomes the new accepted direction/time.
  [[nodiscard]] bool accept(WheelDirection direction, Clock::time_point now = Clock::now());
  void reset();

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  std::optional<Clock::time_point> last_accepted_at_ = std::nullopt;
  std::optional<WheelDirection> last_accepted_direction_ = std::nullopt;
};

[[nodiscard]] bool runtime_wheel_input_accepted(WheelBurstGovernor& governor, terminal::Key key,
                                                WheelBurstGovernor::Clock::time_point now = WheelBurstGovernor::Clock::now());

enum class FrameRenderKind
{
  Footer,
  Full,
};

class FrameScheduler final
{
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kFrameInterval = std::chrono::milliseconds(16);

  void request(FrameRenderKind kind, Clock::time_point now);
  [[nodiscard]] bool pending() const;
  [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept;
  [[nodiscard]] std::optional<FrameRenderKind> take_due(Clock::time_point now);
  [[nodiscard]] std::optional<FrameRenderKind> take_pending();
  void paint_completed(Clock::time_point completed_at, bool succeeded);
  void discard_pending();
  [[nodiscard]] bool failed() const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  std::optional<FrameRenderKind> pending_kind_ = std::nullopt;
  std::optional<Clock::time_point> deadline_ = std::nullopt;
  std::optional<Clock::time_point> last_paint_completed_ = std::nullopt;
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
enum class TranscriptUpdateMode
{
  Deferred,
  Immediate
};

class RuntimeRenderer final
{
 public:
  // Snapshot/sidebar/Composer, frontend and optional Context outlive this renderer.
  // Render the snapshot and apply cursor settings through Context when provided.
  RuntimeRenderer(TuiFrontEnd* frontend, RuntimePresentationState& presentation, Composer& draft_state, terminal::Context* terminal_context = nullptr);
  RuntimeRenderer(RuntimeRenderer const&) = delete;
  RuntimeRenderer& operator=(RuntimeRenderer const&) = delete;
  RuntimeRenderer(RuntimeRenderer&&) = delete;
  RuntimeRenderer& operator=(RuntimeRenderer&&) = delete;

  // Attach once before live dispatch. The loop outlives this nonmoving owner.
  // Unattached standalone fixtures retain synchronous render/explicit-flush behavior.
  void attach_loop(TuiEventLoop& loop);
  [[nodiscard]] bool render();
  [[nodiscard]] bool request_render(FrameRenderKind kind = FrameRenderKind::Full);
  // Composition calls this after each application/input batch and before wait.
  // It also registers any changed view-owned absolute deadlines.
  [[nodiscard]] bool paint_ready_frame();
  // Explicit synchronous terminal/authority handoffs only, never timer callbacks.
  [[nodiscard]] bool paint_for_handoff();
  // Standalone fixtures only; live dispatch uses paint_ready_frame.
  [[nodiscard]] bool flush_pending_render();
  [[nodiscard]] bool render_failed() const;
  void synchronize_detached_transcript_layout();

  [[nodiscard]] TranscriptSelectionMouseResult handle_transcript_selection_mouse(InputEvent const& event, std::function<bool(std::size_t)> const& toggle_tool,
                                                                                 std::function<bool(std::size_t)> const& toggle_thinking);
  void show_transcript_position_indicator(TranscriptPositionIndicatorState::Clock::time_point now = TranscriptPositionIndicatorState::Clock::now());
  [[nodiscard]] bool copy_transcript_selection();
  void clear_transcript_selection();
  // Ends Selecting/HeaderArmed and draft mouse-select without discarding a committed
  // transcript range. Used on Shift cancel, suspend/editor handoff, and mouse rearm.
  void cancel_pointer_interaction();
  void reset_for_session_transition();

  [[nodiscard]] std::unique_lock<std::recursive_mutex> lock_ui();
  // TUI thread: renderer prepares/clamps layout and synchronizes timers around all
  // viewport changes. Readers may borrow the const view until the renderer dies.
  void scroll_transcript(std::ptrdiff_t rows);
  void navigate_transcript(std::size_t offset);
  void show_live_tail(LiveTailReason reason);
  void complete_transcript_update(TranscriptUpdate const& update, std::ptrdiff_t shift, std::size_t event_count,
                                  TranscriptUpdateMode mode = TranscriptUpdateMode::Deferred);
  // Fallback completion has no deferred viewport; remap live selection after eviction.
  void note_transcript_eviction(std::ptrdiff_t shift);
  [[nodiscard]] TranscriptView const& transcript() const noexcept;
  void reset_wheel_input();
  [[nodiscard]] bool accept_wheel_input(terminal::Key key, WheelBurstGovernor::Clock::time_point now = WheelBurstGovernor::Clock::now());
  [[nodiscard]] std::size_t prepare_transcript_layout();
  [[nodiscard]] detail::TranscriptViewportAnchor capture_transcript_viewport();
  [[nodiscard]] TranscriptUpdate begin_transcript_update();
  // Diagnostic/layout borrows expire at the next preparation or paint that mutates
  // the corresponding cache; callers must not retain them across those operations.
  [[nodiscard]] detail::TranscriptLayoutCache const& prepared_transcript_layout() const noexcept;
  [[nodiscard]] detail::CompletionMatchCache const& completion_cache() const noexcept;
  [[nodiscard]] detail::ScreenRowCache const& painted_frame() const noexcept;
  [[nodiscard]] detail::CompletionMatchModel const* refresh_completion_cache();
  [[nodiscard]] std::size_t clamp_completion(std::size_t selected);
  [[nodiscard]] std::size_t previous_completion(std::size_t selected);
  [[nodiscard]] std::size_t next_completion(std::size_t selected);
  [[nodiscard]] std::optional<std::string> selected_completion_disabled_reason(std::size_t selected);
  [[nodiscard]] FileReferenceSelectionText selected_completion_text(std::size_t selected);
  [[nodiscard]] std::optional<std::size_t> file_reference_completion_at(std::size_t row, std::size_t column);
  [[nodiscard]] std::optional<std::size_t> path_completion_at(std::size_t row, std::size_t column);
  [[nodiscard]] std::size_t sidebar_drawer_max_scroll() const;

  // Renderer-owned caches and borrowed presentation contain arbitrary message
  // bodies; generated member printing must not expose them.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] bool render_full(bool freeze_transcript_layout);
  [[nodiscard]] bool paint(FrameRenderKind kind, bool freeze_transcript_layout);
  [[nodiscard]] bool prepare_transcript_selection_authority();
  void synchronize_timers();
  // TUI timer callback: supplies the wake deadline only. The paint stage checks
  // the scheduler clock itself, including deadlines crossed during input decode.
  void frame_due(std::chrono::steady_clock::time_point now);
  void indicator_due(std::chrono::steady_clock::time_point now);
  void autoscroll_due(std::chrono::steady_clock::time_point now);
  // Explicit-clock semantic servicing for unattached renderer fixtures only.
  void service_auxiliary_timers(std::chrono::steady_clock::time_point now);

  RuntimePresentationState& presentation_;
  Composer& draft_state_;
  terminal::Context* terminal_context_ = nullptr;
  TranscriptView transcript_;
  detail::CompletionMatchCache completion_cache_;
  detail::TranscriptLayoutCache transcript_layout_cache_;
  detail::ScreenRowCache screen_row_cache_;
  WheelBurstGovernor wheel_governor_;
  std::recursive_mutex ui_mutex_;
  TuiFrontEnd* frontend_;
  FrameScheduler frame_scheduler_;
  bool loop_attached_ = false;
  // Full urgent invalidation synchronizes detached layout; scheduled paints keep
  // the existing compatible frozen authority. Footer invalidation cannot thaw it.
  bool pending_thaw_ = false;
  std::optional<FrameScheduler::Clock::time_point> armed_frame_deadline_;
  std::optional<FrameScheduler::Clock::time_point> armed_indicator_deadline_;
  std::optional<FrameScheduler::Clock::time_point> armed_autoscroll_deadline_;
  // Handles retire before callback target state and the earlier-lived loop.
  TuiTimer frame_timer_;
  TuiTimer indicator_timer_;
  TuiTimer autoscroll_timer_;
};

} // namespace ava::tui
