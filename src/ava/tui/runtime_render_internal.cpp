// Prepares feature display data and transcript layout before the session's single paint stage.
#include "sys.h"
#include "TuiEventLoop.h"
#include "runtime_state_internal.h"
#include "ava/tui/command_output.h"
#include "ava/tui/mermaid_projection.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/terminal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include "debug.h"

namespace ava::tui {
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

bool WheelBurstGovernor::accept_input(terminal::Key key, Clock::time_point now)
{
  using Key = terminal::Key;
  if (key == Key::MouseWheelUp)
    return accept(WheelDirection::Up, now);
  if (key == Key::MouseWheelDown)
    return accept(WheelDirection::Down, now);
  reset();
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

std::optional<FrameScheduler::Clock::time_point> FrameScheduler::deadline() const noexcept
{
  return deadline_;
}

std::optional<FrameRenderKind> FrameScheduler::take_due(Clock::time_point now)
{
  if (!pending_kind_ || (deadline_ && now < *deadline_))
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

RuntimeRenderer::RuntimeRenderer(TuiEventLoop& loop, TuiFrontEnd* frontend, RuntimePresentationState& presentation, Composer& composer,
                                 terminal::Context* terminal_context)
    : presentation_(presentation), draft_state_(composer), terminal_context_(terminal_context), transcript_(presentation, composer), frontend_(frontend)
{
  // Register while all callback targets are alive. The caller services the loop
  // only after construction; partially constructed handles retire on failure.
  frame_timer_ = loop.member_timer(*this, &RuntimeRenderer::frame_due);
  indicator_timer_ = loop.member_timer(*this, &RuntimeRenderer::indicator_due);
  autoscroll_timer_ = loop.member_timer(*this, &RuntimeRenderer::autoscroll_due);
  synchronize_timers();
}

void RuntimeRenderer::synchronize_timers()
{
  // A failed terminal write retires every render-related callback; deadlines must not keep waking a closing
  // session.
  if (frame_scheduler_.failed())
  {
    frame_timer_.cancel();
    indicator_timer_.cancel();
    autoscroll_timer_.cancel();
    armed_frame_deadline_.reset();
    armed_indicator_deadline_.reset();
    armed_autoscroll_deadline_.reset();
    return;
  }
  // Mirror semantic deadlines only when changed. Urgent thaw work is painted at the batch boundary without
  // waiting for the ordinary frame cadence.
  auto const frame = pending_thaw_ ? std::nullopt : frame_scheduler_.deadline();
  if (frame != armed_frame_deadline_)
  {
    armed_frame_deadline_ = frame;
    if (frame)
      frame_timer_.arm_at(*frame);
    else
      frame_timer_.cancel();
  }
  auto const indicator = transcript_.indicator_deadline();
  if (indicator != armed_indicator_deadline_)
  {
    armed_indicator_deadline_ = indicator;
    if (indicator)
      indicator_timer_.arm_at(*indicator);
    else
      indicator_timer_.cancel();
  }
  auto const autoscroll = transcript_.autoscroll_deadline();
  if (autoscroll != armed_autoscroll_deadline_)
  {
    armed_autoscroll_deadline_ = autoscroll;
    if (autoscroll)
      autoscroll_timer_.arm_at(*autoscroll);
    else
      autoscroll_timer_.cancel();
  }
}

void RuntimeRenderer::frame_due(std::chrono::steady_clock::time_point)
{
  // Waking the loop is sufficient. Keep the registered deadline until paint
  // consumes it, so intervening invalidations do not re-arm an expired timer.
}

void RuntimeRenderer::indicator_due(std::chrono::steady_clock::time_point now)
{
  armed_indicator_deadline_.reset();
  if (transcript_.expire_position_indicator(now))
    frame_scheduler_.request(FrameRenderKind::Full, now);
  synchronize_timers();
}

void RuntimeRenderer::autoscroll_due(std::chrono::steady_clock::time_point now)
{
  armed_autoscroll_deadline_.reset();
  if (auto const deadline = transcript_.autoscroll_deadline(); deadline && now >= *deadline)
  {
    auto const authority_ready = prepare_transcript_selection_authority();
    if (transcript_.service_autoscroll(transcript_layout_cache_, authority_ready, now))
      frame_scheduler_.request(FrameRenderKind::Full, now);
  }
  synchronize_timers();
}

TranscriptView const& RuntimeRenderer::transcript() const noexcept
{
  return transcript_;
}
void RuntimeRenderer::scroll_transcript(std::ptrdiff_t rows)
{
  synchronize_detached_transcript_layout();
  auto const maximum = prepare_transcript_layout();
  if (rows >= 0)
    transcript_.scroll_up(static_cast<std::size_t>(rows), maximum);
  else
    transcript_.scroll_down(static_cast<std::size_t>(-(rows + 1)) + 1, maximum);
  show_transcript_position_indicator();
  synchronize_timers();
}
void RuntimeRenderer::navigate_transcript(std::size_t offset)
{
  transcript_.navigate_to_offset(offset, prepare_transcript_layout());
  synchronize_timers();
}
void RuntimeRenderer::show_live_tail(LiveTailReason reason)
{
  switch (reason)
  {
    case LiveTailReason::Requested:
      transcript_.request_live_tail();
      break;
    case LiveTailReason::LocalCommand:
      transcript_.reset_for_local_command();
      break;
    case LiveTailReason::Navigation:
      transcript_.return_to_live_tail();
      break;
  }
  synchronize_timers();
}
void RuntimeRenderer::complete_transcript_update(TranscriptUpdate const& update, std::ptrdiff_t shift, std::size_t event_count, TranscriptUpdateMode mode)
{
  if (mode == TranscriptUpdateMode::Immediate)
  {
    if (update.detached())
    {
      auto const maximum = prepare_transcript_layout();
      transcript_.restore_layout_anchor(update.anchor(), transcript_layout_cache_, maximum, shift);
    }
  }
  else
    transcript_.complete_transcript_update(update, shift, event_count);
  synchronize_timers();
}
void RuntimeRenderer::note_transcript_eviction(std::ptrdiff_t shift)
{
  transcript_.note_live_selection_item_shift(shift);
  synchronize_timers();
}
void RuntimeRenderer::reset_wheel_input()
{
  wheel_governor_.reset();
}
bool RuntimeRenderer::accept_wheel_input(terminal::Key key, WheelBurstGovernor::Clock::time_point now)
{
  return wheel_governor_.accept_input(key, now);
}
std::size_t RuntimeRenderer::prepare_transcript_layout()
{
  return detail::composer_max_transcript_scroll_offset_cached(presentation_.screen(), presentation_.screen().width, presentation_.screen().height,
                                                              completion_cache_, presentation_.screen().file_references_generation, transcript_layout_cache_,
                                                              presentation_.screen().transcript_generation);
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
detail::CompletionMatchModel const* RuntimeRenderer::refresh_completion_cache()
{
  detail::refresh_completion_match_cache(completion_cache_, presentation_.screen(), presentation_.screen().file_references_generation);
  return completion_cache_.model ? &*completion_cache_.model : nullptr;
}
std::size_t RuntimeRenderer::clamp_completion(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::clamp_completion_selection(completion_cache_, selected);
}
std::optional<std::string> RuntimeRenderer::selected_completion_disabled_reason(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::completion_selection_disabled_reason(completion_cache_, presentation_.screen().file_references, selected);
}
FileReferenceSelectionText RuntimeRenderer::selected_completion_text(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::completion_selection_text(completion_cache_, presentation_.screen(), selected);
}
std::optional<std::size_t> RuntimeRenderer::file_reference_completion_at(std::size_t row, std::size_t column)
{
  return detail::file_reference_palette_selection_for_screen_position_cached(presentation_.screen(), row, column, completion_cache_,
                                                                             presentation_.screen().file_references_generation);
}
std::optional<std::size_t> RuntimeRenderer::path_completion_at(std::size_t row, std::size_t column)
{
  return detail::path_completion_palette_selection_for_screen_position_cached(presentation_.screen(), row, column, completion_cache_,
                                                                              presentation_.screen().file_references_generation);
}
std::size_t RuntimeRenderer::sidebar_drawer_max_scroll() const
{
  return sidebar_drawer_max_scroll_offset(frontend_, presentation_.screen());
}

void RuntimeRenderer::synchronize_detached_transcript_layout()
{
  // Update the output count before computing maximum scroll so its reserved row matches the next paint.
  // This is the navigation boundary at which deferred output can change layout.
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
    auto const height = std::max<std::size_t>(detail::kMinHeight, presentation_.screen().height);
    auto const width = composer_canvas_layout(presentation_.screen()).content_width;
    auto const compact = detail::composer_layout_policy(presentation_.screen(), height).compact_transcript_spacing;
    detail::refresh_transcript_layout_cache(transcript_layout_cache_, presentation_.screen().transcript, presentation_.screen().transcript_generation, width,
                                            presentation_.screen().tool_presentation, presentation_.screen().thinking_visible, compact,
                                            detail::active_mermaid_projection(presentation_.screen()));
  }
  return transcript_.prepare_selection_authority(transcript_layout_cache_);
}
TranscriptSelectionMouseResult RuntimeRenderer::handle_transcript_selection_mouse(InputEvent const& event, std::function<bool(std::size_t)> const& toggle_tool,
                                                                                  std::function<bool(std::size_t)> const& toggle_thinking)
{
  if (event.key != terminal::Key::MousePointerCancel && !prepare_transcript_selection_authority())
    return TranscriptSelectionMouseResult::Ignored;
  auto const before = transcript_.scroll_offset();
  auto const result = transcript_.handle_mouse(event, transcript_layout_cache_, toggle_tool, toggle_thinking);
  if (transcript_.scroll_offset() != before)
    frame_scheduler_.request(FrameRenderKind::Full, std::chrono::steady_clock::now());
  synchronize_timers();
  return result;
}
void RuntimeRenderer::show_transcript_position_indicator(TranscriptPositionIndicatorState::Clock::time_point now)
{
  transcript_.show_position_indicator(now);
  frame_scheduler_.request(FrameRenderKind::Full, now);
  synchronize_timers();
}
bool RuntimeRenderer::copy_transcript_selection()
{
  auto const copied = transcript_.copy_selection(transcript_layout_cache_);
  synchronize_timers();
  return copied;
}
void RuntimeRenderer::clear_transcript_selection()
{
  transcript_.clear_selection();
  synchronize_timers();
}
void RuntimeRenderer::cancel_pointer_interaction()
{
  transcript_.cancel_pointer_interaction();
  synchronize_timers();
}
void RuntimeRenderer::reset_for_session_transition()
{
  transcript_.reset_for_session_transition();
  transcript_layout_cache_ = {};
  screen_row_cache_.valid = false;
  wheel_governor_.reset();
  synchronize_timers();
}
bool RuntimeRenderer::render()
{
  if (frame_scheduler_.failed())
    return false;
  frame_scheduler_.request(FrameRenderKind::Full, std::chrono::steady_clock::now());
  pending_thaw_ = true;
  synchronize_timers();
  return true;
}

bool RuntimeRenderer::paint_for_handoff()
{
  if (frame_scheduler_.failed())
    return false;
  frame_scheduler_.discard_pending();
  frame_timer_.cancel();
  armed_frame_deadline_.reset();
  pending_thaw_ = false;
  auto const succeeded = render_full(false);
  frame_scheduler_.paint_completed(std::chrono::steady_clock::now(), succeeded);
  synchronize_timers();
  return succeeded;
}

bool RuntimeRenderer::render_full(bool freeze_transcript_layout)
{
  auto const& snapshot = presentation_.screen();
  bool wrote = false;
  {
    // Update editor, scroll, and output-count data before computing geometry. The new-output badge must
    // reserve its row even when the preceding stream update was not painted.
    presentation_.publish_composer(draft_state_);
    transcript_.begin_frame();
    auto const [width, height] = terminal_size();
    presentation_.publish_dimensions(width, height);
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
    // Clamp independent surfaces after deciding which transcript layout can remain frozen; none may reuse an
    // offset beyond current geometry.
    presentation_.publish_sidebar_drawer(snapshot.sidebar_drawer_visible,
                                         std::min(snapshot.sidebar_drawer_scroll_offset, sidebar_drawer_max_scroll_offset(frontend_, snapshot)));
    if (snapshot.command_output)
    {
      auto const geometry = command_output_geometry(width, height);
      presentation_.publish_command_output_scroll(
          std::min(snapshot.command_output->scroll_offset,
                   command_output_max_scroll_offset(*snapshot.command_output, geometry.width, geometry.height, snapshot.tool_presentation)));
    }
    draft_state_.clamp_scroll(draft_state_.max_draft_scroll_offset(snapshot, height));
    presentation_.publish_composer(draft_state_);
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
    // Choose selection endpoints only after determining which modal is visible and which layout will be
    // drawn, so the highlight and transcript rows agree.
    auto const selection_projection = selection_blocked          ? TranscriptSelectionProjection::Suppressed
                                      : freeze_detached_viewport ? TranscriptSelectionProjection::Frozen
                                                                 : TranscriptSelectionProjection::Live;
    transcript_.publish_selection(transcript_layout_cache_, selection_projection);
    wrote = detail::draw_screen_cached(frontend_, snapshot, completion_cache_, snapshot.file_references_generation, transcript_layout_cache_,
                                       snapshot.transcript_generation, screen_row_cache_, freeze_detached_viewport, freeze_modal_transcript_layout);
  }
  return wrote;
}

bool RuntimeRenderer::request_render(FrameRenderKind kind)
{
  frame_scheduler_.request(kind, std::chrono::steady_clock::now());
  synchronize_timers();
  return !frame_scheduler_.failed();
}

bool RuntimeRenderer::paint_ready_frame()
{
  // View operations are deliberately independent of the renderer. Reconcile
  // their absolute deadlines here even when this batch needs no actual paint.
  synchronize_timers();
  if (frame_scheduler_.failed())
    return false;
  // An urgent render keeps its existing end-of-batch priority. Ordinary
  // requests become paintable by time, without another timer-dispatch turn.
  auto const kind = pending_thaw_ ? frame_scheduler_.take_pending() : frame_scheduler_.take_due(std::chrono::steady_clock::now());
  if (!kind)
    return true;
  auto const freeze_transcript_layout = !std::exchange(pending_thaw_, false);
  frame_timer_.cancel();
  armed_frame_deadline_.reset();
  // Consume timer/deadline state before terminal output so a later invalidation starts a fresh pending frame;
  // then latch failure and reconcile remaining timers.
  auto const succeeded = paint(*kind, freeze_transcript_layout);
  frame_scheduler_.paint_completed(std::chrono::steady_clock::now(), succeeded);
  synchronize_timers();
  return succeeded;
}

bool RuntimeRenderer::render_failed() const
{
  return frame_scheduler_.failed();
}

bool RuntimeRenderer::paint(FrameRenderKind kind, bool freeze_transcript_layout)
{
  if (terminal_context_)
    terminal_context_->apply_cursor_settings(presentation_.screen().cursor);
  if (kind == FrameRenderKind::Full)
    return render_full(freeze_transcript_layout);

  auto const& snapshot = presentation_.screen();
  bool wrote = false;
  {
    if (snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output || snapshot.select_list || snapshot.subagent_workspace ||
        snapshot.sidebar_drawer_visible || !snapshot.processing)
      return true;
    wrote = detail::draw_processing_footer_cached(frontend_, snapshot, completion_cache_, snapshot.file_references_generation, transcript_layout_cache_,
                                                  snapshot.transcript_generation, screen_row_cache_);
  }
  return wrote;
}

}  // namespace ava::tui
