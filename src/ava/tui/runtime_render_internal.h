#pragma once

#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_transcript_selection_internal.h"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include "debug.h"

namespace ava::tui {

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

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::optional<Clock::time_point> last_accepted_at_ = std::nullopt;
  std::optional<WheelDirection> last_accepted_direction_ = std::nullopt;
};

[[nodiscard]] bool runtime_wheel_input_accepted(WheelBurstGovernor& governor, Key key,
                                                WheelBurstGovernor::Clock::time_point now = WheelBurstGovernor::Clock::now());

enum class FrameRenderKind
{
  Footer,
  Full,
};

class TranscriptPositionIndicatorState final
{
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kVisibleDuration = std::chrono::milliseconds(1000);

  void show(Clock::time_point now = Clock::now()) noexcept;
  void hide() noexcept;
  [[nodiscard]] bool visible() const noexcept;
  [[nodiscard]] bool expire_if_due(Clock::time_point now = Clock::now()) noexcept;
  [[nodiscard]] std::optional<Clock::duration> time_until_expiry(Clock::time_point now = Clock::now()) const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::optional<Clock::time_point> expires_at_ = std::nullopt;
};

class FrameScheduler final
{
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kFrameInterval = std::chrono::milliseconds(16);

  void request(FrameRenderKind kind, Clock::time_point now);
  [[nodiscard]] bool pending() const;
  [[nodiscard]] std::optional<FrameRenderKind> pending_kind() const;
  [[nodiscard]] Clock::duration time_until_due(Clock::time_point now) const;
  [[nodiscard]] std::optional<FrameRenderKind> take_due(Clock::time_point now);
  [[nodiscard]] std::optional<FrameRenderKind> take_pending();
  void paint_completed(Clock::time_point completed_at, bool succeeded);
  void discard_pending();
  [[nodiscard]] bool failed() const;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::optional<FrameRenderKind> pending_kind_ = std::nullopt;
  std::optional<Clock::time_point> deadline_ = std::nullopt;
  std::optional<Clock::time_point> last_paint_completed_ = std::nullopt;
  bool failed_ = false;
};

struct DeferredDetachedViewport
{
  detail::TranscriptViewportAnchor anchor = {};
  std::ptrdiff_t item_index_shift = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class RuntimeRenderer final
{
 public:
  // Render `snapshot` and apply its cursor settings through `terminal_context` when provided.
  RuntimeRenderer(ComposerSnapshot& snapshot, SidebarSnapshot& sidebar, RuntimeDraftState& draft_state, terminal::Context* terminal_context = nullptr);

  [[nodiscard]] bool render();
  [[nodiscard]] bool render_processing_frame();
  [[nodiscard]] bool request_render(FrameRenderKind kind = FrameRenderKind::Full);
  [[nodiscard]] bool flush_pending_render_if_due();
  [[nodiscard]] bool flush_pending_render();
  [[nodiscard]] bool has_pending_render() const;
  [[nodiscard]] std::chrono::steady_clock::duration time_until_pending_render() const;
  [[nodiscard]] bool render_failed() const;
  void defer_detached_transcript_update(detail::TranscriptViewportAnchor anchor, std::ptrdiff_t item_index_shift);
  void synchronize_detached_transcript_layout();
  void discard_deferred_detached_transcript_update();
  [[nodiscard]] bool has_deferred_detached_transcript_update() const;

  [[nodiscard]] TranscriptSelectionMouseResult handle_transcript_selection_mouse(InputEvent const& event, std::function<bool(std::size_t)> const& toggle_tool,
                                                                                 std::function<bool(std::size_t)> const& toggle_thinking);
  void show_transcript_position_indicator(TranscriptPositionIndicatorState::Clock::time_point now = TranscriptPositionIndicatorState::Clock::now());
  [[nodiscard]] bool transcript_position_indicator_visible() const noexcept;
  [[nodiscard]] bool copy_transcript_selection();
  void clear_transcript_selection();
  // Ends Selecting/HeaderArmed and draft mouse-select without discarding a committed
  // transcript range. Used on Shift cancel, suspend/editor handoff, and mouse rearm.
  void cancel_pointer_interaction();
  void note_live_transcript_selection_item_shift(std::ptrdiff_t item_index_shift) noexcept;
  [[nodiscard]] bool has_transcript_selection() const noexcept;
  [[nodiscard]] bool has_pointer_interaction() const noexcept;
  [[nodiscard]] std::optional<TranscriptSelectionRange> transcript_selection_range() const noexcept;
  void reset_for_session_transition();

  std::size_t transcript_scroll_offset = 0;
  std::size_t detached_new_output_count = 0;
  detail::CompletionMatchCache completion_cache;
  detail::TranscriptLayoutCache transcript_layout_cache;
  detail::ScreenRowCache screen_row_cache;
  std::optional<SidebarSnapshot> detached_sidebar_snapshot;
  WheelBurstGovernor wheel_governor;
  std::recursive_mutex ui_mutex;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] bool render_full(bool freeze_transcript_layout);
  [[nodiscard]] bool paint(FrameRenderKind kind, bool freeze_transcript_layout);
  [[nodiscard]] bool prepare_transcript_selection_authority();
  void service_auxiliary_timers(std::chrono::steady_clock::time_point now);

  ComposerSnapshot& snapshot_;
  SidebarSnapshot& sidebar_;
  RuntimeDraftState& draft_state_;
  terminal::Context* terminal_context_ = nullptr;
  RuntimeTranscriptSelectionState transcript_selection_;
  std::ptrdiff_t pending_live_selection_item_index_shift_ = 0;
  TranscriptPositionIndicatorState transcript_position_indicator_;
  FrameScheduler frame_scheduler_;
  std::optional<DeferredDetachedViewport> deferred_detached_viewport_ = std::nullopt;
};

}  // namespace ava::tui
