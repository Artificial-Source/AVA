// Presents asynchronous parent-summary progress through frontend snapshots.
// See docs/engineering/tui-architecture.md for the owner relationships.
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

std::optional<BranchSummaryOutcome> BranchSummaryOperation::service_if_due()
{
  if (!std::exchange(discovery_due_, false))
    return std::nullopt;
  return poll();
}

void BranchSummaryOperation::discover_progress(TuiTimer::Clock::time_point)
{
  // Repeated ticks can share one snapshot read in the next application stage.
  discovery_due_ = true;
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
  // Keep a usable checkpoint even if a post-commit catalog refresh fails. Refresh is
  // synchronous on the TUI thread and never changes the backend operation outcome.
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
  // Restore the view and status before clearing the active flag and canceling the timer.
  // Allow exit only after this cleanup; the app remains responsible for joining its worker.
  auto const exit_requested = exit_requested_;
  restore_selector(std::move(restored));
  present_status(refresh_failed ? std::string(branch_summary_callback_failure_status(runtime_views::BranchSummaryCallbackFailure::Refresh))
                                : runtime_views::branch_summary_terminal_status(operation));
  active_ = false;
  discovery_due_ = false;
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
  // The backend may expose a newer/older operation; only this start generation may
  // replace the modal or release its exit gate.
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
    // Snapshot failure finishes only the modal. It cannot prove worker termination,
    // so app-owned callback dependencies must still survive coordinator shutdown.
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
  // Do not expose a new modal generation until preparation succeeds. The previous
  // selector remains usable when the callback refuses or throws.
  discovery_due_ = false;
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
  // Save the prior selector before reading a snapshot that may already report completion;
  // consume may immediately restore that selector without needing later timer checks.
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
  // Exit intent is sticky even if cancellation is rejected or throws. Cancellation only
  // requests worker cooperation; input remains captured until the modal finishes.
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
  // Send consent for this generation only while awaiting confirmation. A refused
  // callback leaves the modal active so later snapshots can still report completion.
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
