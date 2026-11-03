// Presents asynchronous parent-summary progress through frontend snapshots.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "TuiTimer.h"
#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

class RuntimeRenderer;
class SelectorHost;
class TuiEventLoop;
namespace terminal {
class Context;
}

// TUI-thread result consumed by Session; terminal UI outcomes do not transfer worker ownership.
enum class BranchSummaryOutcome
{
  // No summary UI operation is active, so normal input may proceed.
  Inactive,
  // Keep the modal and discovery alive while backend work or confirmation remains.
  Pending,
  // The modal has settled or prepare failed. An admitted operation restores its prior list.
  Settled,
  // Local settlement releases requested exit and closes the selector; app teardown still joins work.
  ExitReady,
  // A required render failed; Session must begin its shutdown path.
  RenderFailed
};

// TUI-thread cancellation intent; neither choice synchronously joins the backend worker.
enum class BranchSummaryCancelMode
{
  // After cooperative cancellation settles, return to the prior session selector.
  ContinueSession,
  // Latch exit intent until a terminal snapshot permits Session to stop.
  ExitAfterSettlement
};

// Session owns this TUI-thread summary modal and its timer. The app-owned branch
// summary coordinator owns the worker, mutex and result snapshots. All methods here,
// including timer callbacks, run on the TUI thread; frontend calls copy snapshots or
// submit generation-tagged commands across that boundary. This owner has no worker
// or UI mutex. Its borrowed owners outlive it; its final timer member unregisters
// before the remaining fields die. Cancellation is not a join: the app must stop/join
// the coordinator before destroying anything captured by its worker callbacks.
class BranchSummaryOperation final
{
 public:
  // TUI thread: start inactive without launching work or registering discovery.
  // Frontend and session collaborators outlive this owner; attach_loop precedes
  // normal operation, and backend execution remains frontend-owned.
  BranchSummaryOperation(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                         terminal::Context& terminal_context);
  // A copy could issue commands for the same generation with a different restore checkpoint.
  BranchSummaryOperation(BranchSummaryOperation const&) = delete;
  // Keep the timer callback address and generation owner stable until timer teardown.
  BranchSummaryOperation& operator=(BranchSummaryOperation const&) = delete;

  // The attached loop outlives this stable operation and its registration.
  // Attach the renderer first so discovery requests only batch invalidation.
  void attach_loop(TuiEventLoop& loop);
  // TUI thread: consume the timer result once, after discovery and before normal dispatch.
  // Terminal/failure results remain latched until taken.
  [[nodiscard]] std::optional<BranchSummaryOutcome> take_service_outcome();
  // TUI thread: tell Session whether summary input must capture ordinary editor actions.
  [[nodiscard]] bool active() const noexcept;
  // TUI thread: call with no active summary; prepare backend work and save the list
  // checkpoint before consuming its generation. After attach_loop, active discovery polls every 250 ms.
  [[nodiscard]] BranchSummaryOutcome start(SelectListView session_view, std::size_t selected_index, std::string selected_value);
  // TUI thread: request cooperative cancellation for this generation and latch exit intent.
  // Pending means the worker may still use app callbacks; continue polling until local settlement.
  // Even a settled UI result does not establish that all worker callbacks have returned.
  [[nodiscard]] BranchSummaryOutcome cancel(BranchSummaryCancelMode mode);
  // TUI thread: route cancel/exit/confirmation for the current phase; other modal input
  // is rejected with a bell and cannot edit the underlying draft.
  [[nodiscard]] BranchSummaryOutcome handle_input(InputEvent const& input);

  // Retains a session catalog checkpoint plus arbitrary presentation/renderer
  // aggregates; generated printing must not expose those message/path contents.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread: restore the prior query/selection identity into refreshed or checkpoint rows.
  void restore_selector(SelectListView view);
  // TUI thread: keep transient status and open selector subtitle consistent for callback failures.
  void present_status(std::string status);
  // TUI thread: consume a terminal snapshot, optionally refresh catalog, restore the
  // checkpoint and cancel discovery before returning. This does not join the app worker.
  [[nodiscard]] BranchSummaryOutcome settle(TuiBranchSummarySnapshot const& operation);
  // TUI thread: ignore stale generations; matching snapshots update phase/view or settle.
  // The supplied snapshot is owned by the caller and retained only by value as needed.
  [[nodiscard]] BranchSummaryOutcome consume(TuiBranchSummarySnapshot const& operation);
  // TUI thread: only AwaitingConfirmation submits consent for the current generation;
  // callback rejection preserves the modal and reports failure for continued polling.
  [[nodiscard]] BranchSummaryOutcome confirm();
  // TUI thread: copy a backend snapshot through frontend transport; an exception settles
  // the local UI as failed. Backend shutdown responsibility remains with the app.
  [[nodiscard]] BranchSummaryOutcome poll();
  // TUI timer callback: poll once and latch an outcome; rendering requests join the current
  // loop batch. It never runs on the branch-summary worker.
  void discover_progress(TuiTimer::Clock::time_point now);

  // App frontend outlives this owner; callbacks submit coordinator commands or copy snapshots.
  TuiFrontEnd& frontend_;
  // Borrowed from Session; TUI projection; all feedback is published through named operations.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; sole modal storage, replaced by progress and restored after settlement.
  SelectorHost& selectors_;
  // Borrowed from Session; renderer; attach its loop before discovery can request batched rendering.
  RuntimeRenderer& renderer_;
  // Terminal outlives this owner and supplies TUI-thread modal rejection bells.
  terminal::Context& terminal_context_;
  // TUI-thread admission flag set after successful prepare and cleared by local settlement.
  bool active_ = false;
  // TUI-thread sticky exit intent for this operation; cancellation failure does not erase it.
  bool exit_requested_ = false;
  // TUI-thread backend generation accepted by start; guards against stale snapshots and commands.
  std::uint64_t generation_ = 0;
  // TUI-thread latest matching phase; controls whether Enter can confirm the summary.
  TuiBranchSummaryPhase phase_ = TuiBranchSummaryPhase::Idle;
  // TUI-thread owned checkpoint for cancellation/failure or catalog-refresh failure.
  SelectListView prior_session_view_;
  // TUI-thread fallback row when restoration cannot find the original selected identity.
  std::size_t prior_selected_index_ = 0;
  // TUI-thread source session identity, copied before the prior list is replaced.
  ava::core::NoLogString selected_source_value_;
  // TUI-thread timer-to-Session handoff; start clears it and take_service_outcome consumes it.
  std::optional<BranchSummaryOutcome> service_outcome_;
  // TUI-thread repeating discovery registration. Declared last so it unregisters before
  // callback-visible state is destroyed; the attached loop must outlive it.
  TuiTimer discovery_timer_;
};

} // namespace ava::tui
