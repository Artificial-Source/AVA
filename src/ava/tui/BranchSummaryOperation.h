// Presents asynchronous parent-summary progress through frontend snapshots.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "TuiTimerHandle.h"
#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

class BranchSummaryActions;

class RuntimeRenderer;
class SelectorHost;
class TuiTimerRegistry;
namespace terminal {
class Context;
}

// TUI-thread result consumed by Session; finishing the modal does not transfer worker ownership.
enum class BranchSummaryOutcome
{
  // No summary UI operation is active, so normal input may proceed.
  Inactive,
  // Keep the modal and discovery alive while backend work or confirmation remains.
  Pending,
  // The modal finished or prepare failed. A successfully prepared operation restores its prior list.
  Settled,
  // Modal cleanup permits the requested exit and closes the selector; app teardown still joins work.
  ExitReady,
  // The renderer reports a known failure when a frame is requested; Session must shut down.
  RenderFailed
};

// TUI-thread cancellation intent; neither choice synchronously joins the backend worker.
enum class BranchSummaryCancelMode
{
  // After handling cancellation's result, return to the prior session selector.
  ContinueSession,
  // Remember exit intent until modal cleanup permits Session to stop.
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
  // Frontend and session collaborators outlive this owner; create_timers precedes
  // normal operation, and backend execution remains frontend-owned.
  BranchSummaryOperation(BranchSummaryActions& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                         terminal::Context& terminal_context);
  // Not copyable: the timer targets this address, and one owner restores the prior selector.
  BranchSummaryOperation(BranchSummaryOperation const&) = delete;
  BranchSummaryOperation& operator=(BranchSummaryOperation const&) = delete;

  // TUI thread: register a timer that only marks discovery due. The registry outlives this
  // stable operation and the already-constructed renderer used by service_if_due.
  void create_timers(TuiTimerRegistry& timer_registry);
  // TUI application stage: read one pending backend snapshot, or return no value.
  // Several timer ticks require only one read. The returned outcome is handled before
  // input; the timer itself never reads the frontend or completes the operation.
  [[nodiscard]] std::optional<BranchSummaryOutcome> service_if_due();
  // TUI thread: tell Session whether summary input must capture ordinary editor actions.
  [[nodiscard]] bool active() const noexcept;
  // TUI thread: call with no active summary; prepare backend work and save the list
  // checkpoint before consuming its generation. The attached timer uses a 250 ms interval
  // while active and only marks a read due; application servicing performs the read later.
  [[nodiscard]] BranchSummaryOutcome start(SelectListView session_view, std::size_t selected_index, std::string selected_value);
  // TUI thread: request cooperative cancellation for this generation and latch exit intent.
  // Pending means the worker may still use app callbacks; continue polling until modal cleanup.
  // Finishing the modal does not establish that all worker callbacks have returned.
  [[nodiscard]] BranchSummaryOutcome cancel(BranchSummaryCancelMode mode);
  // TUI thread: route cancel/exit/confirmation for the current phase; other modal input
  // is rejected with a bell and cannot edit the underlying draft.
  [[nodiscard]] BranchSummaryOutcome handle_input(InputEvent const& input);

  // The owned session catalog checkpoint contains user paths and labels; keep them
  // out of generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread: restore the prior query/selection identity into refreshed or checkpoint rows.
  void restore_selector(SelectListView view);
  // TUI thread: keep transient status and open selector subtitle consistent for callback failures.
  void present_status(std::string status);
  // TUI thread: consume a finished-operation snapshot, optionally refresh catalog, restore the
  // checkpoint and cancel discovery before returning. This does not join the app worker.
  [[nodiscard]] BranchSummaryOutcome settle(TuiBranchSummarySnapshot const& operation);
  // TUI thread: ignore stale generations; matching snapshots update or finish the modal.
  // The supplied snapshot is owned by the caller and retained only by value as needed.
  [[nodiscard]] BranchSummaryOutcome consume(TuiBranchSummarySnapshot const& operation);
  // TUI thread: only AwaitingConfirmation submits consent for the current generation;
  // callback rejection preserves the modal and reports failure for continued polling.
  [[nodiscard]] BranchSummaryOutcome confirm();
  // TUI thread: copy a backend snapshot through the frontend; an exception finishes
  // the local UI as failed. Backend shutdown responsibility remains with the app.
  [[nodiscard]] BranchSummaryOutcome poll();
  // TUI timer callback: mark discovery due without calling the frontend. The
  // application stage copies snapshots and applies their result before input.
  void discover_progress(TuiTimerHandle::Clock::time_point now);

  // Frontend-owned narrow summary interface outlives this TUI-thread operation.
  BranchSummaryActions& frontend_;
  // Borrowed from Session; display state; receives status updates for this operation.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; sole modal storage, replaced by progress and restored when finished.
  SelectorHost& selectors_;
  // Session's already-constructed renderer outlives this operation; application
  // servicing requests frames through it after applying a changed snapshot.
  RuntimeRenderer& renderer_;
  // Terminal outlives this owner and supplies TUI-thread modal rejection bells.
  terminal::Context& terminal_context_;
  // TUI-thread active-modal flag, set after successful prepare and cleared when the modal finishes.
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
  // Set by the TUI timer; consumed by application servicing. Starting or
  // finishing an operation clears it so a later generation cannot inherit work.
  bool discovery_due_ = false;
  // TUI-thread repeating discovery timer. Declared last so it unregisters before
  // callback-visible state is destroyed; the timer registry must outlive it.
  TuiTimerHandle discovery_timer_;
};

} // namespace ava::tui
