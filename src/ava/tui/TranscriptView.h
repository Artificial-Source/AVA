#pragma once

#include "DeferredDetachedViewport.h"
#include "TranscriptPositionIndicatorState.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/TranscriptUpdate.h"
#include "ava/tui/runtime_transcript_selection_internal.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>

namespace ava::tui {

class RuntimePresentationState;

enum class TranscriptSelectionProjection
{
  Live,
  Frozen,
  Suppressed
};

// TUI-thread viewport/selection owner. Borrowed presentation, sidebar and Composer
// outlive this object. The renderer supplies immutable prepared layout authority;
// the session serializes every update on the TUI thread.
class TranscriptView final
{
 public:
  using Clock = std::chrono::steady_clock;
  TranscriptView(RuntimePresentationState& presentation, Composer& composer);
  TranscriptView(TranscriptView const&) = delete;
  TranscriptView& operator=(TranscriptView const&) = delete;

  [[nodiscard]] std::size_t scroll_offset() const noexcept;
  [[nodiscard]] std::size_t new_output_count() const noexcept;
  [[nodiscard]] std::optional<SidebarSnapshot> const& frozen_sidebar() const noexcept;
  [[nodiscard]] bool has_deferred_update() const noexcept;
  // Request alone leaves detached chrome until paint; local commands clear the
  // count/sidebar immediately; explicit navigation also discards deferred layout.
  void request_live_tail();
  void reset_for_local_command();
  void return_to_live_tail();
  void freeze_sidebar();
  // Maximum comes from prepared layout. These operations never refresh a frozen
  // cache; renderer coordinates the position-indicator paint request separately.
  void scroll_up(std::size_t amount, std::size_t maximum);
  void scroll_down(std::size_t amount, std::size_t maximum);
  void navigate_to_offset(std::size_t offset, std::size_t maximum);
  void restore_layout_anchor(detail::TranscriptViewportAnchor const& anchor, detail::TranscriptLayoutCache const& layout, std::size_t maximum,
                             std::ptrdiff_t item_index_shift);
  // The token retains the detached decision from before transcript/search mutation.
  // Deferred output updates only scroll projection until synchronization or paint.
  void complete_transcript_update(TranscriptUpdate const& update, std::ptrdiff_t item_index_shift, std::size_t event_count);
  void defer_update(detail::TranscriptViewportAnchor anchor, std::ptrdiff_t item_index_shift);
  void synchronize_deferred(detail::TranscriptLayoutCache const& layout, std::size_t maximum);

  void begin_frame();
  void publish_chrome();
  void clamp_for_frame(std::size_t maximum);
  void finish_frame_viewport();
  void clear_selection();
  void cancel_pointer_interaction();
  void note_live_selection_item_shift(std::ptrdiff_t item_index_shift) noexcept;
  [[nodiscard]] bool has_selection() const noexcept;
  [[nodiscard]] bool prepare_selection_authority(detail::TranscriptLayoutCache const& layout);
  void publish_selection(detail::TranscriptLayoutCache const& layout, TranscriptSelectionProjection projection);
  [[nodiscard]] TranscriptSelectionMouseResult handle_mouse(InputEvent const& event, detail::TranscriptLayoutCache const& layout,
                                                            std::function<bool(std::size_t)> const& toggle_tool,
                                                            std::function<bool(std::size_t)> const& toggle_thinking);
  [[nodiscard]] bool copy_selection(detail::TranscriptLayoutCache const& layout);
  [[nodiscard]] bool service_autoscroll(detail::TranscriptLayoutCache const& layout, bool authority_ready, Clock::time_point now);
  void show_position_indicator(Clock::time_point now);
  [[nodiscard]] bool position_indicator_visible() const noexcept;
  [[nodiscard]] bool expire_position_indicator(Clock::time_point now);
  // Absolute semantic deadlines let the renderer register callbacks without
  // choosing a wait duration or borrowing mutable selection/indicator state.
  [[nodiscard]] std::optional<Clock::time_point> indicator_deadline() const noexcept;
  [[nodiscard]] std::optional<Clock::time_point> autoscroll_deadline() const noexcept;
  void reset_for_session_transition();

  // Selection authority and frozen sidebar retain arbitrary message/path data.
  // Generated printing must not expose those content-bearing aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Internal TUI-thread selection steps; public feature operations preserve ordering.
  void discard_deferred_update();
  [[nodiscard]] bool has_pointer_interaction() const noexcept;
  void settle_scroll_change();
  RuntimePresentationState& presentation_;
  Composer& composer_;
  std::size_t scroll_offset_ = 0;
  std::size_t new_output_count_ = 0;
  std::optional<SidebarSnapshot> frozen_sidebar_;
  RuntimeTranscriptSelectionState selection_;
  std::ptrdiff_t pending_live_selection_item_index_shift_ = 0;
  TranscriptPositionIndicatorState position_indicator_;
  std::optional<DeferredDetachedViewport> deferred_viewport_;
};

} // namespace ava::tui
