#include "sys.h"
#include "PendingAttachments.h"
#include "SessionTransitionCoordinator.h"
#include "ava/agent/question.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/ActiveRunEvents.h"
#include "ava/tui/ActiveRunInbox.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/command_output.h"
#include "ava/tui/event_state.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_commands_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/runtime_mermaid_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_plugin_ui_internal.h"
#include "ava/tui/runtime_prompts_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/terminal.h"
#include "ava/permissions/permission.h"
#include "ava/core/Application.h"
#include "ava/core/ids.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
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
bool ActiveRun::apply_composer_result(ComposerInputResult result)
{
  presentation_state_.publish_composer_feedback(result);
  if (result.beep)
    ava::core::Application::instance().terminal_context().beep();
  return result.handled;
}

using Signals = core::Signals;
using runtime_commands::is_compact_command;
using runtime_commands::shell_helper_submission;
using runtime_transcript::apply_assistant_turn_meta;
using runtime_transcript::assistant_meta_for_snapshot;
using runtime_transcript::capture_thinking_expansion;
using runtime_transcript::capture_tool_detail_visibility;
using runtime_transcript::carry_thinking_expansion;
using runtime_transcript::carry_tool_detail_visibility;
using runtime_transcript::push_fallback_assistant_outputs;
using runtime_transcript::push_transcript;

ActiveRun::ActiveRun(TuiEventLoop& loop, TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state, RuntimeRenderer& renderer,
                     RuntimePromptCoordinator& prompt_coordinator, RuntimePromptStashController& prompt_stash, RuntimePluginUiCoordinator& plugin_ui,
                     RuntimeNavigationController& navigation, RuntimeActionController& action_controller, TranscriptSearchController& transcript_search,
                     RuntimeSubagentWorkspaceController& subagent_workspace, SessionTransitionCoordinator& transitions,
                     RuntimeMermaidPresentationController& mermaid, PendingAttachments& attachments)
    : frontend_(frontend),
      presentation_state_(presentation_state),
      draft_state_(draft_state),
      renderer_(renderer),
      prompt_coordinator_(prompt_coordinator),
      prompt_stash_(prompt_stash),
      plugin_ui_(plugin_ui),
      navigation_(navigation),
      action_controller_(action_controller),
      transcript_search_(transcript_search),
      subagent_workspace_(subagent_workspace),
      transitions_(transitions),
      mermaid_(mermaid),
      attachments_(attachments),
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
  if (inbox_->stop_requested() || events_->aggregate().run_status == TuiEventRunStatus::Canceled)
    presentation_state_.settle_responding_activity(ToolTimelineStatus::Canceled, "assistant stopped");
  else if (events_->aggregate().run_status == TuiEventRunStatus::Error)
    presentation_state_.settle_responding_activity(ToolTimelineStatus::Error, "assistant failed");
  else
    presentation_state_.settle_responding_activity(ToolTimelineStatus::Success, "assistant responded");
}

void ActiveRun::apply_non_transcript_event_authority()
{
  auto const& state = events_->aggregate();
  presentation_state_.publish_queued_messages(state.queued_messages);
  presentation_state_.publish_run_activity(state.activity);
  presentation_state_.publish_run_todos(state.todos);
  if (inbox_->stop_requested() && state.run_status == TuiEventRunStatus::Running)
    upsert_stopping_activity();
  presentation_state_.publish_modified_files(state.modified_files);
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
  ActiveRunTerminal terminal{TuiSubmitResult{}};
  {
    auto owned_submission = std::move(submitted);
    auto owned_context = std::move(context);
    try
    {
      terminal = frontend->on_submit(owned_submission, std::move(owned_context));
    }
    catch (...)
    {
      terminal = std::current_exception();
    }
  }
  // All submission/backend callback scopes have retired. Publishing the payload
  // is the last operation-state action; only resource-safe notify/epilogue follow.
  inbox->publish_terminal(std::move(terminal));
}

bool ActiveRun::start(std::string submitted)
{
  // Finish or shut down the previous submission before starting another one.
  ASSERT(!active());
  if (active())
    return false;
  phase_ = Phase::Running;
  submitted_ = std::move(submitted);
  is_command_submission_ = submitted_.starts_with('/') || shell_helper_submission(submitted_);
  close_after_submit_ = false;
  terminal_write_failed_ = false;
  terminal_signal_received_ = false;
  session_changed_ = false;
  queues_finished_ = false;
  completion_failed_ = false;
  final_projection_serviced_ = false;
  try
  {
    auto const& snapshot = presentation_state_.screen();
    if (is_command_submission_ && runtime_commands::session_switching_command(submitted_))
      attachments_.clear();
    std::vector<ava::session::ImageAttachmentRef> attachments;
    if (!is_command_submission_)
      attachments = attachments_.take();
    inbox_ = std::make_shared<ActiveRunInbox>(wakeup_);
    auto const supports_queue = !is_command_submission_ || is_compact_command(submitted_);
    if (supports_queue)
      active_queues_ = frontend_.create_active_run_queues(inbox_->envelope_sink());
    auto request_id = active_queues_ ? active_queues_->active_request_id : std::string{};
    if (request_id.empty())
      request_id = ava::core::make_id("request");
    else
      active_queues_->mark_follow_up_started = inbox_->wrap_follow_up_started(std::move(active_queues_->mark_follow_up_started));
    inbox_->set_request_id(request_id);
    events_ = std::make_unique<ActiveRunEvents>(is_command_submission_, submitted_, request_id, snapshot.transcript, presentation_state_.sidebar().todos);
    auto endpoint = plugin_ui_.begin_submission(submitted_, request_id);
    prompt_coordinator_.set_audit_sink(inbox_->event_sink());
    turn_started_at_ = std::chrono::steady_clock::now();
    draft_state_.record_submission(submitted_);
    presentation_state_.publish_status(is_command_submission_ ? "running command..." : "thinking...");
    presentation_state_.publish_processing(true);
    if (!render())
    {
      terminal_write_failed_ = true;
      shutdown();
      return false;
    }
    prompt_coordinator_.open_admission();
    // The plugin owner and its synchronized deadline state outlive this joined
    // worker. This predicate has no projection or terminal access.
    auto cancel_requested = [inbox = inbox_, plugin = &plugin_ui_]() { return inbox->stop_requested() || plugin->deadline_reached(); };
    TuiSubmitContext context{
        .request_id = std::move(request_id),
        .permission_resolver = prompt_coordinator_.permission_resolver(),
        .question_resolver = prompt_coordinator_.question_resolver(),
        .event_sink = inbox_->event_sink(),
        .cancel_requested = std::move(cancel_requested),
        .take_steering_messages = active_queues_ ? active_queues_->take_steering_messages : std::function<ava::core::Result<std::vector<std::string>>()>{},
        .skip_active_steering = active_queues_ ? active_queues_->skip_active_steering : std::function<ava::core::VoidResult(std::string_view)>{},
        .take_next_follow_up = active_queues_ ? active_queues_->take_next_follow_up : std::function<std::optional<TuiQueuedFollowUp>()>{},
        .mark_follow_up_started = active_queues_ ? active_queues_->mark_follow_up_started : std::function<ava::core::VoidResult(TuiQueuedFollowUp const&)>{},
        .on_subagent_launch = inbox_->subagent_launch_sink(),
        .plugin_ui = std::move(endpoint),
        .image_attachments = std::move(attachments),
        .key_bindings = frontend_.key_bindings()};
    worker_ = ava::core::make_async("tui_submit", std::bind_front(&ActiveRun::submit_worker, &frontend_, inbox_, submitted_, std::move(context)));
    spinner_.start(snapshot.spinner_frame);
    return true;
  }
  catch (...)
  {
    shutdown();
    throw;
  }
}

bool ActiveRun::stop_requested() const
{
  return inbox_ && (inbox_->stop_requested() || plugin_ui_.deadline_reached());
}

bool ActiveRun::request_stop()
{
  auto const newly_requested = inbox_->request_stop();
  plugin_ui_.cancel_active();
  prompt_coordinator_.fail_pending_requests();
  if (newly_requested)
    ava::core::Application::instance().terminal_context().beep();
  {
    presentation_state_.publish_status("stop requested");
    upsert_stopping_activity();
  }
  return renderer_.request_render();
}

void ActiveRun::request_close_after_submit()
{
  static_cast<void>(inbox_->request_stop());
  close_after_submit_ = true;
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
    terminal_signal_received_ = true;
  return true;
}

void ActiveRun::before_prompt()
{
  // A host prompt freezes the underlying turn, including spinner phase. Restart
  // from the displayed frame after the claim retires, preserving baseline cadence.
  spinner_.stop();
  transcript_search_.close_before_prompt();
  action_controller_.close_startup_overview();
}

bool ActiveRun::service_prompt()
{
  auto outcome = prompt_coordinator_.service();
  if (!apply_prompt_outcome(outcome))
    return false;
  if (!prompt_coordinator_.has_active())
  {
    outcome = prompt_coordinator_.start_pending(std::bind_front(&ActiveRun::stop_requested, this), std::bind_front(&ActiveRun::before_prompt, this));
    return apply_prompt_outcome(outcome);
  }
  return true;
}

bool ActiveRun::apply_prompt_outcome(PromptServiceOutcome outcome)
{
  if (outcome != PromptServiceOutcome::RequestRunStop)
    return true;
  // Escape retains its permission claim until stop is visible to the worker.
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
    presentation_state_.publish_spinner(frame);
  }
  if (!subagent_workspace_.active() && !renderer_.request_render(FrameRenderKind::Footer))
    terminal_write_failed_ = true;
}

TuiInputOutcome ActiveRun::service_control()
{
  if (!active() || !Signals::received(terminal_signals))
    return TuiInputOutcome::Pass;
  // An active question's automatic answer keeps precedence over signal denial.
  static_cast<void>(prompt_coordinator_.cancel_active(PromptCancelReason::TerminalSignal));
  bool const exit_requested = Signals::try_obtain(SIGTERM);
  bool const claimed_sigint = !exit_requested && Signals::try_obtain(SIGINT);

  // ┌─────────────────────┬────────────────┬────────────────────────────────────────────────────────────────────┐
  // │Successfully claimed │UI state        │Intended action                                                     │
  // ├─────────────────────┼────────────────┼────────────────────────────────────────────────────────────────────┤
  // │SIGTERM              │Any draft state │Request run cancellation and close after submit; leave loop (break) │
  // ├─────────────────────┼────────────────┼────────────────────────────────────────────────────────────────────┤
  // │SIGINT only          │Empty draft     │Request run cancellation and close after submit; leave loop (break) │
  // ├─────────────────────┼────────────────┼────────────────────────────────────────────────────────────────────┤
  // │SIGINT only          │Nonempty draft  │Clear draft, render, and continue; keep the active run running      │
  // └─────────────────────┴────────────────┴────────────────────────────────────────────────────────────────────┘

  if (exit_requested || (claimed_sigint && draft_state_.draft().text.empty()))
    return cancel(ActiveRunCancel::TerminalSignal) ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
  if (claimed_sigint)     // If this is true than draft.text.empty() must be false.
  {
    static_cast<void>(action_controller_.clear_draft_for_interrupt());
    presentation_state_.publish_completion(draft_state_);
    return render() ? TuiInputOutcome::Handled : TuiInputOutcome::Failure;
  }
  return TuiInputOutcome::Pass;
}

RuntimeEventDrainResult ActiveRun::consume(std::size_t max_events)
{
  if (!active())
    return RuntimeEventDrainResult::NoEvents;
  if (terminal_write_failed_ || (phase_ == Phase::Running && (!service_prompt() || !poll_plugin_ui())))
  {
    terminal_write_failed_ = true;
    return RuntimeEventDrainResult::RenderFailed;
  }
  // Do not project provider events beneath a host prompt. Its worker may borrow
  // the current view; queued events resume after the prompt has retired.
  if (prompt_coordinator_.has_active())
    return RuntimeEventDrainResult::NoEvents;
  if (phase_ == Phase::Running && !spinner_.active())
    spinner_.start(presentation_state_.screen().spinner_frame);
  auto const drained = drain_events(max_events);
  if (drained == RuntimeEventDrainResult::RenderFailed)
    terminal_write_failed_ = true;
  if (phase_ == Phase::Finalizing && !inbox_->has_pending())
    final_projection_serviced_ = true;
  return drained;
}

bool ActiveRun::handle_input(runtime_input::RuntimeInput const& input)
{
  if (phase_ != Phase::Running || close_after_submit_)
    return true;
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
    // This binding is synchronous: the coordinator serializes claim-or-search,
    // then drops its mutex before beginning a newly claimed modal operation.
    auto const outcome = prompt_coordinator_.dispatch_search_input_with_prompt_precedence(std::bind_front(&ActiveRun::handle_search_input_event, this, input),
                                                                                          std::bind_front(&ActiveRun::stop_requested, this),
                                                                                          std::bind_front(&ActiveRun::before_prompt, this));
    if (outcome == SearchInputPromptDispatchResult::RequestRunStop)
      return apply_prompt_outcome(PromptServiceOutcome::RequestRunStop);
    return outcome != SearchInputPromptDispatchResult::InputFailed;
  }
  auto const prompt = prompt_coordinator_.start_pending(std::bind_front(&ActiveRun::stop_requested, this), std::bind_front(&ActiveRun::before_prompt, this));
  if (prompt == PromptServiceOutcome::RequestRunStop)
    return apply_prompt_outcome(prompt);
  if (prompt != PromptServiceOutcome::None)
    return true; // A raced claim consumes the retained key; it never enters the new modal.
  return dispatch_active_input(input);
}

bool ActiveRun::accepts_input() const
{
  return phase_ == Phase::Running && !close_after_submit_ && inbox_ && !inbox_->terminal_ready();
}

bool ActiveRun::has_pending_updates()
{
  return active() && ((!prompt_coordinator_.has_active() && (inbox_->has_pending() || prompt_coordinator_.has_pending_requests())) ||
                      (phase_ == Phase::Finalizing && !final_projection_serviced_));
}

void ActiveRun::finish_queues()
{
  if (queues_finished_)
    return;
  queues_finished_ = true;
  if (active_queues_ && active_queues_->finish)
  {
    if (auto finished = active_queues_->finish(inbox_->stop_requested()); !finished)
    {
      presentation_state_.publish_status(finished.error().format());
      completion_failed_ = true;
    }
  }
}

void ActiveRun::begin_completion(TuiSubmitResult result)
{
  result_ = std::move(result);
  prompt_coordinator_.fail_pending_requests();
  static_cast<void>(prompt_coordinator_.cancel_active(PromptCancelReason::Shutdown));
  plugin_ui_.finish_submission();
  worker_.get();
  events_->authorize_receipts(result_->ordinary_turn_request_ids);
  // Read backend identity/transports after commit, before replacing the projection.
  auto committed_session_id = frontend_.read_committed_session_id();
  auto boundary_reads = result_->quit ? RuntimePresentationState::BoundaryReads{} : RuntimePresentationState::read_boundary_data(frontend_);
  {
    session_changed_ = transitions_.apply(std::move(committed_session_id));
    if (!result_->quit)
      presentation_state_.apply_boundary_data(frontend_, std::move(boundary_reads));
  }
  finish_queues();
  // Freeze admission before discarding an old-session backlog: a retained sink
  // cannot repopulate that backlog between discard and final bounded draining.
  inbox_->retire();
  if (session_changed_)
  {
    events_->discard();
    inbox_->discard();
  }
  // Queue finish may publish final receipts. Freeze admission only afterward;
  // subsequent bounded turns project every accepted event before settlement.
  prompt_coordinator_.set_audit_sink(nullptr);
  spinner_.stop();
  phase_ = Phase::Finalizing;
}

std::optional<ActiveRunOutcome> ActiveRun::complete()
{
  if (!active())
    return std::nullopt;
  try
  {
    if (phase_ == Phase::Running)
    {
      auto terminal = inbox_->take_terminal();
      if (!terminal)
        return std::nullopt;
      if (auto error = std::get_if<std::exception_ptr>(&*terminal))
      {
        auto exception = *error;
        shutdown();
        std::rethrow_exception(exception);
      }
      begin_completion(std::move(std::get<TuiSubmitResult>(*terminal)));
      return std::nullopt;
    }
    if (!final_projection_serviced_ || inbox_->has_pending())
      return std::nullopt;
    if (!session_changed_)
    {
      if (is_command_submission_)
      {
        auto local_tools = result_->local_command_result ? result_->local_command_result->tool_timeline : std::vector<ToolTimelineItem>{};
        local_tools = events_->merge_unauthorized_tools(std::move(local_tools));
        if (!local_tools.empty())
        {
          if (!result_->local_command_result)
            result_->local_command_result = TuiLocalCommandResult{};
          result_->local_command_result->tool_timeline = std::move(local_tools);
        }
      }
      if (result_->local_command_result)
      {
        for (auto const& tool : result_->local_command_result->tool_timeline)
          presentation_state_.publish_tool(tool, TuiToolIndexOrigin::LocalCommand);
      }
    }
    auto const outcome = terminal_signal_received_ || terminal_write_failed_ || completion_failed_
                             ? ActiveRunOutcome(true, terminal_write_failed_, terminal_signal_received_)
                             : settle_completion();
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
  if (inbox_)
    inbox_->retire();
  active_queues_.reset();
  result_.reset();
  events_.reset();
  inbox_.reset();
  submitted_.clear();
  presentation_state_.publish_processing(false);
  phase_ = Phase::Idle;
}

ActiveRunOutcome ActiveRun::shutdown() noexcept
{
  if (!active())
    return ActiveRunOutcome(true, terminal_write_failed_, terminal_signal_received_);
  // A destructor must continue independent cleanup after a failed step. Use
  // fixed stage names only: exceptions can contain prompt or command bodies.
  auto cleanup = [](char const* stage, auto&& operation) noexcept {
    try
    {
      operation();
    }
    catch (...)
    {
      try
      {
        Dout(dc::warning, "TUI shutdown cleanup failed at " << stage);
      }
      catch (...)
      {
        // Logging must not turn a recoverable cleanup failure into termination.
      }
    }
  };
  spinner_.stop();
  // Stop admission and retire all callbacks borrowing worker-local data before
  // joining. These cancellation paths avoid allocation; mutex failure remains
  // an unrecoverable platform failure, not a promised liveness guarantee.
  cleanup("stop request", [&] {
    if (inbox_)
      static_cast<void>(inbox_->request_stop());
  });
  cleanup("queued prompts", [&] { prompt_coordinator_.fail_pending_requests(); });
  cleanup("active prompt", [&] { static_cast<void>(prompt_coordinator_.cancel_active(PromptCancelReason::Shutdown)); });
  cleanup("plugin waiters", [&] { plugin_ui_.finish_submission(); });
  cleanup("worker join", [&] {
    if (worker_.valid())
      worker_.get();
  });
  cleanup("queue receipts", [&] { finish_queues(); });
  cleanup("audit retirement", [&] { prompt_coordinator_.set_audit_sink(nullptr); });
  cleanup("inbox retirement", [&] {
    if (inbox_)
      inbox_->retire();
  });
  // Each local value is now independent of worker execution. Do not call the
  // normal retirement sequence again: it would repeat fallible cleanup steps.
  active_queues_.reset();
  result_.reset();
  events_.reset();
  inbox_.reset();
  submitted_.clear();
  presentation_state_.publish_processing(false);
  phase_ = Phase::Idle;
  return ActiveRunOutcome(true, terminal_write_failed_, terminal_signal_received_);
}

RuntimeEventDrainResult ActiveRun::drain_events(std::size_t max_events)
{
  auto batch = inbox_->drain(max_events);
  auto const& snapshot = presentation_state_.screen();
  events_->apply_batch(batch, presentation_state_);
  auto projection = events_->take_transcript_projection();
  if (!projection)
  {
    if (batch.empty())
      return RuntimeEventDrainResult::NoEvents;
    {
      apply_non_transcript_event_authority();
    }
    return renderer_.request_render() ? RuntimeEventDrainResult::UpdatedNoRender : RuntimeEventDrainResult::RenderFailed;
  }
  if (is_command_submission_)
  {
    for (auto const& item : *projection)
      if (item.tool)
        presentation_state_.publish_tool(*item.tool, TuiToolIndexOrigin::Provider);
  }
  {
    auto const update = renderer_.begin_transcript_update();
    apply_assistant_turn_meta(*projection, assistant_meta_for_snapshot(&frontend_, snapshot, std::chrono::steady_clock::now() - turn_started_at_),
                              snapshot.thinking_visible);
    auto const capped = presentation_state_.publish_run_transcript(*events_, std::move(*projection));
    auto const changed = events_->baseline().size() > capped.leading_evictions ? events_->baseline().size() - capped.leading_evictions : std::size_t{0};
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
ActiveRunOutcome ActiveRun::settle_completion()
{
  auto const& snapshot = presentation_state_.screen();
  auto& result = *result_;
  auto const& event_state = events_->aggregate();
  auto const events_received = is_command_submission_ ? events_->authorized_events_received() : inbox_->received_any();
  auto settlement_event_state = events_->settlement_state();
  auto const conversation_committed = events_->conversation_committed();
  auto local_output = result.local_command_result ? result.local_command_result->output : std::vector<std::string>{};
  auto local_tools = result.local_command_result ? result.local_command_result->tool_timeline : std::vector<ToolTimelineItem>{};
  if (is_command_submission_ && !conversation_committed && !result.local_command_result)
  {
    local_output = result.output;
    local_tools = result.tool_timeline;
  }
  auto settlement = settle_tui_submission(events_->baseline(), settlement_event_state, submitted_, is_command_submission_, conversation_committed,
                                          std::move(local_output), std::move(local_tools), inbox_->stop_requested(), result.local_command_result.has_value());
  if (settlement.policy.preserve_transcript && !session_changed_)
  {
    // Local command event projection is withheld for the entire run. Restore the
    // exact pre-submit sequence defensively so no independent receipt path can leak.
    presentation_state_.publish_transcript(std::move(settlement.transcript));

    if (!event_state.activity.empty())
      presentation_state_.publish_run_activity(event_state.activity);
    if (!event_state.todos.empty())
      presentation_state_.publish_run_todos(event_state.todos);
    auto files = event_state.modified_files;
    for (auto const& tool : result.tool_timeline)
      for (auto const& path : tool.changed_paths)
        files.push_back(SidebarModifiedFile{.path = path});
    presentation_state_.publish_modified_files(files);
  }
  {
    settle_turn_activity();
  }
  if (settlement.policy.project_conversation && !events_received)
  {
    auto const turn_elapsed = std::chrono::steady_clock::now() - turn_started_at_;
    auto const assistant_meta = assistant_meta_for_snapshot(&frontend_, snapshot, turn_elapsed);
    auto const transcript_size_before_fallback = snapshot.transcript.size();
    std::ptrdiff_t item_index_shift = 0;
    if (!is_command_submission_ && !session_changed_)
    {
      item_index_shift += presentation_state_.append_transcript(TranscriptItem{.label = "you", .text = submitted_});
    }
    if (inbox_->stop_requested() && !session_changed_)
    {
      item_index_shift += presentation_state_.append_fallback_outputs({"stopped by user"}, assistant_meta);
    }
    else
    {
      auto const& conversation_tools = is_command_submission_ ? result.conversation_tool_timeline : result.tool_timeline;
      auto const& conversation_output = is_command_submission_ ? result.conversation_output : result.output;
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
  if (settlement.policy.present_command_output)
  {
    presentation_state_.publish_local_completion(submitted_, std::move(settlement.command_output), std::move(settlement.command_tools), false);
  }
  else
  {
    if (!events_received && !transcript_search_.is_open())
      renderer_.show_live_tail(LiveTailReason::Requested);
    presentation_state_.publish_status(events_received ? (event_state.run_status == TuiEventRunStatus::Error      ? "error"
                                                          : event_state.run_status == TuiEventRunStatus::Canceled ? "stopped"
                                                                                                                  : "done")
                                                       : (result.output.empty() ? "ok" : "done"));
  }
  presentation_state_.publish_processing(false);
  presentation_state_.refresh_prompt_metadata();
  if (mermaid_.service() == MermaidServiceOutcome::RenderFailed || !render())
  {
    terminal_write_failed_ = true;
    return ActiveRunOutcome(true, true, terminal_signal_received_);
  }

  return ActiveRunOutcome(close_after_submit_ || result.quit, terminal_write_failed_, terminal_signal_received_);
}
std::optional<std::vector<std::string>> dispatch_tui_active_nonblocking_command(TuiActiveRunQueues const& queues, std::string const& submitted)
{
  return queues.run_nonblocking_command ? queues.run_nonblocking_command(submitted) : std::nullopt;
}

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
