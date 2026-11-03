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
// detached sidebar and anchors are owned copies. Renderer supplies the prepared/drawn
// layout and retires timer callbacks before destruction. Session ordering serializes
// every transition, and selection uses the same layout authority as the visible rows.
class TranscriptView final
{
 public:
  // Use the renderer's monotonic timer clock for indicator and selection deadlines on the TUI thread.
  using Clock = std::chrono::steady_clock;
  // TUI thread: bind session-owned projection and Composer, both of which must remain alive through this view's
  // destruction.
  TranscriptView(RuntimePresentationState& presentation, Composer& composer);
  // A viewport has one renderer owner; copying would split selection and deferred-update authority.
  TranscriptView(TranscriptView const&) = delete;
  // Preserve the original presentation/Composer lifetime bindings and single viewport authority.
  TranscriptView& operator=(TranscriptView const&) = delete;

  // TUI thread: report distance from live tail; zero requests live following but detached chrome may await the
  // next frame.
  [[nodiscard]] std::size_t scroll_offset() const noexcept;
  // TUI thread: read accumulated detached output; the painted count may lag until chrome publication.
  [[nodiscard]] std::size_t new_output_count() const noexcept;
  // TUI thread: borrow the captured sidebar until a live-tail transition/reset clears it; never retain nested
  // content across mutation.
  [[nodiscard]] std::optional<SidebarSnapshot> const& frozen_sidebar() const noexcept;
  // TUI thread: tell the renderer to preserve painted layout or synchronize before geometry-dependent
  // navigation.
  [[nodiscard]] bool has_deferred_update() const noexcept;
  // Request alone leaves detached chrome until paint; local commands clear the
  // count/sidebar immediately; explicit navigation also discards deferred layout.
  // TUI thread: request tail following without immediately clearing detached count/sidebar; begin_frame commits
  // that cleanup.
  void request_live_tail();
  // TUI thread: clear detached count/sidebar immediately for local feedback while retaining pending layout
  // work.
  void reset_for_local_command();
  // TUI thread: explicit navigation clears detached chrome and pending anchors together; the next layout may
  // follow live output.
  void return_to_live_tail();
  // TUI thread: capture live sidebar once on entering detached history so streaming activity does not change
  // its context.
  void freeze_sidebar();
  // Maximum comes from prepared layout. These operations never refresh a frozen
  // cache; renderer coordinates the position-indicator paint request separately.
  // TUI thread: use the renderer-prepared maximum, move toward older rows, and freeze sidebar on detachment.
  void scroll_up(std::size_t amount, std::size_t maximum);
  // TUI thread: use the renderer-prepared maximum; reaching zero clears count/sidebar without refreshing layout
  // here.
  void scroll_down(std::size_t amount, std::size_t maximum);
  // TUI thread: clamp explicit navigation, discard obsolete deferred anchors, and settle sidebar/count
  // ownership.
  void navigate_to_offset(std::size_t offset, std::size_t maximum);
  // TUI thread: restore against the newly prepared layout using the matching eviction shift; caller publishes
  // chrome afterward.
  void restore_layout_anchor(detail::TranscriptViewportAnchor const& anchor, detail::TranscriptLayoutCache const& layout, std::size_t maximum,
                             std::ptrdiff_t item_index_shift);
  // The token retains the detached decision from before transcript/search mutation.
  // Deferred output updates only scroll projection until synchronization or paint.
  // TUI thread: consume the pre-mutation token after transcript/search publication; detached output defers
  // geometry, live output queues selection shifts.
  void complete_transcript_update(TranscriptUpdate const& update, std::ptrdiff_t item_index_shift, std::size_t event_count);
  // TUI thread: retain only the first detached anchor and accumulate later eviction shifts until
  // synchronization.
  void defer_update(detail::TranscriptViewportAnchor anchor, std::ptrdiff_t item_index_shift);
  // TUI thread: use the renderer's current layout to restore viewport and selection with the same accumulated
  // shift, then retire the anchor.
  void synchronize_deferred(detail::TranscriptLayoutCache const& layout, std::size_t maximum);

  // TUI thread, before geometry: finalize a requested live tail and publish chrome/sidebar so reserved rows
  // affect layout correctly.
  void begin_frame();
  // TUI thread: publish scroll and detached count together; live tail displays no new-output badge.
  void publish_chrome();
  // TUI thread: clamp after preparing live layout; finish_frame_viewport performs resulting live-tail cleanup.
  void clamp_for_frame(std::size_t maximum);
  // TUI thread, after clamp/synchronization: release frozen sidebar at live tail and publish final frame
  // chrome.
  void finish_frame_viewport();
  // TUI thread: discard range, pointer state, and pending item shifts, then remove the projected highlight.
  void clear_selection();
  // TUI thread: end transcript and Composer drags while preserving any committed transcript range; publish the
  // settled highlight.
  void cancel_pointer_interaction();
  // TUI thread, after cap eviction: accumulate shifts only when selection or a click chain still references
  // live item indices.
  void note_live_selection_item_shift(std::ptrdiff_t item_index_shift) noexcept;
  // TUI thread: let the renderer decide whether layout authority must be refreshed before painting a highlight.
  [[nodiscard]] bool has_selection() const noexcept;
  // TUI thread: apply pending live shifts only outside deferred layout, then validate endpoints against the
  // supplied cache.
  [[nodiscard]] bool prepare_selection_authority(detail::TranscriptLayoutCache const& layout);
  // TUI thread, during paint: apply the chosen live/frozen/suppressed policy before projecting highlight
  // endpoints.
  void publish_selection(detail::TranscriptLayoutCache const& layout, TranscriptSelectionProjection projection);
  // TUI thread: consume pointer reports using drawn layout; toggle callbacks run synchronously and must not
  // destroy this view.
  [[nodiscard]] TranscriptSelectionMouseResult handle_mouse(InputEvent const& event, detail::TranscriptLayoutCache const& layout,
                                                            std::function<bool(std::size_t)> const& toggle_tool,
                                                            std::function<bool(std::size_t)> const& toggle_thinking);
  // TUI thread: copy from supplied painted authority and publish success/failure feedback; invalid authority
  // fails closed.
  [[nodiscard]] bool copy_selection(detail::TranscriptLayoutCache const& layout);
  // TUI thread timer servicing: advance a held edge drag only with valid authority, settle chrome, and report
  // whether painting is needed.
  [[nodiscard]] bool service_autoscroll(detail::TranscriptLayoutCache const& layout, bool authority_ready, Clock::time_point now);
  // TUI thread: renew the deadline and publish visibility; renderer must reconcile its timer afterward.
  void show_position_indicator(Clock::time_point now);
  // TUI thread: report indicator intent; expiry is applied explicitly by its renderer timer callback.
  [[nodiscard]] bool position_indicator_visible() const noexcept;
  // TUI thread: retire a due indicator and report whether its projected visibility changed.
  [[nodiscard]] bool expire_position_indicator(Clock::time_point now);
  // Absolute semantic deadlines let the renderer register callbacks without
  // choosing a wait duration or borrowing mutable selection/indicator state.
  // TUI thread: copy the absolute deadline so RuntimeRenderer can arm/cancel without accessing mutable
  // indicator state.
  [[nodiscard]] std::optional<Clock::time_point> indicator_deadline() const noexcept;
  // TUI thread: copy the next edge-drag deadline; no deadline means no selection wake is needed.
  [[nodiscard]] std::optional<Clock::time_point> autoscroll_deadline() const noexcept;
  // TUI thread before new session projection: clear old selection, anchors, and indicator while borrowed owners
  // remain alive.
  void reset_for_session_transition();

  // Selection authority and frozen sidebar retain arbitrary message/path data.
  // Generated printing must not expose those content-bearing aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Internal TUI-thread selection steps; public feature operations preserve ordering.
  // TUI thread: release only the pending anchor; callers separately choose whether detached chrome should
  // remain.
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
  // Owned TUI-thread selection authority; never shared with producers and destroyed without callbacks.
  RuntimeTranscriptSelectionState selection_;
  // TUI-thread eviction debt applied before rebinding live selection; deferred selection uses its anchor's
  // shift instead.
  std::ptrdiff_t pending_live_selection_item_index_shift_ = 0;
  // Owned TUI-thread deadline value; renderer owns the timer registration and retires it before this view dies.
  TranscriptPositionIndicatorState position_indicator_;
  // Owned first detached anchor and cumulative eviction shift; reset only when synchronized or explicitly
  // abandoned.
  std::optional<DeferredDetachedViewport> deferred_viewport_;
};

} // namespace ava::tui
