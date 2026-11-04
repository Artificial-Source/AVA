// Owns one submission and its worker lifetime; see docs/engineering/tui-architecture.md.
#include "sys.h"
#include "PendingAttachments.h"
#include "SessionTransitionCoordinator.h"
#include "ava/agent/question.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/ActiveRunEvents.h"
#include "ava/tui/ActiveRunInbox.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/command_output.h"
#include "ava/tui/event_state.h"
#include "ava/tui/runtime_commands_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/runtime_mermaid_internal.h"
#include "ava/tui/runtime_plugin_ui_internal.h"
#include "ava/tui/runtime_prompts_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/terminal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/permissions/permission.h"
#include "ava/core/ids.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ava::tui {

#ifdef CWDEBUG
void ActiveRun::Operation::print_on(std::ostream& os) const
{
  os << "{is_command_submission:" << is_command_submission << ", close_after_submit:" << close_after_submit
     << ", terminal_write_failed:" << terminal_write_failed << ", terminal_signal_received:" << terminal_signal_received
     << ", queues_finished:" << queues_finished << ", completion_failed:" << completion_failed << '}';
}
#endif

using runtime_commands::is_compact_command;
using runtime_commands::shell_helper_submission;
using runtime_transcript::apply_assistant_turn_meta;
using runtime_transcript::assistant_meta_for_snapshot;

ActiveRun::ActiveRun(TuiEventLoop& loop, TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, RuntimeRenderer& renderer,
                     RuntimePromptCoordinator& prompt_coordinator, RuntimePluginUiCoordinator& plugin_ui, TranscriptSearchController& transcript_search,
                     RuntimeSubagentWorkspaceController& subagent_workspace, SessionTransitionCoordinator& transitions,
                     RuntimeMermaidPresentationController& mermaid, PendingAttachments& attachments, terminal::Context& terminal_context)
    : frontend_(frontend),
      presentation_state_(presentation_state),
      renderer_(renderer),
      prompt_coordinator_(prompt_coordinator),
      plugin_ui_(plugin_ui),
      transcript_search_(transcript_search),
      subagent_workspace_(subagent_workspace),
      transitions_(transitions),
      mermaid_(mermaid),
      attachments_(attachments),
      terminal_context_(terminal_context),
      wakeup_(loop.wakeup()),
      spinner_(loop, std::bind_front(&ActiveRun::publish_spinner_frame, this))
{
}

void ActiveRun::upsert_stopping_activity()
{
  presentation_state_.publish_activity_item(SidebarActivityItem{
      .id = "stopping", .label = "stopping", .detail = "waiting for active work to stop; queued drafts skip on stop", .status = ToolTimelineStatus::Running});
}

void ActiveRun::settle_turn_activity()
{
  if (operation_->inbox->stop_requested() || operation_->events->aggregate().run_status == TuiEventRunStatus::Canceled)
    presentation_state_.settle_responding_activity(ToolTimelineStatus::Canceled, "assistant stopped");
  else if (operation_->events->aggregate().run_status == TuiEventRunStatus::Error)
    presentation_state_.settle_responding_activity(ToolTimelineStatus::Error, "assistant failed");
  else
    presentation_state_.settle_responding_activity(ToolTimelineStatus::Success, "assistant responded");
}

void ActiveRun::apply_non_transcript_event_authority()
{
  auto const& state = operation_->events->aggregate();
  presentation_state_.publish_run_progress(state);
  if (operation_->inbox->stop_requested() && state.run_status == TuiEventRunStatus::Running)
    upsert_stopping_activity();
}

ActiveRunOutcome ActiveRun::exit_outcome() const
{
  auto outcome = ActiveRunOutcome::close_session();
  if (active() && operation_->terminal_write_failed)
    outcome = outcome.with_terminal_write_failure();
  if (active() && operation_->terminal_signal_received)
    outcome = outcome.with_terminal_signal();
  if (active() && operation_->completion_failed)
    outcome = outcome.with_cleanup_failure();
  return outcome;
}

ActiveRun::~ActiveRun()
{
  shutdown();
}

bool ActiveRun::refresh_plugin_surface_fit()
{
  refresh_geometry_from_kernel();
  auto const [width, height] = terminal_size();
  presentation_state_.publish_dimensions(width, height);
  return plugin_ui_.cancel_unfittable_surfaces();
}

bool ActiveRun::render()
{
  static_cast<void>(refresh_plugin_surface_fit());
  return renderer_.render();
}

void ActiveRun::submit_worker(TuiFrontEnd* frontend, std::shared_ptr<ActiveRunInbox> inbox, std::string submitted, TuiSubmitContext context)
{
  ActiveRunResult final_result{TuiSubmitResult{}};
  {
    auto owned_submission = std::move(submitted);
    auto owned_context = std::move(context);
    try
    {
      final_result = frontend->on_submit(owned_submission, std::move(owned_context));
    }
    catch (...)
    {
      final_result = std::current_exception();
    }
  }
  // Backend callback scopes have ended. Store the final result last, then notify
  // the loop and return; the TUI may take it and join this worker immediately.
  inbox->publish_result(std::move(final_result));
}

bool ActiveRun::start(std::string submitted)
{
  // Finish or shut down the previous submission before starting another one.
  ASSERT(!active());
  if (active())
    return false;
  operation_.emplace();
  operation_->submitted = std::move(submitted);
  operation_->is_command_submission = operation_->submitted.starts_with('/') || shell_helper_submission(operation_->submitted);
  try
  {
    // Prepare transport identities and the pre-submit baseline before any worker
    // can publish; queue callbacks retain inbox ownership independently of this operation.
    auto const& snapshot = presentation_state_.screen();
    if (operation_->is_command_submission && runtime_commands::session_switching_command(operation_->submitted))
      attachments_.clear();
    std::vector<ava::session::ImageAttachmentRef> attachments;
    if (!operation_->is_command_submission)
      attachments = attachments_.take();
    operation_->inbox = std::make_shared<ActiveRunInbox>(wakeup_);
    auto const supports_queue = !operation_->is_command_submission || is_compact_command(operation_->submitted);
    if (supports_queue)
      operation_->active_queues = frontend_.create_active_run_queues(operation_->inbox->envelope_sink());
    auto request_id = operation_->active_queues ? operation_->active_queues->active_request_id : std::string{};
    if (request_id.empty())
      request_id = ava::core::make_id("request");
    else
      operation_->active_queues->mark_follow_up_started =
          operation_->inbox->wrap_follow_up_started(std::move(operation_->active_queues->mark_follow_up_started));
    operation_->inbox->set_request_id(request_id);
    operation_->events = std::make_unique<ActiveRunEvents>(operation_->is_command_submission, operation_->submitted, request_id, snapshot.transcript,
                                                           presentation_state_.sidebar().todos);
    auto endpoint = plugin_ui_.begin_submission(operation_->submitted, request_id);
    prompt_coordinator_.set_audit_sink(operation_->inbox->event_sink());
    operation_->turn_started_at = std::chrono::steady_clock::now();
    presentation_state_.publish_run_frame(true, std::nullopt, operation_->is_command_submission ? "running command..." : "thinking...");
    if (!render())
    {
      operation_->terminal_write_failed = true;
      shutdown();
      return false;
    }
    // Request the initial processing frame before accepting prompts and launching work.
    // The catch path retires any partially constructed operation through shutdown.
    prompt_coordinator_.open_admission();
    // The plugin owner and its synchronized deadline state outlive this joined
    // worker. This predicate never reads screen data or accesses the terminal.
    auto cancel_requested = [inbox = operation_->inbox, plugin = &plugin_ui_]() { return inbox->stop_requested() || plugin->deadline_reached(); };
    TuiSubmitContext context{
        .request_id = std::move(request_id),
        .permission_resolver = prompt_coordinator_.permission_resolver(),
        .question_resolver = prompt_coordinator_.question_resolver(),
        .event_sink = operation_->inbox->event_sink(),
        .cancel_requested = std::move(cancel_requested),
        .take_steering_messages =
            operation_->active_queues ? operation_->active_queues->take_steering_messages : std::function<ava::core::Result<std::vector<std::string>>()>{},
        .skip_active_steering =
            operation_->active_queues ? operation_->active_queues->skip_active_steering : std::function<ava::core::VoidResult(std::string_view)>{},
        .take_next_follow_up = operation_->active_queues ? operation_->active_queues->take_next_follow_up : std::function<std::optional<TuiQueuedFollowUp>()>{},
        .mark_follow_up_started =
            operation_->active_queues ? operation_->active_queues->mark_follow_up_started : std::function<ava::core::VoidResult(TuiQueuedFollowUp const&)>{},
        .on_subagent_launch = operation_->inbox->subagent_launch_sink(),
        .plugin_ui = std::move(endpoint),
        .image_attachments = std::move(attachments),
        .key_bindings = frontend_.key_bindings()};
    operation_->worker = ava::core::make_async(
        "tui_submit", std::bind_front(&ActiveRun::submit_worker, &frontend_, operation_->inbox, operation_->submitted, std::move(context)));
    spinner_.start(snapshot.spinner_frame);
    return true;
  }
  catch (...)
  {
    shutdown();
    throw;
  }
}

bool ActiveRun::queue_message(std::string message, bool steering)
{
  // TUI thread: availability and cancellation are checked before entering the
  // backend queue. Only accepted messages let the input owner clear its draft.
  auto& queues = operation_->active_queues;
  if (operation_->inbox->stop_requested())
    presentation_state_.publish_status("stop requested; queueing disabled");
  else if (!queues)
    presentation_state_.publish_status("active-run queue unavailable");
  else if (steering && !queues->queue_steering)
    presentation_state_.publish_status("active-run steering unavailable");
  else if (!steering && !queues->queue_follow_up)
    presentation_state_.publish_status("active-run follow-up unavailable");
  else
  {
    auto queued = steering ? queues->queue_steering(std::move(message)) : queues->queue_follow_up(std::move(message));
    if (queued)
      return true;
    presentation_state_.publish_status(queued.error().format());
    terminal_context_.beep();
  }
  return false;
}

std::optional<TuiRestoredQueuedMessage> ActiveRun::restore_message()
{
  if (!operation_->active_queues || !operation_->active_queues->restore_latest)
  {
    presentation_state_.publish_status("active-run restore unavailable");
    return std::nullopt;
  }
  auto restored = operation_->active_queues->restore_latest();
  if (restored)
    return std::move(*restored);
  presentation_state_.publish_status(restored.error().format());
  terminal_context_.beep();
  return std::nullopt;
}

TuiActiveNonblockingCommandDispatchResult ActiveRun::dispatch_command(ComposerSnapshot const& completion, std::string const& command)
{
  if (!operation_->active_queues || !operation_->active_queues->run_nonblocking_command)
    return {};
  return dispatch_tui_active_nonblocking_command_gated(completion, *operation_->active_queues, command);
}

bool ActiveRun::stop_requested() const
{
  return active() && operation_->inbox && (operation_->inbox->stop_requested() || plugin_ui_.deadline_reached());
}

bool ActiveRun::request_stop()
{
  auto const newly_requested = operation_->inbox->request_stop();
  plugin_ui_.cancel_active();
  prompt_coordinator_.fail_pending_requests();
  if (newly_requested)
    terminal_context_.beep();
  {
    presentation_state_.publish_status("stop requested");
    upsert_stopping_activity();
  }
  return renderer_.request_render();
}

void ActiveRun::request_close_after_submit()
{
  static_cast<void>(operation_->inbox->request_stop());
  operation_->close_after_submit = true;
  plugin_ui_.cancel_active();
  prompt_coordinator_.fail_pending_requests();
}

bool ActiveRun::cancel(ActiveRunCancel reason)
{
  if (!active())
    return true;
  // Resolve a ready question automatically before publishing control cancellation.
  static_cast<void>(prompt_coordinator_.cancel_active(reason == ActiveRunCancel::Stop ? PromptCancelReason::RunStop : PromptCancelReason::TerminalSignal));
  if (reason == ActiveRunCancel::Stop)
    return request_stop();
  request_close_after_submit();
  if (reason == ActiveRunCancel::TerminalSignal)
    operation_->terminal_signal_received = true;
  return true;
}

void ActiveRun::before_prompt()
{
  // Stop the spinner while a host prompt is active. Restart from its displayed
  // frame after the prompt completes, preserving the existing animation cadence.
  spinner_.stop();
  transcript_search_.close_before_prompt();
}

void ActiveRun::service_prompt()
{
  // Only direct permission input can request stop. Automatic prompt service either
  // retains or completes a prompt and cannot return that input-only outcome.
  static_cast<void>(prompt_coordinator_.service());
  if (!prompt_coordinator_.has_active())
    static_cast<void>(prompt_coordinator_.start_pending(std::bind_front(&ActiveRun::stop_requested, this), std::bind_front(&ActiveRun::before_prompt, this)));
}

bool ActiveRun::apply_prompt_outcome(PromptServiceOutcome outcome)
{
  if (outcome != PromptServiceOutcome::RequestRunStop)
    return true;
  // Escape keeps the permission request held until stop is visible to the worker.
  // Complete it only after the input callback has returned, avoiding reentrancy.
  auto const rendered = request_stop();
  static_cast<void>(prompt_coordinator_.cancel_active(PromptCancelReason::RunStop));
  return rendered;
}

bool ActiveRun::plugin_host_modal_conflict() const
{
  auto const& snapshot = presentation_state_.screen();
  return snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output || snapshot.select_list || snapshot.subagent_workspace ||
         snapshot.sidebar_drawer_visible || transcript_search_.is_open() ||
         slash_palette_visible(snapshot.input, snapshot.input_cursor, snapshot.slash_commands) ||
         file_reference_palette_visible(snapshot.input, snapshot.input_cursor, snapshot.file_references) ||
         path_completion_palette_visible(snapshot.input, snapshot.input_cursor, snapshot.file_references, snapshot.path_completion_force_active);
}

bool ActiveRun::poll_plugin_ui()
{
  // Host prompts own input until their callbacks retire. Keep plugin requests
  // queued (and any visible modal intact) so they can resume after the prompt.
  if (prompt_coordinator_.has_active())
    return true;
  auto const geometry_changed = refresh_plugin_surface_fit();
  auto const polled = plugin_ui_.poll(plugin_host_modal_conflict());
  if (polled.deadline_expired && !cancel(ActiveRunCancel::Stop))
    return false;
  return (!geometry_changed && !polled.changed) || renderer_.request_render();
}

void ActiveRun::publish_spinner_frame(std::size_t frame)
{
  if (!active() || !presentation_state_.screen().processing)
    return;
  {
    presentation_state_.publish_run_frame(true, frame);
  }
  if (!subagent_workspace_.active() && !renderer_.request_render(FrameRenderKind::Footer))
    operation_->terminal_write_failed = true;
}

RuntimeEventDrainResult ActiveRun::consume(std::size_t max_events)
{
  if (!active())
    return RuntimeEventDrainResult::NoEvents;
  if (operation_->terminal_write_failed)
    return RuntimeEventDrainResult::RenderFailed;
  service_prompt();
  if (!poll_plugin_ui())
  {
    operation_->terminal_write_failed = true;
    return RuntimeEventDrainResult::RenderFailed;
  }
  // Keep provider events queued while a host prompt is visible. TUI callbacks may
  // borrow worker-stack data (for example, QuestionPrompt::auto_resolve); retire
  // them before releasing that worker. Workers never read the prompt view.
  if (prompt_coordinator_.has_active())
    return RuntimeEventDrainResult::NoEvents;
  if (!spinner_.active())
    spinner_.start(presentation_state_.screen().spinner_frame);
  auto const drained = drain_events(max_events);
  if (drained == RuntimeEventDrainResult::RenderFailed)
    operation_->terminal_write_failed = true;
  return drained;
}

bool ActiveRun::handle_search_input_event(runtime_input::RuntimeInput const& active_input)
{
  using Key = terminal::Key;
  if (active_input.event.key != Key::MouseWheelUp && active_input.event.key != Key::MouseWheelDown)
    renderer_.reset_wheel_input();
  if (active_input.resize)
  {
    transcript_search_.refresh_after_resize();
    return renderer_.render();
  }
  if ((active_input.event.key == Key::MouseWheelUp || active_input.event.key == Key::MouseWheelDown) && !renderer_.accept_wheel_input(active_input.event.key))
  {
    return true;
  }
  return transcript_search_.handle_input(active_input.event).value_or(true);
}

std::optional<bool> ActiveRun::handle_modal_input(runtime_input::RuntimeInput const& input)
{
  if (!active() || operation_->close_after_submit)
    return true;
  // An active host prompt receives the retained key before plugin or search routing.
  // Returning a value prevents ordinary active-input stages from seeing it.
  if (prompt_coordinator_.has_active())
  {
    auto const outcome = prompt_coordinator_.handle_input(input);
    return apply_prompt_outcome(outcome);
  }
  auto const& snapshot = presentation_state_.screen();
  if (snapshot.plugin_ui_modal)
  {
    auto const geometry_changed = input.event.key != terminal::Key::CtrlC && refresh_plugin_surface_fit();
    if (input.resize)
      return renderer_.render();
    if (geometry_changed)
      return renderer_.request_render();
    auto const outcome = plugin_ui_.handle_input(input.event);
    if (outcome == TuiPluginUiInputResult::RequestStop)
      return request_stop();
    if (outcome == TuiPluginUiInputResult::Redraw)
      return renderer_.request_render();
    if (outcome == TuiPluginUiInputResult::Handled)
      return true;
  }
  if (transcript_search_.is_open())
  {
    // This binding is synchronous: the coordinator chooses a queued prompt or
    // search under its mutex, then unlocks before invoking either callback.
    auto const outcome = prompt_coordinator_.dispatch_search_input_with_prompt_precedence(std::bind_front(&ActiveRun::handle_search_input_event, this, input),
                                                                                          std::bind_front(&ActiveRun::stop_requested, this),
                                                                                          std::bind_front(&ActiveRun::before_prompt, this));
    return outcome != SearchInputPromptDispatchResult::InputFailed;
  }
  // Check again for a prompt queued after earlier service. If one starts, discard
  // the old key because it was decoded for a different view.
  auto const prompt = prompt_coordinator_.start_pending(std::bind_front(&ActiveRun::stop_requested, this), std::bind_front(&ActiveRun::before_prompt, this));
  if (prompt != PromptServiceOutcome::None)
    return true; // Discard the retained key; it never answers the newly opened prompt.
  return std::nullopt;
}

bool ActiveRun::accepts_input() const
{
  return active() && operation_->inbox && !operation_->inbox->result_ready();
}

bool ActiveRun::has_pending_updates()
{
  return active() && !prompt_coordinator_.has_active() && (operation_->inbox->has_pending() || prompt_coordinator_.has_pending_requests());
}

void ActiveRun::finish_queues()
{
  if (operation_->queues_finished)
    return;
  operation_->queues_finished = true;
  if (operation_->active_queues && operation_->active_queues->finish)
  {
    if (auto finished = operation_->active_queues->finish(operation_->inbox->stop_requested()); !finished)
    {
      // Keep failure before preparing feedback, which may throw. Diagnostics
      // name only the stage because the backend error can contain private text.
      operation_->completion_failed = true;
      std::fprintf(stderr, "TUI cleanup failed at queue receipts\n");
      presentation_state_.publish_status(finished.error().format());
    }
  }
}

std::optional<ActiveRunOutcome> ActiveRun::complete()
{
  if (!active())
    return std::nullopt;
  try
  {
    auto final_result = operation_->inbox->take_result();
    if (!final_result)
      return std::nullopt;
    if (auto error = std::get_if<std::exception_ptr>(&*final_result))
    {
      auto exception = *error;
      shutdown();
      std::rethrow_exception(exception);
    }
    auto result = std::move(std::get<TuiSubmitResult>(*final_result));
    // Remove TUI callbacks borrowing worker data and release prompt/plugin waits
    // before joining. The final result says backend callback scopes have ended;
    // get() still waits for the worker epilogue.
    prompt_coordinator_.fail_pending_requests();
    static_cast<void>(prompt_coordinator_.cancel_active(PromptCancelReason::Shutdown));
    plugin_ui_.finish_submission();
    operation_->worker.get();
    // The backend's ordinary_turn_request_ids select command conversation events
    // before the final transcript update. A session change discards old-session data.
    operation_->events->authorize_receipts(result.ordinary_turn_request_ids);
    auto committed_session_id = frontend_.read_committed_session_id();
    auto boundary_reads = result.quit ? RuntimePresentationState::BoundaryReads{} : RuntimePresentationState::read_boundary_data(frontend_);
    bool const session_changed = transitions_.apply(std::move(committed_session_id));
    if (!result.quit)
      presentation_state_.apply_boundary_data(frontend_, std::move(boundary_reads));
    finish_queues();
    // Finishing backend queues can enqueue final events. Close the inbox afterward
    // so retained sinks cannot add old-session events after discard.
    operation_->inbox->retire();
    if (session_changed)
    {
      operation_->events->discard();
      operation_->inbox->discard();
    }
    prompt_coordinator_.set_audit_sink(nullptr);
    spinner_.stop();
    // The inbox is closed and the worker has joined: the backlog is now finite.
    // Drain it in bounded batches in this call. The first drain also updates the
    // transcript with command events matching the backend's ordinary-turn IDs.
    do
    {
      if (drain_events(256) == RuntimeEventDrainResult::RenderFailed)
      {
        operation_->terminal_write_failed = true;
        break;
      }
    }
    while (operation_->inbox->has_pending());
    if (!session_changed)
    {
      if (operation_->is_command_submission)
      {
        auto local_tools = result.local_command_result ? result.local_command_result->tool_timeline : std::vector<ToolTimelineItem>{};
        local_tools = operation_->events->merge_unauthorized_tools(std::move(local_tools));
        if (!local_tools.empty())
        {
          if (!result.local_command_result)
            result.local_command_result = TuiLocalCommandResult{};
          result.local_command_result->tool_timeline = std::move(local_tools);
        }
      }
      if (result.local_command_result)
        for (auto const& tool : result.local_command_result->tool_timeline)
          presentation_state_.publish_tool(tool, TuiToolIndexOrigin::LocalCommand);
    }
    // Capture the final decision while operation flags still exist. Retirement below
    // can then destroy all per-run values without losing signal or I/O evidence.
    auto const outcome = operation_->terminal_signal_received || operation_->terminal_write_failed || operation_->completion_failed
                             ? exit_outcome()
                             : settle_completion(result, session_changed);
    retire_operation();
    return outcome;
  }
  catch (...)
  {
    shutdown();
    throw;
  }
}

void ActiveRun::retire_operation()
{
  spinner_.stop();
  prompt_coordinator_.fail_pending_requests();
  static_cast<void>(prompt_coordinator_.cancel_active(PromptCancelReason::Shutdown));
  prompt_coordinator_.set_audit_sink(nullptr);
  plugin_ui_.finish_submission();
  if (operation_->inbox)
    operation_->inbox->retire();
  presentation_state_.publish_run_frame(false);
  operation_.reset();
}

ActiveRunOutcome ActiveRun::shutdown() noexcept
{
  if (!active())
    return exit_outcome();
  auto outcome = exit_outcome();
  // Cleanup runs synchronously on the TUI thread. Preserve failure even in a
  // release build, and log only a fixed stage: exception text may contain input.
  auto cleanup = [&outcome](char const* stage, auto&& operation) noexcept {
    try
    {
      operation();
      return true;
    }
    catch (...)
    {
      outcome = outcome.with_cleanup_failure();
      std::fprintf(stderr, "TUI shutdown cleanup failed at %s\n", stage);
      return false;
    }
  };
  spinner_.stop();
  // Attempt every independent release before deciding whether joining is safe.
  // Retire callbacks that borrow worker data, and release prompt/plugin waiters.
  bool released = cleanup("stop request", [this] {
    if (operation_->inbox)
      static_cast<void>(operation_->inbox->request_stop());
  });
  released &= cleanup("queued prompts", [this] { prompt_coordinator_.fail_pending_requests(); });
  released &= cleanup("active prompt", [this] { static_cast<void>(prompt_coordinator_.cancel_active(PromptCancelReason::Shutdown)); });
  released &= cleanup("plugin waiters", [this] { plugin_ui_.finish_submission(); });
  // A failed release can leave a worker blocked forever. After logging and
  // recording failure, terminate instead of joining that worker. An enclosing
  // completion catch may still hold private exception text, so use an abort-only
  // handler. Change the process handler only when termination is already required.
  if (!released)
  {
    std::set_terminate([]() noexcept { std::abort(); });
    std::terminate();
  }
  cleanup("worker join", [this] {
    if (operation_->worker.valid())
      operation_->worker.get();
  });
  cleanup("queue receipts", [this] { finish_queues(); });
  // A returned error does not enter cleanup's catch. Preserve it alongside any
  // earlier exception failure before resetting the operation below.
  if (operation_->completion_failed)
    outcome = outcome.with_cleanup_failure();
  cleanup("audit retirement", [this] { prompt_coordinator_.set_audit_sink(nullptr); });
  cleanup("inbox retirement", [this] {
    if (operation_->inbox)
      operation_->inbox->retire();
  });
  // The worker has returned, so later cleanup failures cannot strand it. Keep
  // every failure recorded in the outcome before destroying per-run state.
  presentation_state_.publish_run_frame(false);
  operation_.reset();
  return outcome;
}

RuntimeEventDrainResult ActiveRun::drain_events(std::size_t max_events)
{
  // Reduce before updating the transcript: command events can update the sidebar
  // even while conversation text awaits the backend's ordinary-turn request IDs.
  auto batch = operation_->inbox->drain(max_events);
  auto const& snapshot = presentation_state_.screen();
  operation_->events->apply_batch(batch, presentation_state_);
  auto projection = operation_->events->take_transcript_projection();
  if (!projection)
  {
    if (batch.empty())
      return RuntimeEventDrainResult::NoEvents;
    {
      apply_non_transcript_event_authority();
    }
    return renderer_.request_render() ? RuntimeEventDrainResult::UpdatedNoRender : RuntimeEventDrainResult::RenderFailed;
  }
  if (operation_->is_command_submission)
  {
    for (auto const& item : *projection)
      if (item.tool)
        presentation_state_.publish_tool(*item.tool, TuiToolIndexOrigin::Provider);
  }
  {
    // Preserve viewport/search positions around transcript replacement and cap eviction.
    // Only after those owners agree on indices may a frame be requested.
    auto const update = renderer_.begin_transcript_update();
    apply_assistant_turn_meta(*projection, assistant_meta_for_snapshot(&frontend_, snapshot, std::chrono::steady_clock::now() - operation_->turn_started_at),
                              snapshot.thinking_visible);
    auto const capped = presentation_state_.publish_run_transcript(*operation_->events, std::move(*projection));
    auto const changed =
        operation_->events->baseline().size() > capped.leading_evictions ? operation_->events->baseline().size() - capped.leading_evictions : std::size_t{0};
    transcript_search_.refresh_after_transcript_mutation(capped.item_index_shift, changed);
    apply_non_transcript_event_authority();
    renderer_.complete_transcript_update(update, capped.item_index_shift, batch.size());
  }
  if (subagent_workspace_.active() && !snapshot.permission_prompt && !snapshot.question_prompt)
    return RuntimeEventDrainResult::UpdatedNoRender;
  if (renderer_.transcript().scroll_offset() > 0 && !snapshot.permission_prompt && !snapshot.question_prompt && !snapshot.select_list)
    return RuntimeEventDrainResult::UpdatedNoRender;
  return renderer_.request_render() ? RuntimeEventDrainResult::UpdatedNoRender : RuntimeEventDrainResult::RenderFailed;
}
ActiveRunOutcome ActiveRun::settle_completion(TuiSubmitResult const& result, bool session_changed)
{
  // Filter command events by ordinary-turn request ID before choosing fallback
  // text. The remaining events can still supply local command output.
  auto const& snapshot = presentation_state_.screen();
  auto const& event_state = operation_->events->aggregate();
  auto const events_received = operation_->is_command_submission ? operation_->events->authorized_events_received() : operation_->inbox->received_any();
  auto settlement_event_state = operation_->events->settlement_state();
  auto const conversation_committed = operation_->events->conversation_committed();
  auto local_output = result.local_command_result ? result.local_command_result->output : std::vector<std::string>{};
  auto local_tools = result.local_command_result ? result.local_command_result->tool_timeline : std::vector<ToolTimelineItem>{};
  if (operation_->is_command_submission && !conversation_committed && !result.local_command_result)
  {
    local_output = result.output;
    local_tools = result.tool_timeline;
  }
  auto settlement = settle_tui_submission(operation_->events->baseline(), settlement_event_state, operation_->submitted, operation_->is_command_submission,
                                          conversation_committed, std::move(local_output), std::move(local_tools), operation_->inbox->stop_requested(),
                                          result.local_command_result.has_value());
  if (settlement.policy.preserve_transcript && !session_changed)
  {
    // A command with only local output must leave the conversation unchanged.
    // Restore the exact pre-submit sequence before presenting its local result.
    presentation_state_.publish_transcript(std::move(settlement.transcript));

    presentation_state_.publish_run_summary(event_state, result.tool_timeline);
  }
  {
    settle_turn_activity();
  }
  // Use final backend payloads only when no eligible streamed evidence arrived.
  // Do not duplicate bounded Bash output already retained in its tool card.
  if (settlement.policy.project_conversation && !events_received)
  {
    auto const turn_elapsed = std::chrono::steady_clock::now() - operation_->turn_started_at;
    auto const assistant_meta = assistant_meta_for_snapshot(&frontend_, snapshot, turn_elapsed);
    auto const transcript_size_before_fallback = snapshot.transcript.size();
    std::ptrdiff_t item_index_shift = 0;
    if (!operation_->is_command_submission && !session_changed)
    {
      item_index_shift += presentation_state_.append_transcript(TranscriptItem{.label = "you", .text = operation_->submitted});
    }
    if (operation_->inbox->stop_requested() && !session_changed)
    {
      item_index_shift += presentation_state_.append_fallback_outputs({"stopped by user"}, assistant_meta);
    }
    else
    {
      auto const& conversation_tools = operation_->is_command_submission ? result.conversation_tool_timeline : result.tool_timeline;
      auto const& conversation_output = operation_->is_command_submission ? result.conversation_output : result.output;
      for (auto const& tool : conversation_tools)
      {
        presentation_state_.publish_tool(tool, TuiToolIndexOrigin::Provider);
        item_index_shift += presentation_state_.append_transcript(TranscriptItem{.tool = tool});
      }
      auto const bounded_bash_output_is_in_card =
          std::ranges::any_of(conversation_tools, [](auto const& tool) { return tool.name == "bash" && tool.truncated && !tool.spill_path.empty(); });
      if (!bounded_bash_output_is_in_card)
        item_index_shift += presentation_state_.append_fallback_outputs(conversation_output, assistant_meta);
    }
    auto const shifted_first_new_item = static_cast<std::ptrdiff_t>(transcript_size_before_fallback) + item_index_shift;
    auto const changed_from_item_index = shifted_first_new_item > 0 ? static_cast<std::size_t>(shifted_first_new_item) : std::size_t{0};
    transcript_search_.refresh_after_transcript_mutation(item_index_shift, std::min(changed_from_item_index, snapshot.transcript.size()));
    renderer_.note_transcript_eviction(item_index_shift);
  }
  // Update the chosen output surface after the transcript; only then finish
  // processing metadata and request the final frame used by the next idle turn.
  // Local completion already chooses feedback; only other completions replace it.
  std::optional<std::string> final_status;
  if (settlement.policy.present_command_output)
  {
    presentation_state_.publish_local_completion(operation_->submitted, std::move(settlement.command_output), std::move(settlement.command_tools), false);
  }
  else
  {
    if (!events_received && !transcript_search_.is_open())
      renderer_.show_live_tail(LiveTailReason::Requested);
    final_status = events_received ? (event_state.run_status == TuiEventRunStatus::Error      ? "error"
                                      : event_state.run_status == TuiEventRunStatus::Canceled ? "stopped"
                                                                                              : "done")
                                   : (result.output.empty() ? "ok" : "done");
  }
  presentation_state_.publish_run_frame(false, std::nullopt, std::move(final_status));
  presentation_state_.refresh_prompt_metadata();
  if (mermaid_.service() == MermaidServiceOutcome::RenderFailed || !render())
  {
    operation_->terminal_write_failed = true;
    return exit_outcome();
  }

  return operation_->close_after_submit || result.quit ? ActiveRunOutcome::close_session() : ActiveRunOutcome::continue_session();
}
// TUI thread with an active queue capability: delegate only to the backend-provided
// command handler. Absence means ordinary routing must decide what happens next.
std::optional<std::vector<std::string>> dispatch_tui_active_nonblocking_command(TuiActiveRunQueues const& queues, std::string const& submitted)
{
  return queues.run_nonblocking_command ? queues.run_nonblocking_command(submitted) : std::nullopt;
}

// TUI thread: reject a disabled visible completion before invoking the backend
// capability, so typed text cannot bypass the palette restriction.
TuiActiveNonblockingCommandDispatchResult dispatch_tui_active_nonblocking_command_gated(ComposerSnapshot const& completion_snapshot,
                                                                                        TuiActiveRunQueues const& queues, std::string const& submitted)
{
  if (auto const disabled_status = detail::disabled_visible_completion_selection_status(completion_snapshot))
  {
    return TuiActiveNonblockingCommandDispatchResult{.kind = TuiActiveNonblockingCommandDispatchKind::Blocked, .status = *disabled_status};
  }
  auto output = dispatch_tui_active_nonblocking_command(queues, submitted);
  if (!output)
    return {};
  return TuiActiveNonblockingCommandDispatchResult{.kind = TuiActiveNonblockingCommandDispatchKind::Handled, .output = std::move(*output)};
}

}  // namespace ava::tui
