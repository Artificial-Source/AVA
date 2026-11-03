#include "sys.h"
#include "BranchSummaryOperation.h"
#include "SelectorHost.h"
#include "TuiEventLoop.h"
#include "TuiFrontEnd.h"
#include "runtime_render_internal.h"
#include "runtime_views_internal.h"
#include "terminal/Context.h"

#include <chrono>
#include <utility>

namespace ava::tui {

BranchSummaryOperation::BranchSummaryOperation(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors,
                                               RuntimeRenderer& renderer, terminal::Context& terminal_context)
    : frontend_(frontend), presentation_state_(presentation), selectors_(selectors), renderer_(renderer), terminal_context_(terminal_context)
{
}

void BranchSummaryOperation::attach_loop(TuiEventLoop& loop)
{
  discovery_timer_ = loop.member_timer(*this, &BranchSummaryOperation::discover_progress);
  if (active_)
    discovery_timer_.repeat(std::chrono::milliseconds{250});
}

std::optional<BranchSummaryOutcome> BranchSummaryOperation::take_service_outcome()
{
  return std::exchange(service_outcome_, std::nullopt);
}

void BranchSummaryOperation::discover_progress(TuiTimer::Clock::time_point)
{
  auto const outcome = poll();
  // A terminal/failure result must survive another due batch until composition
  // consumes it. Settlement cancels discovery before this callback returns.
  if (!service_outcome_ || (*service_outcome_ != BranchSummaryOutcome::RenderFailed && *service_outcome_ != BranchSummaryOutcome::ExitReady))
    service_outcome_ = outcome;
}

bool BranchSummaryOperation::active() const noexcept
{
  return active_;
}

BranchSummaryOutcome BranchSummaryOperation::handle_input(InputEvent const& input)
{
  if (!active_)
    return BranchSummaryOutcome::Inactive;
  switch (runtime_views::branch_summary_input_intent(phase_, input, frontend_.key_bindings()))
  {
    using runtime_views::BranchSummaryInputIntent;
    case BranchSummaryInputIntent::Exit:
      return cancel(BranchSummaryCancelMode::ExitAfterSettlement);
    case BranchSummaryInputIntent::Cancel:
      return cancel(BranchSummaryCancelMode::ContinueSession);
    case BranchSummaryInputIntent::Confirm:
      return confirm();
    case BranchSummaryInputIntent::Block:
      terminal_context_.beep();
      return BranchSummaryOutcome::Pending;
  }
  return BranchSummaryOutcome::Pending;
}

void BranchSummaryOperation::restore_selector(SelectListView view)
{
  selectors_.open(ActiveSelectList::Session,
                  runtime_views::restore_branch_summary_session_view(std::move(view), prior_session_view_, selected_source_value_, prior_selected_index_));
}

void BranchSummaryOperation::present_status(std::string status)
{
  presentation_state_.publish_status(status);
  selectors_.set_subtitle(std::move(status));
}

BranchSummaryOutcome BranchSummaryOperation::settle(TuiBranchSummarySnapshot const& operation)
{
  auto restored = prior_session_view_;
  bool refresh_failed = false;
  if (operation.refresh_required)
  {
    try
    {
      auto refreshed = frontend_.on_branch_summary_refresh_catalog();
      if (refreshed)
        restored = std::move(*refreshed);
      else
        refresh_failed = true;
    }
    catch (...)
    {
      refresh_failed = true;
    }
  }
  auto const exit_requested = exit_requested_;
  restore_selector(std::move(restored));
  present_status(refresh_failed ? std::string(branch_summary_callback_failure_status(runtime_views::BranchSummaryCallbackFailure::Refresh))
                                : runtime_views::branch_summary_terminal_status(operation));
  active_ = false;
  discovery_timer_.cancel();
  phase_ = operation.phase;
  if (exit_requested)
  {
    selectors_.close();
    return BranchSummaryOutcome::ExitReady;
  }
  return renderer_.request_render() ? BranchSummaryOutcome::Settled : BranchSummaryOutcome::RenderFailed;
}

BranchSummaryOutcome BranchSummaryOperation::consume(TuiBranchSummarySnapshot const& operation)
{
  if (!active_)
    return BranchSummaryOutcome::Inactive;
  if (operation.generation != generation_)
    return BranchSummaryOutcome::Pending;
  phase_ = operation.phase;
  if (runtime_views::branch_summary_terminal(operation))
    return settle(operation);
  selectors_.open(ActiveSelectList::BranchSummary, runtime_views::branch_summary_operation_view(operation));
  return renderer_.request_render() ? BranchSummaryOutcome::Pending : BranchSummaryOutcome::RenderFailed;
}

BranchSummaryOutcome BranchSummaryOperation::poll()
{
  if (!active_)
    return BranchSummaryOutcome::Inactive;
  try
  {
    return consume(frontend_.branch_summary_snapshot());
  }
  catch (...)
  {
    TuiBranchSummarySnapshot failed;
    failed.generation = generation_;
    failed.phase = TuiBranchSummaryPhase::Failed;
    failed.failure_code = TuiBranchSummaryFailureCode::Internal;
    auto settled = settle(failed);
    present_status(std::string(branch_summary_callback_failure_status(runtime_views::BranchSummaryCallbackFailure::Snapshot)));
    return settled;
  }
}

BranchSummaryOutcome BranchSummaryOperation::start(SelectListView session_view, std::size_t selected_index, std::string selected_value)
{
  service_outcome_.reset();
  ava::core::Result<TuiBranchSummarySnapshot> prepared =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "parent summary prepare callback failed"));
  try
  {
    prepared = frontend_.on_branch_summary_prepare(selected_value);
  }
  catch (...)
  {
  }
  if (!prepared)
  {
    present_status(std::string(branch_summary_callback_failure_status(runtime_views::BranchSummaryCallbackFailure::Prepare)));
    return renderer_.request_render() ? BranchSummaryOutcome::Settled : BranchSummaryOutcome::RenderFailed;
  }
  active_ = true;
  exit_requested_ = false;
  generation_ = prepared->generation;
  phase_ = prepared->phase;
  prior_session_view_ = std::move(session_view);
  prior_selected_index_ = selected_index;
  selected_source_value_ = std::move(selected_value);
  auto const outcome = consume(*prepared);
  if (active_)
    discovery_timer_.repeat(std::chrono::milliseconds{250});
  return outcome;
}

BranchSummaryOutcome BranchSummaryOperation::cancel(BranchSummaryCancelMode mode)
{
  if (!active_)
    return BranchSummaryOutcome::Inactive;
  exit_requested_ = exit_requested_ || mode == BranchSummaryCancelMode::ExitAfterSettlement;
  ava::core::Result<bool> canceled = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "cancel callback unavailable"));
  try
  {
    canceled = frontend_.on_branch_summary_cancel(generation_);
  }
  catch (...)
  {
  }
  if (!canceled || !*canceled)
  {
    present_status(std::string(branch_summary_callback_failure_status(runtime_views::BranchSummaryCallbackFailure::Cancel)));
    return renderer_.request_render() ? BranchSummaryOutcome::Pending : BranchSummaryOutcome::RenderFailed;
  }
  presentation_state_.publish_status("canceling parent summary");
  return poll();
}

BranchSummaryOutcome BranchSummaryOperation::confirm()
{
  if (!active_)
    return BranchSummaryOutcome::Inactive;
  if (phase_ != TuiBranchSummaryPhase::AwaitingConfirmation)
    return BranchSummaryOutcome::Pending;
  ava::core::Result<bool> confirmed = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "confirm callback unavailable"));
  try
  {
    confirmed = frontend_.on_branch_summary_confirm(generation_);
  }
  catch (...)
  {
  }
  if (!confirmed || !*confirmed)
  {
    present_status(std::string(branch_summary_callback_failure_status(runtime_views::BranchSummaryCallbackFailure::Confirm)));
    return renderer_.request_render() ? BranchSummaryOutcome::Pending : BranchSummaryOutcome::RenderFailed;
  }
  presentation_state_.publish_status("parent summary generation started");
  return poll();
}

} // namespace ava::tui
