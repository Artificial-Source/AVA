#include "sys.h"
#include "ava/tui/command_output.h"
#include "ava/tui/mermaid_projection.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/terminal.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <string>
#include <curses.h>

namespace ava::tui {
using Signals = core::Signals;

std::pair<std::size_t, std::size_t> terminal_size()
{
  int height = 0;
  int width = 0;
  getmaxyx(stdscr, height, width);
  if (width > 0 && height > 0)
    return {static_cast<std::size_t>(width), static_cast<std::size_t>(height)};
  return {80, 24};
}

bool WheelBurstGovernor::accept(WheelDirection direction, Clock::time_point now)
{
  if (last_accepted_at_ && last_accepted_direction_ && *last_accepted_direction_ == direction && now < *last_accepted_at_ + kAcceptedEventInterval)
    return false;
  last_accepted_at_ = now;
  last_accepted_direction_ = direction;
  return true;
}

void WheelBurstGovernor::reset()
{
  last_accepted_at_.reset();
  last_accepted_direction_.reset();
}

bool runtime_wheel_input_accepted(WheelBurstGovernor& governor, Key key, WheelBurstGovernor::Clock::time_point now)
{
  if (key == Key::MouseWheelUp)
    return governor.accept(WheelDirection::Up, now);
  if (key == Key::MouseWheelDown)
    return governor.accept(WheelDirection::Down, now);
  governor.reset();
  return true;
}

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

std::optional<TranscriptPositionIndicatorState::Clock::duration> TranscriptPositionIndicatorState::time_until_expiry(Clock::time_point now) const noexcept
{
  if (!expires_at_)
    return std::nullopt;
  return now >= *expires_at_ ? Clock::duration::zero() : *expires_at_ - now;
}

void FrameScheduler::request(FrameRenderKind kind, Clock::time_point now)
{
  if (failed_)
    return;
  if (!pending_kind_)
  {
    pending_kind_ = kind;
    deadline_ = last_paint_completed_.value_or(now) + kFrameInterval;
    return;
  }
  if (kind == FrameRenderKind::Full)
    pending_kind_ = FrameRenderKind::Full;
}

bool FrameScheduler::pending() const
{
  return pending_kind_.has_value();
}

std::optional<FrameRenderKind> FrameScheduler::pending_kind() const
{
  return pending_kind_;
}

FrameScheduler::Clock::duration FrameScheduler::time_until_due(Clock::time_point now) const
{
  if (!deadline_ || now >= *deadline_)
    return Clock::duration::zero();
  return *deadline_ - now;
}

std::optional<FrameRenderKind> FrameScheduler::take_due(Clock::time_point now)
{
  if (!pending_kind_ || time_until_due(now) > Clock::duration::zero())
    return std::nullopt;
  return take_pending();
}

std::optional<FrameRenderKind> FrameScheduler::take_pending()
{
  auto kind = pending_kind_;
  pending_kind_.reset();
  deadline_.reset();
  return kind;
}

void FrameScheduler::paint_completed(Clock::time_point completed_at, bool succeeded)
{
  last_paint_completed_ = completed_at;
  if (!succeeded)
    failed_ = true;
}

void FrameScheduler::discard_pending()
{
  pending_kind_.reset();
  deadline_.reset();
}

bool FrameScheduler::failed() const
{
  return failed_;
}

RuntimeRenderer::RuntimeRenderer(ComposerSnapshot& snapshot, SidebarSnapshot& sidebar, RuntimeDraftState& draft_state)
    : snapshot_(snapshot), sidebar_(sidebar), draft_state_(draft_state)
{
}

void RuntimeRenderer::defer_detached_transcript_update(detail::TranscriptViewportAnchor anchor, std::ptrdiff_t item_index_shift)
{
  if (transcript_scroll_offset == 0)
    return;
  if (!deferred_detached_viewport_)
  {
    deferred_detached_viewport_ = DeferredDetachedViewport{.anchor = anchor, .item_index_shift = item_index_shift};
    return;
  }
  deferred_detached_viewport_->item_index_shift += item_index_shift;
}

void RuntimeRenderer::synchronize_detached_transcript_layout()
{
  // Navigation max-scroll call sites invoke this before geometry math. Publish renderer
  // chrome authority first so Wave A reserved-row height matches the eventual paint.
  snapshot_.transcript_scroll_offset = transcript_scroll_offset;
  snapshot_.transcript_new_output_count = transcript_scroll_offset > 0 ? detached_new_output_count : 0;
  if (!deferred_detached_viewport_)
    return;
  if (transcript_scroll_offset == 0)
  {
    deferred_detached_viewport_.reset();
    detached_new_output_count = 0;
    snapshot_.transcript_new_output_count = 0;
    return;
  }
  auto deferred = *deferred_detached_viewport_;
  auto const max_scroll =
      detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, completion_cache, snapshot_.file_references_generation,
                                                           transcript_layout_cache, snapshot_.transcript_generation);
  transcript_scroll_offset = detail::restore_transcript_viewport_anchor(deferred.anchor, transcript_layout_cache.layout, max_scroll, deferred.item_index_shift);
  if (transcript_scroll_offset == 0)
    detached_new_output_count = 0;
  snapshot_.transcript_scroll_offset = transcript_scroll_offset;
  snapshot_.transcript_new_output_count = transcript_scroll_offset > 0 ? detached_new_output_count : 0;
  transcript_selection_.apply_item_index_shift(deferred.item_index_shift, transcript_layout_cache.layout);
  static_cast<void>(transcript_selection_.ensure_authority(transcript_layout_cache, &snapshot_));
  deferred_detached_viewport_.reset();
}

void RuntimeRenderer::discard_deferred_detached_transcript_update()
{
  deferred_detached_viewport_.reset();
}

bool RuntimeRenderer::has_deferred_detached_transcript_update() const
{
  return deferred_detached_viewport_.has_value();
}

bool RuntimeRenderer::prepare_transcript_selection_authority()
{
  // A detached deferred viewport is deliberately frozen on the cache that was
  // drawn. Never refresh it from the live transcript here.
  if (!deferred_detached_viewport_)
  {
    auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot_.height);
    auto const width = composer_canvas_layout(snapshot_).content_width;
    auto const compact = detail::composer_layout_policy(snapshot_, height).compact_transcript_spacing;
    detail::refresh_transcript_layout_cache(transcript_layout_cache, snapshot_.transcript, snapshot_.transcript_generation, width, snapshot_.tool_presentation,
                                            snapshot_.thinking_visible, compact, detail::active_mermaid_projection(snapshot_));
    if (pending_live_selection_item_index_shift_ != 0)
    {
      transcript_selection_.apply_item_index_shift(pending_live_selection_item_index_shift_, transcript_layout_cache.layout);
      pending_live_selection_item_index_shift_ = 0;
    }
  }
  return transcript_selection_.ensure_authority(transcript_layout_cache, &snapshot_);
}

TranscriptSelectionMouseResult RuntimeRenderer::handle_transcript_selection_mouse(InputEvent const& event, std::function<bool(std::size_t)> const& toggle_tool,
                                                                                  std::function<bool(std::size_t)> const& toggle_thinking)
{
  std::lock_guard<std::recursive_mutex> lock(ui_mutex);
  if (event.key == Key::MousePointerCancel)
  {
    auto const had_interaction = transcript_selection_.dragging() || draft_state_.mouse_selecting;
    transcript_selection_.cancel_pointer_interaction();
    draft_state_.mouse_selecting = false;
    transcript_selection_.publish(snapshot_);
    return had_interaction ? TranscriptSelectionMouseResult::HandledNeedsRender : TranscriptSelectionMouseResult::Ignored;
  }
  // Frozen detached layouts keep geometry authority. Header toggles and live snapshot
  // lookups map through the exact accumulated deferred item_index_shift; body drag does
  // not force a live rebuild here.
  auto const frozen_to_live_item_index_shift = deferred_detached_viewport_ ? deferred_detached_viewport_->item_index_shift : std::ptrdiff_t{0};
  if (!prepare_transcript_selection_authority())
    return TranscriptSelectionMouseResult::Ignored;
  auto const scroll_before = transcript_scroll_offset;
  auto const result = transcript_selection_.handle_mouse(event, snapshot_, transcript_layout_cache, &draft_state_, transcript_scroll_offset,
                                                         frozen_to_live_item_index_shift, toggle_tool, toggle_thinking);
  if (transcript_scroll_offset != scroll_before)
  {
    show_transcript_position_indicator();
    if (transcript_scroll_offset > 0 && !detached_sidebar_snapshot)
      detached_sidebar_snapshot = sidebar_;
    if (transcript_scroll_offset == 0)
    {
      detached_new_output_count = 0;
      detached_sidebar_snapshot.reset();
    }
  }
  return result;
}

void RuntimeRenderer::show_transcript_position_indicator(TranscriptPositionIndicatorState::Clock::time_point now)
{
  std::lock_guard<std::recursive_mutex> lock(ui_mutex);
  transcript_position_indicator_.show(now);
  snapshot_.transcript_position_indicator_visible = true;
  frame_scheduler_.request(FrameRenderKind::Full, now);
}

bool RuntimeRenderer::transcript_position_indicator_visible() const noexcept
{
  return transcript_position_indicator_.visible();
}

bool RuntimeRenderer::copy_transcript_selection()
{
  std::lock_guard<std::recursive_mutex> lock(ui_mutex);
  return transcript_selection_.copy_selection(snapshot_, transcript_layout_cache);
}

void RuntimeRenderer::clear_transcript_selection()
{
  std::lock_guard<std::recursive_mutex> lock(ui_mutex);
  transcript_selection_.clear();
  pending_live_selection_item_index_shift_ = 0;
  transcript_selection_.publish(snapshot_);
}

void RuntimeRenderer::reset_for_session_transition()
{
  std::lock_guard<std::recursive_mutex> lock(ui_mutex);
  transcript_selection_.clear();
  pending_live_selection_item_index_shift_ = 0;
  transcript_selection_.publish(snapshot_);
  transcript_scroll_offset = 0;
  detached_new_output_count = 0;
  detached_sidebar_snapshot.reset();
  deferred_detached_viewport_.reset();
  transcript_layout_cache = {};
  screen_row_cache.valid = false;
  wheel_governor.reset();
  transcript_position_indicator_.hide();
  snapshot_.transcript_scroll_offset = 0;
  snapshot_.transcript_new_output_count = 0;
  snapshot_.transcript_position_indicator_visible = false;
}

void RuntimeRenderer::cancel_pointer_interaction()
{
  std::lock_guard<std::recursive_mutex> lock(ui_mutex);
  transcript_selection_.cancel_pointer_interaction();
  draft_state_.mouse_selecting = false;
  transcript_selection_.publish(snapshot_);
}

void RuntimeRenderer::note_live_transcript_selection_item_shift(std::ptrdiff_t item_index_shift) noexcept
{
  // HeaderArmed has no committed range but still owns a live item index that must track
  // leading eviction between press and release while the layout is not frozen.
  if (!transcript_selection_.empty() || transcript_selection_.dragging() || transcript_selection_.has_click_chain())
    pending_live_selection_item_index_shift_ += item_index_shift;
}

bool RuntimeRenderer::has_transcript_selection() const noexcept
{
  return !transcript_selection_.empty();
}

bool RuntimeRenderer::has_pointer_interaction() const noexcept
{
  return transcript_selection_.dragging() || draft_state_.mouse_selecting;
}

std::optional<TranscriptSelectionRange> RuntimeRenderer::transcript_selection_range() const noexcept
{
  return transcript_selection_.range();
}

void RuntimeRenderer::service_auxiliary_timers(std::chrono::steady_clock::time_point now)
{
  std::lock_guard<std::recursive_mutex> lock(ui_mutex);
  if (auto const due = transcript_selection_.time_until_edge_autoscroll(now); due && *due <= std::chrono::steady_clock::duration::zero())
  {
    auto const scroll_before = transcript_scroll_offset;
    auto result = TranscriptSelectionMouseResult::HandledNeedsRender;
    if (prepare_transcript_selection_authority())
    {
      auto const frozen_to_live_item_index_shift = deferred_detached_viewport_ ? deferred_detached_viewport_->item_index_shift : std::ptrdiff_t{0};
      result = transcript_selection_.tick_edge_autoscroll(snapshot_, transcript_layout_cache, transcript_scroll_offset, frozen_to_live_item_index_shift, now);
    }
    if (transcript_scroll_offset != scroll_before)
    {
      transcript_position_indicator_.show(now);
      snapshot_.transcript_position_indicator_visible = true;
      if (transcript_scroll_offset > 0 && !detached_sidebar_snapshot)
        detached_sidebar_snapshot = sidebar_;
      if (transcript_scroll_offset == 0)
      {
        detached_new_output_count = 0;
        detached_sidebar_snapshot.reset();
      }
    }
    if (result == TranscriptSelectionMouseResult::HandledNeedsRender)
      frame_scheduler_.request(FrameRenderKind::Full, now);
  }
  if (transcript_position_indicator_.expire_if_due(now))
  {
    snapshot_.transcript_position_indicator_visible = false;
    frame_scheduler_.request(FrameRenderKind::Full, now);
  }
}

bool RuntimeRenderer::render()
{
  if (frame_scheduler_.failed())
    return false;
  service_auxiliary_timers(std::chrono::steady_clock::now());
  frame_scheduler_.discard_pending();
  auto const succeeded = render_full(false);
  frame_scheduler_.paint_completed(std::chrono::steady_clock::now(), succeeded);
  return succeeded;
}

bool RuntimeRenderer::render_full(bool freeze_transcript_layout)
{
  auto& snapshot = snapshot_;
  auto& sidebar = sidebar_;
  auto& draft_state = draft_state_;
  auto& draft = draft_state.draft;
  auto& selected_slash_command_index = draft_state.selected_slash_command_index;
  auto& slash_palette_suppressed = draft_state.slash_palette_suppressed;
  auto& path_completion_force_active = draft_state.path_completion_force_active;
  auto& draft_scroll_offset = draft_state.draft_scroll_offset;

  bool wrote = false;
  {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex);
    draft.cursor = clamp_composer_draft_cursor(draft.text, draft.cursor);
    snapshot.input = draft.text;
    snapshot.input_cursor = draft.cursor;
    if (auto const selection = draft_state.selection_bounds())
    {
      snapshot.input_selection_start = selection->first;
      snapshot.input_selection_end = selection->second;
    }
    else
    {
      snapshot.input_selection_start = std::string::npos;
      snapshot.input_selection_end = std::string::npos;
    }
    snapshot.selected_slash_command_index = selected_slash_command_index;
    snapshot.slash_palette_suppressed = slash_palette_suppressed;
    snapshot.path_completion_force_active = path_completion_force_active;
    if (transcript_scroll_offset == 0)
    {
      discard_deferred_detached_transcript_update();
      detached_new_output_count = 0;
      detached_sidebar_snapshot.reset();
    }
    // Publish renderer geometry authority before freeze/sync/clamp so Wave A chrome height
    // (N-new reserved row) participates in max-scroll math. Snapshot lag after detached stream
    // updates otherwise suppresses the row during sync while paint reserves it, shifting the
    // visible numbered window by one.
    snapshot.transcript_scroll_offset = transcript_scroll_offset;
    snapshot.transcript_new_output_count = transcript_scroll_offset > 0 ? detached_new_output_count : 0;
    snapshot.sidebar = transcript_scroll_offset > 0 && detached_sidebar_snapshot ? *detached_sidebar_snapshot : sidebar;
    auto const [width, height] = terminal_size();
    snapshot.width = width;
    snapshot.height = height;
    auto const compact_spacing = detail::composer_layout_policy(snapshot, height).compact_transcript_spacing;
    auto const presentation_settings_compatible = transcript_layout_cache.valid && transcript_layout_cache.tool_presentation == snapshot.tool_presentation &&
                                                  transcript_layout_cache.thinking_visible == snapshot.thinking_visible &&
                                                  transcript_layout_cache.compact_spacing == compact_spacing;
    auto const frozen_cache_compatible = presentation_settings_compatible && transcript_layout_cache.width == composer_canvas_layout(snapshot).content_width;
    auto const freeze_modal_transcript_layout =
        (snapshot.command_output.has_value() || (snapshot.select_list.has_value() && snapshot.select_list->freeze_underlying_transcript_layout) ||
         snapshot.subagent_workspace.has_value()) &&
        presentation_settings_compatible;
    // Detached freeze keeps a width-compatible cache across scheduled paints. Modal freeze keeps the
    // pre-modal underlying layout even when the active selector canvas width differs, and freezes the
    // draw itself whether or not a deferred detached viewport is pending.
    auto const freeze_detached_viewport =
        freeze_modal_transcript_layout || (freeze_transcript_layout && deferred_detached_viewport_.has_value() && frozen_cache_compatible);
    auto const synchronized_detached_viewport = deferred_detached_viewport_.has_value() && !freeze_detached_viewport;
    if (synchronized_detached_viewport)
      synchronize_detached_transcript_layout();
    snapshot.sidebar_drawer_scroll_offset = std::min(snapshot.sidebar_drawer_scroll_offset, sidebar_drawer_max_scroll_offset(snapshot));
    if (snapshot.command_output)
    {
      auto const geometry = command_output_geometry(width, height);
      snapshot.command_output->scroll_offset =
          std::min(snapshot.command_output->scroll_offset,
                   command_output_max_scroll_offset(*snapshot.command_output, geometry.width, geometry.height, snapshot.tool_presentation));
    }
    draft_scroll_offset = std::min(draft_scroll_offset, draft_state.max_draft_scroll_offset(snapshot, height));
    snapshot.draft_scroll_offset = draft_scroll_offset;
    if (transcript_scroll_offset > 0 && !synchronized_detached_viewport && !freeze_detached_viewport)
    {
      transcript_scroll_offset = std::min(transcript_scroll_offset, detail::composer_max_transcript_scroll_offset_cached(
                                                                        snapshot, width, height, completion_cache, snapshot.file_references_generation,
                                                                        transcript_layout_cache, snapshot.transcript_generation));
    }
    if (transcript_scroll_offset == 0)
    {
      detached_new_output_count = 0;
      detached_sidebar_snapshot.reset();
      snapshot.sidebar = sidebar;
    }
    // Re-publish final scroll/count after sync/clamp; live tail clears count truthfully.
    snapshot.transcript_scroll_offset = transcript_scroll_offset;
    snapshot.transcript_new_output_count = transcript_scroll_offset > 0 ? detached_new_output_count : 0;
    detail::refresh_completion_match_cache(completion_cache, snapshot, snapshot.file_references_generation);
    auto const completion_palette_visible = completion_cache.model && completion_cache.model->palette_visible;
    auto const slash_palette_is_visible =
        !snapshot.slash_palette_suppressed && slash_palette_visible(snapshot.input, snapshot.input_cursor, snapshot.slash_commands);
    if (snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output || snapshot.plugin_ui_modal || snapshot.select_list ||
        snapshot.subagent_workspace || snapshot.sidebar_drawer_visible || slash_palette_is_visible || completion_palette_visible)
    {
      transcript_selection_.clear();
      pending_live_selection_item_index_shift_ = 0;
    }
    if (!transcript_selection_.empty())
    {
      if (!freeze_detached_viewport)
      {
        auto const width = composer_canvas_layout(snapshot).content_width;
        auto const compact = detail::composer_layout_policy(snapshot, height).compact_transcript_spacing;
        detail::refresh_transcript_layout_cache(transcript_layout_cache, snapshot.transcript, snapshot.transcript_generation, width, snapshot.tool_presentation,
                                                snapshot.thinking_visible, compact, detail::active_mermaid_projection(snapshot));
      }
      if (!freeze_detached_viewport && pending_live_selection_item_index_shift_ != 0)
      {
        transcript_selection_.apply_item_index_shift(pending_live_selection_item_index_shift_, transcript_layout_cache.layout);
        pending_live_selection_item_index_shift_ = 0;
      }
      static_cast<void>(transcript_selection_.ensure_authority(transcript_layout_cache, &snapshot));
    }
    transcript_selection_.publish(snapshot);
    wrote = detail::draw_screen_cached(snapshot, completion_cache, snapshot.file_references_generation, transcript_layout_cache, snapshot.transcript_generation,
                                       screen_row_cache, freeze_detached_viewport, freeze_modal_transcript_layout);
  }
  return wrote;
}

bool RuntimeRenderer::render_processing_frame()
{
  if (frame_scheduler_.failed())
    return false;
  frame_scheduler_.discard_pending();
  auto const succeeded = paint(FrameRenderKind::Footer, false);
  frame_scheduler_.paint_completed(std::chrono::steady_clock::now(), succeeded);
  return succeeded;
}

bool RuntimeRenderer::request_render(FrameRenderKind kind)
{
  frame_scheduler_.request(kind, std::chrono::steady_clock::now());
  return !frame_scheduler_.failed();
}

bool RuntimeRenderer::flush_pending_render_if_due()
{
  auto const now = std::chrono::steady_clock::now();
  service_auxiliary_timers(now);
  auto const kind = frame_scheduler_.take_due(now);
  if (!kind)
    return !frame_scheduler_.failed();
  auto const succeeded = paint(*kind, true);
  frame_scheduler_.paint_completed(std::chrono::steady_clock::now(), succeeded);
  return succeeded;
}

bool RuntimeRenderer::flush_pending_render()
{
  service_auxiliary_timers(std::chrono::steady_clock::now());
  auto const kind = frame_scheduler_.take_pending();
  if (!kind)
    return !frame_scheduler_.failed();
  auto const succeeded = paint(*kind, true);
  frame_scheduler_.paint_completed(std::chrono::steady_clock::now(), succeeded);
  return succeeded;
}

bool RuntimeRenderer::has_pending_render() const
{
  return frame_scheduler_.pending() || transcript_position_indicator_.visible() || transcript_selection_.time_until_edge_autoscroll().has_value();
}

std::chrono::steady_clock::duration RuntimeRenderer::time_until_pending_render() const
{
  auto const now = std::chrono::steady_clock::now();
  auto wait = std::chrono::steady_clock::duration::max();
  if (frame_scheduler_.pending())
    wait = std::min(wait, frame_scheduler_.time_until_due(now));
  if (auto const indicator_wait = transcript_position_indicator_.time_until_expiry(now))
    wait = std::min(wait, *indicator_wait);
  if (auto const autoscroll_wait = transcript_selection_.time_until_edge_autoscroll(now))
    wait = std::min(wait, *autoscroll_wait);
  return wait == std::chrono::steady_clock::duration::max() ? std::chrono::steady_clock::duration::zero() : wait;
}

bool RuntimeRenderer::render_failed() const
{
  return frame_scheduler_.failed();
}

bool RuntimeRenderer::paint(FrameRenderKind kind, bool freeze_transcript_layout)
{
  if (kind == FrameRenderKind::Full)
    return render_full(freeze_transcript_layout);

  auto& snapshot = snapshot_;
  bool wrote = false;
  {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex);
    if (snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output || snapshot.select_list || snapshot.subagent_workspace ||
        snapshot.sidebar_drawer_visible || !snapshot.processing)
      return true;
    wrote = detail::draw_processing_footer_cached(snapshot, completion_cache, snapshot.file_references_generation, transcript_layout_cache,
                                                  snapshot.transcript_generation, screen_row_cache);
  }
  return wrote;
}

}  // namespace ava::tui
