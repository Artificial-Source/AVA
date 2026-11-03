#include "sys.h"
#include "TranscriptView.h"

#include <algorithm>

namespace ava::tui {

void TranscriptPositionIndicatorState::show(Clock::time_point now) noexcept
{
  expires_at_ = now + kVisibleDuration;
}
void TranscriptPositionIndicatorState::hide() noexcept
{
  expires_at_.reset();
}
bool TranscriptPositionIndicatorState::visible() const noexcept
{
  return expires_at_.has_value();
}
bool TranscriptPositionIndicatorState::expire_if_due(Clock::time_point now) noexcept
{
  if (!expires_at_ || now < *expires_at_)
    return false;
  expires_at_.reset();
  return true;
}
std::optional<TranscriptPositionIndicatorState::Clock::time_point> TranscriptPositionIndicatorState::expires_at() const noexcept
{
  return expires_at_;
}

TranscriptView::TranscriptView(ComposerSnapshot& snapshot, SidebarSnapshot const& sidebar, Composer& composer)
    : snapshot_(snapshot), sidebar_(sidebar), composer_(composer)
{
}
std::size_t TranscriptView::scroll_offset() const noexcept
{
  return scroll_offset_;
}
std::size_t TranscriptView::new_output_count() const noexcept
{
  return new_output_count_;
}
std::optional<SidebarSnapshot> const& TranscriptView::frozen_sidebar() const noexcept
{
  return frozen_sidebar_;
}
bool TranscriptView::has_deferred_update() const noexcept
{
  return deferred_viewport_.has_value();
}
void TranscriptView::request_live_tail()
{
  scroll_offset_ = 0;
}
void TranscriptView::reset_for_local_command()
{
  scroll_offset_ = 0;
  new_output_count_ = 0;
  frozen_sidebar_.reset();
}
void TranscriptView::return_to_live_tail()
{
  reset_for_local_command();
  discard_deferred_update();
}
void TranscriptView::freeze_sidebar()
{
  if (scroll_offset_ > 0 && !frozen_sidebar_)
    frozen_sidebar_ = sidebar_;
}
void TranscriptView::settle_scroll_change()
{
  if (scroll_offset_ > 0)
    freeze_sidebar();
  else
  {
    new_output_count_ = 0;
    frozen_sidebar_.reset();
  }
}
void TranscriptView::scroll_up(std::size_t amount, std::size_t maximum)
{
  auto const clamped = std::min(scroll_offset_, maximum);
  scroll_offset_ = std::min(maximum, clamped + amount);
  freeze_sidebar();
}
void TranscriptView::scroll_down(std::size_t amount, std::size_t maximum)
{
  auto const clamped = std::min(scroll_offset_, maximum);
  scroll_offset_ = amount >= clamped ? 0 : clamped - amount;
  if (scroll_offset_ == 0)
  {
    new_output_count_ = 0;
    frozen_sidebar_.reset();
  }
}
void TranscriptView::navigate_to_offset(std::size_t offset, std::size_t maximum)
{
  scroll_offset_ = std::min(offset, maximum);
  discard_deferred_update();
  settle_scroll_change();
}
void TranscriptView::restore_layout_anchor(detail::TranscriptViewportAnchor const& anchor, detail::TranscriptLayoutCache const& layout, std::size_t maximum,
                                           std::ptrdiff_t item_index_shift)
{
  scroll_offset_ = detail::restore_transcript_viewport_anchor(anchor, layout.layout, maximum, item_index_shift);
}
void TranscriptView::defer_update(detail::TranscriptViewportAnchor anchor, std::ptrdiff_t item_index_shift)
{
  if (scroll_offset_ == 0)
    return;
  if (!deferred_viewport_)
  {
    deferred_viewport_ = DeferredDetachedViewport{.anchor = anchor, .item_index_shift = item_index_shift};
    return;
  }
  deferred_viewport_->item_index_shift += item_index_shift;
}
void TranscriptView::complete_transcript_update(TranscriptUpdate const& update, std::ptrdiff_t item_index_shift, std::size_t event_count)
{
  if (update.detached())
  {
    defer_update(update.anchor(), item_index_shift);
    new_output_count_ += event_count;
    // Keep new-output projection lag until the existing paint/synchronization
    // boundary; UpdatedNoRender relies on the last painted chrome authority.
    snapshot_.transcript_scroll_offset = scroll_offset_;
  }
  else
  {
    note_live_selection_item_shift(item_index_shift);
    discard_deferred_update();
    scroll_offset_ = 0;
  }
}
void TranscriptView::discard_deferred_update()
{
  deferred_viewport_.reset();
}
void TranscriptView::publish_chrome()
{
  snapshot_.transcript_scroll_offset = scroll_offset_;
  snapshot_.transcript_new_output_count = scroll_offset_ > 0 ? new_output_count_ : 0;
}
void TranscriptView::synchronize_deferred(detail::TranscriptLayoutCache const& layout, std::size_t maximum)
{
  if (!deferred_viewport_)
    return;
  if (scroll_offset_ == 0)
  {
    deferred_viewport_.reset();
    new_output_count_ = 0;
    snapshot_.transcript_new_output_count = 0;
    return;
  }
  auto const deferred = *deferred_viewport_;
  restore_layout_anchor(deferred.anchor, layout, maximum, deferred.item_index_shift);
  if (scroll_offset_ == 0)
    new_output_count_ = 0;
  publish_chrome();
  selection_.apply_item_index_shift(deferred.item_index_shift, layout.layout);
  static_cast<void>(selection_.ensure_authority(layout, &snapshot_));
  deferred_viewport_.reset();
}
void TranscriptView::begin_frame()
{
  if (scroll_offset_ == 0)
    return_to_live_tail();
  publish_chrome();
  snapshot_.sidebar = scroll_offset_ > 0 && frozen_sidebar_ ? *frozen_sidebar_ : sidebar_;
}
void TranscriptView::clamp_for_frame(std::size_t maximum)
{
  scroll_offset_ = std::min(scroll_offset_, maximum);
}
void TranscriptView::finish_frame_viewport()
{
  if (scroll_offset_ == 0)
  {
    new_output_count_ = 0;
    frozen_sidebar_.reset();
    snapshot_.sidebar = sidebar_;
  }
  publish_chrome();
}
void TranscriptView::clear_selection()
{
  selection_.clear();
  pending_live_selection_item_index_shift_ = 0;
  selection_.publish(snapshot_);
}
void TranscriptView::cancel_pointer_interaction()
{
  selection_.cancel_pointer_interaction();
  composer_.end_pointer_selection();
  selection_.publish(snapshot_);
}
void TranscriptView::note_live_selection_item_shift(std::ptrdiff_t item_index_shift) noexcept
{
  // HeaderArmed has no committed range but still tracks a live item index.
  if (!selection_.empty() || selection_.dragging() || selection_.has_click_chain())
    pending_live_selection_item_index_shift_ += item_index_shift;
}
bool TranscriptView::has_selection() const noexcept
{
  return !selection_.empty();
}
bool TranscriptView::has_pointer_interaction() const noexcept
{
  return selection_.dragging() || composer_.mouse_selecting();
}
std::optional<TranscriptSelectionRange> TranscriptView::selection_range() const noexcept
{
  return selection_.range();
}
bool TranscriptView::prepare_selection_authority(detail::TranscriptLayoutCache const& layout)
{
  if (!deferred_viewport_ && pending_live_selection_item_index_shift_ != 0)
  {
    selection_.apply_item_index_shift(pending_live_selection_item_index_shift_, layout.layout);
    pending_live_selection_item_index_shift_ = 0;
  }
  return selection_.ensure_authority(layout, &snapshot_);
}
void TranscriptView::publish_selection(detail::TranscriptLayoutCache const& layout, TranscriptSelectionProjection projection)
{
  if (projection == TranscriptSelectionProjection::Suppressed)
  {
    selection_.clear();
    pending_live_selection_item_index_shift_ = 0;
  }
  if (!selection_.empty())
  {
    if (projection == TranscriptSelectionProjection::Live && pending_live_selection_item_index_shift_ != 0)
    {
      selection_.apply_item_index_shift(pending_live_selection_item_index_shift_, layout.layout);
      pending_live_selection_item_index_shift_ = 0;
    }
    static_cast<void>(selection_.ensure_authority(layout, &snapshot_));
  }
  selection_.publish(snapshot_);
}
TranscriptSelectionMouseResult TranscriptView::handle_mouse(InputEvent const& event, detail::TranscriptLayoutCache const& layout,
                                                            std::function<bool(std::size_t)> const& toggle_tool,
                                                            std::function<bool(std::size_t)> const& toggle_thinking)
{
  if (event.key == terminal::Key::MousePointerCancel)
  {
    auto const had_interaction = has_pointer_interaction();
    cancel_pointer_interaction();
    return had_interaction ? TranscriptSelectionMouseResult::HandledNeedsRender : TranscriptSelectionMouseResult::Ignored;
  }
  auto const shift = deferred_viewport_ ? deferred_viewport_->item_index_shift : std::ptrdiff_t{0};
  auto const before = scroll_offset_;
  auto const result = selection_.handle_mouse(event, snapshot_, layout, &composer_, scroll_offset_, shift, toggle_tool, toggle_thinking);
  if (scroll_offset_ != before)
  {
    show_position_indicator(Clock::now());
    settle_scroll_change();
  }
  return result;
}
bool TranscriptView::copy_selection(detail::TranscriptLayoutCache const& layout)
{
  return selection_.copy_selection(snapshot_, layout);
}
bool TranscriptView::service_autoscroll(detail::TranscriptLayoutCache const& layout, bool authority_ready, Clock::time_point now)
{
  auto const before = scroll_offset_;
  auto result = TranscriptSelectionMouseResult::HandledNeedsRender;
  if (authority_ready)
  {
    auto const shift = deferred_viewport_ ? deferred_viewport_->item_index_shift : std::ptrdiff_t{0};
    result = selection_.tick_edge_autoscroll(snapshot_, layout, scroll_offset_, shift, now);
  }
  if (scroll_offset_ != before)
  {
    show_position_indicator(now);
    settle_scroll_change();
  }
  return result == TranscriptSelectionMouseResult::HandledNeedsRender;
}
void TranscriptView::show_position_indicator(Clock::time_point now)
{
  position_indicator_.show(now);
  snapshot_.transcript_position_indicator_visible = true;
}
bool TranscriptView::position_indicator_visible() const noexcept
{
  return position_indicator_.visible();
}
bool TranscriptView::expire_position_indicator(Clock::time_point now)
{
  if (!position_indicator_.expire_if_due(now))
    return false;
  snapshot_.transcript_position_indicator_visible = false;
  return true;
}
std::optional<TranscriptView::Clock::time_point> TranscriptView::indicator_deadline() const noexcept
{
  return position_indicator_.expires_at();
}
std::optional<TranscriptView::Clock::time_point> TranscriptView::autoscroll_deadline() const noexcept
{
  return selection_.edge_autoscroll_deadline();
}
void TranscriptView::reset_for_session_transition()
{
  clear_selection();
  return_to_live_tail();
  position_indicator_.hide();
  snapshot_.transcript_scroll_offset = 0;
  snapshot_.transcript_new_output_count = 0;
  snapshot_.transcript_position_indicator_visible = false;
}

} // namespace ava::tui
