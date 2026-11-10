// Owns viewport and selection transitions beneath RuntimeRenderer; callers outside the renderer read a const view.
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

// TUI-thread paint policy selected by RuntimeRenderer after checking modal visibility and layout compatibility.
enum class TranscriptSelectionProjection
{
  // Apply pending eviction shifts against current layout before publishing selection.
  Live,
  // Keep endpoints in the previously painted detached layout until synchronization.
  Frozen,
  // Clear selection because a modal or competing surface owns the visible input area.
  Suppressed
};

// RuntimeRenderer owns this TUI-thread viewport/selection state and exposes it only
// as a const view to feature callers. Borrowed presentation and Composer outlive it;
// detached sidebar and anchors are owned copies. The renderer supplies layout rows
// and retires timer callbacks before destruction. All transitions run serially on
// the TUI thread; selection coordinates refer to the supplied layout, which may be
// prepared before the next paint.
class TranscriptView final
{
 public:
  // Use the renderer's monotonic timer clock for indicator and selection deadlines on the TUI thread.
  using Clock = std::chrono::steady_clock;
  // TUI thread: bind session-owned projection and Composer, both of which must remain alive through this view's
  // destruction.
  TranscriptView(RuntimePresentationState& presentation, Composer& composer);
  // Not copyable: one renderer owns the selection and pending viewport updates.
  TranscriptView(TranscriptView const&) = delete;
  TranscriptView& operator=(TranscriptView const&) = delete;

  // TUI thread: report distance from live tail; zero requests live following, but sidebar/count cleanup may await the
  // next frame.
  [[nodiscard]] std::size_t scroll_offset() const noexcept;
  // TUI thread: tell the renderer to preserve painted layout or synchronize before geometry-dependent
  // navigation.
  [[nodiscard]] bool has_deferred_update() const noexcept;
  // TUI thread: request tail following; keep the detached count/sidebar until begin_frame clears them.
  void request_live_tail();
  // TUI thread: clear detached count/sidebar immediately for local feedback while retaining pending layout
  // work.
  void reset_for_local_command();
  // TUI thread: explicit navigation clears detached count/sidebar and pending anchors together; the next layout may
  // follow live output.
  void return_to_live_tail();
  // TUI thread: capture live sidebar once on entering detached history so streaming activity does not change
  // its context.
  void freeze_sidebar();
  // TUI thread: move toward older rows within the renderer-prepared maximum and freeze the sidebar on
  // detachment. Scroll operations leave the layout unchanged; the renderer requests indicator painting.
  void scroll_up(std::size_t amount, std::size_t maximum);
  // TUI thread: use the renderer-prepared maximum; reaching zero clears count/sidebar without refreshing layout
  // here.
  void scroll_down(std::size_t amount, std::size_t maximum);
  // TUI thread: clamp explicit navigation, discard obsolete deferred anchors, and clear or freeze the
  // sidebar/count according to the resulting offset.
  void navigate_to_offset(std::size_t offset, std::size_t maximum);
  // TUI thread: restore against the newly prepared layout using the matching item-index shift; caller updates the
  // displayed scroll position and output count afterward.
  void restore_layout_anchor(detail::TranscriptViewportAnchor const& anchor, detail::TranscriptLayoutCache const& layout, std::size_t maximum,
                             std::ptrdiff_t item_index_shift);
  // TUI thread after transcript/search changes: use the token's pre-mutation detached state. Detached output
  // retains the first anchor and updates only screen scroll data until synchronization/paint; live output
  // queues selection index adjustments.
  void complete_transcript_update(TranscriptUpdate const& update, std::ptrdiff_t item_index_shift, std::size_t event_count);
  // TUI thread: retain only the first detached anchor and accumulate later eviction shifts until
  // synchronization.
  void defer_update(detail::TranscriptViewportAnchor anchor, std::ptrdiff_t item_index_shift);
  // TUI thread: use the renderer's current layout to restore viewport and selection with the same accumulated
  // shift, then retire the anchor.
  void synchronize_deferred(detail::TranscriptLayoutCache const& layout, std::size_t maximum);

  // TUI thread, before geometry: finalize a requested live tail and update scroll/count/sidebar data so reserved rows
  // affect layout correctly.
  void begin_frame();
  // TUI thread: publish scroll and detached count together; live tail displays no new-output badge.
  void publish_chrome();
  // TUI thread: clamp after preparing live layout; finish_frame_viewport performs resulting live-tail cleanup.
  void clamp_for_frame(std::size_t maximum);
  // TUI thread, after clamp/synchronization: release frozen sidebar at live tail and update the frame's final
  // scroll position and output count.
  void finish_frame_viewport();
  // TUI thread: discard range, pointer state, and pending item shifts, then remove the screen highlight.
  void clear_selection();
  // TUI thread: end transcript and Composer drags while preserving any committed transcript range; update the
  // screen highlight.
  void cancel_pointer_interaction();
  // TUI thread, after cap eviction: accumulate shifts only when selection or a click chain still references
  // live item indices.
  void note_live_selection_item_shift(std::ptrdiff_t item_index_shift) noexcept;
  // TUI thread: report whether a range exists so the renderer can prepare its layout before painting a highlight.
  [[nodiscard]] bool has_selection() const noexcept;
  // TUI thread: apply pending live shifts only outside deferred layout, then validate endpoints against the
  // supplied cache.
  [[nodiscard]] bool prepare_selection_authority(detail::TranscriptLayoutCache const& layout);
  // TUI thread, during paint: apply the chosen live/frozen/suppressed policy before projecting highlight
  // endpoints.
  void publish_selection(detail::TranscriptLayoutCache const& layout, TranscriptSelectionProjection projection);
  // TUI thread: consume pointer reports using the supplied layout; toggle callbacks run synchronously and must not
  // destroy this view.
  [[nodiscard]] TranscriptSelectionMouseResult handle_mouse(InputEvent const& event, detail::TranscriptLayoutCache const& layout,
                                                            std::function<bool(std::size_t)> const& toggle_tool,
                                                            std::function<bool(std::size_t)> const& toggle_thinking);
  // TUI thread: copy using the supplied retained layout cache, without rebuilding it. If its generation or
  // display settings no longer match the selection, copy nothing and report failure; otherwise report
  // whether the clipboard request was sent.
  [[nodiscard]] bool copy_selection(detail::TranscriptLayoutCache const& layout);
  // TUI thread timer servicing: when authority_ready confirms usable selection geometry, advance a held
  // edge drag and update scroll/sidebar/count state. Return whether painting is needed.
  [[nodiscard]] bool service_autoscroll(detail::TranscriptLayoutCache const& layout, bool authority_ready, Clock::time_point now);
  // TUI thread: renew the deadline and publish visibility; renderer must reconcile its timer afterward.
  void show_position_indicator(Clock::time_point now);
  // TUI thread: retire a due indicator and report whether its projected visibility changed.
  [[nodiscard]] bool expire_position_indicator(Clock::time_point now);
  // TUI thread: copy the absolute expiry so RuntimeRenderer can arm/cancel its timer without borrowing
  // mutable indicator state.
  [[nodiscard]] std::optional<Clock::time_point> indicator_deadline() const noexcept;
  // TUI thread: copy the next edge-drag deadline; no deadline means no selection wake is needed.
  [[nodiscard]] std::optional<Clock::time_point> autoscroll_deadline() const noexcept;
  // TUI thread before new session projection: clear old selection, anchors, and indicator while borrowed owners
  // remain alive.
  void reset_for_session_transition();

  // Selection source copies and frozen sidebar retain arbitrary message/path data.
  // Generated printing must not expose those content-bearing aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread: release only the pending anchor; callers separately choose whether detached count/sidebar
  // should remain.
  void discard_deferred_update();
  // TUI thread: include draft drags so a pointer-cancel report can request a repaint even without transcript
  // selection.
  [[nodiscard]] bool has_pointer_interaction() const noexcept;
  // TUI thread after pointer navigation: freeze sidebar when detached, or release detached count/sidebar on
  // reaching live tail.
  void settle_scroll_change();
  // Borrowed session-owned projection; only this TUI-thread owner publishes viewport/selection fields, and
  // presentation outlives it.
  RuntimePresentationState& presentation_;
  // Borrowed editor outlives this view; TUI-thread pointer transitions end the competing draft selection.
  Composer& composer_;
  // TUI-thread distance from the live tail, changed by navigation/restoration; zero enables following newly
  // appended output.
  std::size_t scroll_offset_ = 0;
  // TUI-thread count accumulated by detached updates; live-tail/reset transitions clear it and paint publishes
  // it.
  std::size_t new_output_count_ = 0;
  // Owned sidebar captured on first detachment; preserved across output updates and cleared when following live
  // tail.
  std::optional<SidebarSnapshot> frozen_sidebar_;
  // Owned TUI-thread selection endpoints and source copies; never shared with producers and destroyed without callbacks.
  RuntimeTranscriptSelectionState selection_;
  // TUI-thread item-index adjustment since selection was last updated: removing N front items adds -N.
  // Apply it before using live selection indices; detached selection uses its deferred anchor's shift.
  std::ptrdiff_t pending_live_selection_item_index_shift_ = 0;
  // Owned TUI-thread deadline value; renderer owns the timer and retires it before this view dies.
  TranscriptPositionIndicatorState position_indicator_;
  // Owned first detached anchor and cumulative eviction shift; reset only when synchronized or explicitly
  // abandoned.
  std::optional<DeferredDetachedViewport> deferred_viewport_;
};

} // namespace ava::tui
