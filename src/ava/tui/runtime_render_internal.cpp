#include "sys.h"
#include "ava/tui/command_output.h"
#include "ava/tui/mermaid_projection.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/terminal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <string>

namespace ava::tui {
using Signals = core::Signals;

std::pair<std::size_t, std::size_t> terminal_size()
{
  auto const& terminal_context = ava::core::Application::instance().terminal_context();
  auto const height = terminal_context.rows();
  auto const width = terminal_context.cols();
  if (width > 0 && height > 0)
    return {width, height};
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

bool runtime_wheel_input_accepted(WheelBurstGovernor& governor, terminal::Key key, WheelBurstGovernor::Clock::time_point now)
{
  using Key = terminal::Key;
  if (key == Key::MouseWheelUp)
    return governor.accept(WheelDirection::Up, now);
  if (key == Key::MouseWheelDown)
    return governor.accept(WheelDirection::Down, now);
  governor.reset();
  return true;
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

RuntimeRenderer::RuntimeRenderer(TuiFrontEnd* frontend, ComposerSnapshot& snapshot, SidebarSnapshot const& sidebar, Composer& composer,
                                 terminal::Context* terminal_context)
    : snapshot_(snapshot), draft_state_(composer), terminal_context_(terminal_context), transcript_(snapshot, sidebar, composer), frontend_(frontend)
{
}

std::unique_lock<std::recursive_mutex> RuntimeRenderer::lock_ui()
{
  return std::unique_lock<std::recursive_mutex>(ui_mutex_);
}
TranscriptView& RuntimeRenderer::transcript() noexcept
{
  return transcript_;
}
TranscriptView const& RuntimeRenderer::transcript() const noexcept
{
  return transcript_;
}
void RuntimeRenderer::reset_wheel_input()
{
  wheel_governor_.reset();
}
bool RuntimeRenderer::accept_wheel_input(terminal::Key key, WheelBurstGovernor::Clock::time_point now)
{
  return runtime_wheel_input_accepted(wheel_governor_, key, now);
}
std::size_t RuntimeRenderer::prepare_transcript_layout()
{
  return detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, completion_cache_,
                                                              snapshot_.file_references_generation, transcript_layout_cache_, snapshot_.transcript_generation);
}
detail::TranscriptViewportAnchor RuntimeRenderer::capture_transcript_viewport()
{
  auto const maximum = prepare_transcript_layout();
  return detail::capture_transcript_viewport_anchor(transcript_layout_cache_.layout, maximum, transcript_.scroll_offset());
}
TranscriptUpdate RuntimeRenderer::begin_transcript_update()
{
  auto const detached = transcript_.scroll_offset() > 0;
  auto anchor = detail::TranscriptViewportAnchor{};
  if (detached && !transcript_.has_deferred_update())
    anchor = capture_transcript_viewport();
  if (detached)
    transcript_.freeze_sidebar();
  return TranscriptUpdate(detached, anchor);
}
detail::TranscriptLayoutCache const& RuntimeRenderer::prepared_transcript_layout() const noexcept
{
  return transcript_layout_cache_;
}
detail::CompletionMatchCache const& RuntimeRenderer::completion_cache() const noexcept
{
  return completion_cache_;
}
detail::ScreenRowCache const& RuntimeRenderer::painted_frame() const noexcept
{
  return screen_row_cache_;
}
detail::CompletionMatchModel const* RuntimeRenderer::refresh_completion_cache()
{
  detail::refresh_completion_match_cache(completion_cache_, snapshot_, snapshot_.file_references_generation);
  return completion_cache_.model ? &*completion_cache_.model : nullptr;
}
std::size_t RuntimeRenderer::clamp_completion(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::clamp_completion_selection(completion_cache_, selected);
}
std::size_t RuntimeRenderer::previous_completion(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::previous_completion_selection(completion_cache_, selected);
}
std::size_t RuntimeRenderer::next_completion(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::next_completion_selection(completion_cache_, selected);
}
std::optional<std::string> RuntimeRenderer::selected_completion_disabled_reason(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::completion_selection_disabled_reason(completion_cache_, snapshot_.file_references, selected);
}
FileReferenceSelectionText RuntimeRenderer::selected_completion_text(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::completion_selection_text(completion_cache_, snapshot_, selected);
}
std::optional<std::size_t> RuntimeRenderer::file_reference_completion_at(std::size_t row, std::size_t column)
{
  return detail::file_reference_palette_selection_for_screen_position_cached(snapshot_, row, column, completion_cache_, snapshot_.file_references_generation);
}
std::optional<std::size_t> RuntimeRenderer::path_completion_at(std::size_t row, std::size_t column)
{
  return detail::path_completion_palette_selection_for_screen_position_cached(snapshot_, row, column, completion_cache_, snapshot_.file_references_generation);
}
std::size_t RuntimeRenderer::sidebar_drawer_max_scroll() const
{
  return sidebar_drawer_max_scroll_offset(frontend_, snapshot_);
}

void RuntimeRenderer::synchronize_detached_transcript_layout()
{
  // Navigation publishes chrome before max-scroll math so reserved-row geometry
  // matches the eventual paint, while deferred output remains frozen until here.
  transcript_.publish_chrome();
  if (!transcript_.has_deferred_update())
    return;
  auto const maximum = transcript_.scroll_offset() > 0 ? prepare_transcript_layout() : 0;
  transcript_.synchronize_deferred(transcript_layout_cache_, maximum);
}
bool RuntimeRenderer::prepare_transcript_selection_authority()
{
  // A deferred detached viewport stays bound to the cache actually drawn.
  if (!transcript_.has_deferred_update())
  {
    auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot_.height);
    auto const width = composer_canvas_layout(snapshot_).content_width;
    auto const compact = detail::composer_layout_policy(snapshot_, height).compact_transcript_spacing;
    detail::refresh_transcript_layout_cache(transcript_layout_cache_, snapshot_.transcript, snapshot_.transcript_generation, width, snapshot_.tool_presentation,
                                            snapshot_.thinking_visible, compact, detail::active_mermaid_projection(snapshot_));
  }
  return transcript_.prepare_selection_authority(transcript_layout_cache_);
}
TranscriptSelectionMouseResult RuntimeRenderer::handle_transcript_selection_mouse(InputEvent const& event, std::function<bool(std::size_t)> const& toggle_tool,
                                                                                  std::function<bool(std::size_t)> const& toggle_thinking)
{
  auto lock = lock_ui();
  if (event.key != terminal::Key::MousePointerCancel && !prepare_transcript_selection_authority())
    return TranscriptSelectionMouseResult::Ignored;
  auto const before = transcript_.scroll_offset();
  auto const result = transcript_.handle_mouse(event, transcript_layout_cache_, toggle_tool, toggle_thinking);
  if (transcript_.scroll_offset() != before)
    frame_scheduler_.request(FrameRenderKind::Full, std::chrono::steady_clock::now());
  return result;
}
void RuntimeRenderer::show_transcript_position_indicator(TranscriptPositionIndicatorState::Clock::time_point now)
{
  auto lock = lock_ui();
  transcript_.show_position_indicator(now);
  frame_scheduler_.request(FrameRenderKind::Full, now);
}
bool RuntimeRenderer::copy_transcript_selection()
{
  auto lock = lock_ui();
  return transcript_.copy_selection(transcript_layout_cache_);
}
void RuntimeRenderer::clear_transcript_selection()
{
  auto lock = lock_ui();
  transcript_.clear_selection();
}
void RuntimeRenderer::cancel_pointer_interaction()
{
  auto lock = lock_ui();
  transcript_.cancel_pointer_interaction();
}
void RuntimeRenderer::reset_for_session_transition()
{
  auto lock = lock_ui();
  transcript_.reset_for_session_transition();
  transcript_layout_cache_ = {};
  screen_row_cache_.valid = false;
  wheel_governor_.reset();
}
void RuntimeRenderer::service_auxiliary_timers(std::chrono::steady_clock::time_point now)
{
  auto lock = lock_ui();
  if (transcript_.autoscroll_due(now))
  {
    auto const authority_ready = prepare_transcript_selection_authority();
    if (transcript_.service_autoscroll(transcript_layout_cache_, authority_ready, now))
      frame_scheduler_.request(FrameRenderKind::Full, now);
  }
  if (transcript_.expire_position_indicator(now))
    frame_scheduler_.request(FrameRenderKind::Full, now);
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
  bool wrote = false;
  {
    auto lock = lock_ui();
    draft_state_.project(snapshot);
    // Publish viewport chrome before freeze/sync/clamp so the N-new reserved row
    // participates in geometry even when the prior stream update was not painted.
    transcript_.begin_frame();
    auto const [width, height] = terminal_size();
    snapshot.width = width;
    snapshot.height = height;
    auto const compact_spacing = detail::composer_layout_policy(snapshot, height).compact_transcript_spacing;
    auto const presentation_settings_compatible = transcript_layout_cache_.valid && transcript_layout_cache_.tool_presentation == snapshot.tool_presentation &&
                                                  transcript_layout_cache_.thinking_visible == snapshot.thinking_visible &&
                                                  transcript_layout_cache_.compact_spacing == compact_spacing;
    auto const frozen_cache_compatible = presentation_settings_compatible && transcript_layout_cache_.width == composer_canvas_layout(snapshot).content_width;
    auto const freeze_modal_transcript_layout =
        (snapshot.command_output.has_value() || (snapshot.select_list.has_value() && snapshot.select_list->freeze_underlying_transcript_layout) ||
         snapshot.subagent_workspace.has_value()) &&
        presentation_settings_compatible;
    // Detached freeze keeps a width-compatible cache across scheduled paints. Modal freeze keeps the
    // pre-modal underlying layout even when the active selector canvas width differs, and freezes the
    // draw itself whether or not a deferred detached viewport is pending.
    auto const freeze_detached_viewport =
        freeze_modal_transcript_layout || (freeze_transcript_layout && transcript_.has_deferred_update() && frozen_cache_compatible);
    auto const synchronized_detached_viewport = transcript_.has_deferred_update() && !freeze_detached_viewport;
    if (synchronized_detached_viewport)
      synchronize_detached_transcript_layout();
    snapshot.sidebar_drawer_scroll_offset = std::min(snapshot.sidebar_drawer_scroll_offset, sidebar_drawer_max_scroll_offset(frontend_, snapshot));
    if (snapshot.command_output)
    {
      auto const geometry = command_output_geometry(width, height);
      snapshot.command_output->scroll_offset =
          std::min(snapshot.command_output->scroll_offset,
                   command_output_max_scroll_offset(*snapshot.command_output, geometry.width, geometry.height, snapshot.tool_presentation));
    }
    draft_state_.clamp_scroll(draft_state_.max_draft_scroll_offset(snapshot, height));
    snapshot.draft_scroll_offset = draft_state_.scroll_offset();
    if (transcript_.scroll_offset() > 0 && !synchronized_detached_viewport && !freeze_detached_viewport)
      transcript_.clamp_for_frame(prepare_transcript_layout());
    transcript_.finish_frame_viewport();
    detail::refresh_completion_match_cache(completion_cache_, snapshot, snapshot.file_references_generation);
    auto const completion_palette_visible = completion_cache_.model && completion_cache_.model->palette_visible;
    auto const slash_palette_is_visible =
        !snapshot.slash_palette_suppressed && slash_palette_visible(snapshot.input, snapshot.input_cursor, snapshot.slash_commands);
    auto const selection_blocked = snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output || snapshot.plugin_ui_modal ||
                                   snapshot.select_list || snapshot.subagent_workspace || snapshot.sidebar_drawer_visible || slash_palette_is_visible ||
                                   completion_palette_visible;
    if (!selection_blocked && transcript_.has_selection() && !freeze_detached_viewport)
    {
      auto const width = composer_canvas_layout(snapshot).content_width;
      auto const compact = detail::composer_layout_policy(snapshot, height).compact_transcript_spacing;
      detail::refresh_transcript_layout_cache(transcript_layout_cache_, snapshot.transcript, snapshot.transcript_generation, width, snapshot.tool_presentation,
                                              snapshot.thinking_visible, compact, detail::active_mermaid_projection(snapshot));
    }
    auto const selection_projection = selection_blocked          ? TranscriptSelectionProjection::Suppressed
                                      : freeze_detached_viewport ? TranscriptSelectionProjection::Frozen
                                                                 : TranscriptSelectionProjection::Live;
    transcript_.publish_selection(transcript_layout_cache_, selection_projection);
    wrote = detail::draw_screen_cached(frontend_, snapshot, completion_cache_, snapshot.file_references_generation, transcript_layout_cache_,
                                       snapshot.transcript_generation, screen_row_cache_, freeze_detached_viewport, freeze_modal_transcript_layout);
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
  return frame_scheduler_.pending() || transcript_.time_until_service(std::chrono::steady_clock::now()).has_value();
}

std::chrono::steady_clock::duration RuntimeRenderer::time_until_pending_render() const
{
  auto const now = std::chrono::steady_clock::now();
  auto wait = std::chrono::steady_clock::duration::max();
  if (frame_scheduler_.pending())
    wait = std::min(wait, frame_scheduler_.time_until_due(now));
  if (auto const service_wait = transcript_.time_until_service(now))
    wait = std::min(wait, *service_wait);
  return wait == std::chrono::steady_clock::duration::max() ? std::chrono::steady_clock::duration::zero() : wait;
}

bool RuntimeRenderer::render_failed() const
{
  return frame_scheduler_.failed();
}

bool RuntimeRenderer::paint(FrameRenderKind kind, bool freeze_transcript_layout)
{
  if (terminal_context_)
    terminal_context_->apply_cursor_settings(snapshot_.cursor);
  if (kind == FrameRenderKind::Full)
    return render_full(freeze_transcript_layout);

  auto& snapshot = snapshot_;
  bool wrote = false;
  {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    if (snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output || snapshot.select_list || snapshot.subagent_workspace ||
        snapshot.sidebar_drawer_visible || !snapshot.processing)
      return true;
    wrote = detail::draw_processing_footer_cached(frontend_, snapshot, completion_cache_, snapshot.file_references_generation, transcript_layout_cache_,
                                                  snapshot.transcript_generation, screen_row_cache_);
  }
  return wrote;
}

}  // namespace ava::tui
