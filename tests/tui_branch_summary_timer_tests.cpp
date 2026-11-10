// Branch-summary discovery publishes a due flag; application work runs only when serviced.
#include "sys.h"
#include "support/test_harness.h"
#include "support/tui_test_support.h"
#include "ava/tui/BranchSummaryOperation.h"
#include "ava/tui/Composer.h"
#include "ava/tui/SelectorHost.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/TuiTimeoutHandler.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/core/Application.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ava::tui::ActiveSelectList;
using ava::tui::BranchSummaryCancelMode;
using ava::tui::BranchSummaryOutcome;
using ava::tui::TuiBranchSummaryPhase;
using Clock = ava::tui::TuiTimeoutHandler::Clock;

// Return an owned session checkpoint whose selected identity survives a reordered refresh.
ava::tui::SelectListView summary_session_view()
{
  ava::tui::SelectListView view;
  view.title = "Summary timer sessions";
  view.items.resize(2);
  view.items[0].value = "first";
  view.items[0].label = "TIMER-FIRST";
  view.items[1].value = "second";
  view.items[1].label = "TIMER-SECOND";
  view.selected_item_index = 1;
  view.query = "TIMER";
  return view;
}

// Test-thread backend fixture record; its enclosing owner keeps every hook capture alive.
struct SummaryBackendFixture
{
  // Controlled backend publication, copied by prepare and snapshot hooks without a worker.
  ava::tui::TuiBranchSummarySnapshot operation{
      .generation = 41, .phase = TuiBranchSummaryPhase::Generating, .source_label = "timer source", .model_label = "timer model"};
  // Count application snapshot calls independently of timer dispatch.
  std::size_t snapshot_reads = 0;
  // Count catalog work so terminal discovery cannot hide synchronous refreshes.
  std::size_t refreshes = 0;
  // Count cooperative requests separately from eventual settlement.
  std::size_t cancellations = 0;
  // Record the generation sent through the cancellation boundary.
  std::uint64_t canceled_generation = 0;
  // Configure acceptance without making cancellation itself complete backend work.
  bool accept_cancellation = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Own offline hooks and real TUI owners on one thread. Reverse destruction retires
// summary and renderer timers before the loop and hook captures.
class SummaryTimerFixture final
{
 public:
  // Establish borrowed dependencies before registering callbacks; the common
  // runner owns the initialized process terminal for this fixture's lifetime.
  SummaryTimerFixture()
      : presentation_(frontend_.frontend(), {}),
        renderer_(tui_timeout_handler_, frontend_.frontend(), presentation_, composer_, &ava::core::Application::instance().terminal_context()),
        selectors_(presentation_),
        summary_(*frontend_.frontend(), presentation_, selectors_, renderer_, ava::core::Application::instance().terminal_context())
  {
    presentation_.publish_dimensions(100, 24);
    summary_.create_timers(tui_timeout_handler_);
    frontend_.hooks().on_branch_summary_prepare = [this](std::string_view source) {
      expect(source == "second", "timer fixture prepares the selected session identity");
      return ava::core::Result<ava::tui::TuiBranchSummarySnapshot>{backend_.operation};
    };
    frontend_.hooks().branch_summary_snapshot = [this] {
      ++backend_.snapshot_reads;
      return backend_.operation;
    };
    frontend_.hooks().on_branch_summary_refresh_catalog = [this] {
      ++backend_.refreshes;
      auto refreshed = summary_session_view();
      std::swap(refreshed.items[0], refreshed.items[1]);
      return ava::core::Result<ava::tui::SelectListView>{std::move(refreshed)};
    };
    frontend_.hooks().on_branch_summary_cancel = [this](std::uint64_t generation) {
      ++backend_.cancellations;
      backend_.canceled_generation = generation;
      return ava::core::Result<bool>{backend_.accept_cancellation};
    };
  }

  // Open the checkpoint and start a fresh generation only while the summary is inactive.
  [[nodiscard]] BranchSummaryOutcome start()
  {
    auto prior = summary_session_view();
    selectors_.open(ActiveSelectList::Session, prior);
    return summary_.start(std::move(prior), 1, "second");
  }
  // Borrow fixture-owned callback data only on this test thread.
  SummaryBackendFixture& backend() { return backend_; }
  // Borrow the real operation until this fixture retires its discovery handle.
  ava::tui::BranchSummaryOperation& summary() { return summary_; }
  // Borrow the registry for explicit synthetic deadline dispatch, without waiting.
  ava::tui::TuiTimeoutHandler& tui_timeout_handler() { return tui_timeout_handler_; }
  // Observe which selector is active before and after application servicing.
  ava::tui::SelectorHost const& selectors() const { return selectors_; }
  // Render the projected state for comparison, without discovery hooks. This
  // observes pending screen state, not output from an actual terminal paint.
  [[nodiscard]] std::vector<std::string> frame() { return ava::tui::render_composer(frontend_.frontend(), presentation_.screen()); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Keep controlled publications and counters alive until frontend hooks are destroyed.
  SummaryBackendFixture backend_;
  // Own the registered offline frontend until every TUI borrower retires.
  tui_test_support::TuiFrontEndFixture frontend_;
  // Outlive every timer, including the renderer's frame callbacks.
  ava::tui::TuiTimeoutHandler tui_timeout_handler_;
  // Own the screen projection read by selector and frame assertions.
  ava::tui::RuntimePresentationState presentation_;
  // Supply the renderer's stable editor dependency through renderer destruction.
  ava::tui::Composer composer_;
  // Use production invalidation behavior without painting from timer callbacks.
  ava::tui::RuntimeRenderer renderer_;
  // Own the selected modal kind and its published session checkpoint.
  ava::tui::SelectorHost selectors_;
  // Retire summary discovery before destroying its borrowed collaborators.
  ava::tui::BranchSummaryOperation summary_;
};

// Repeated timer batches coalesce without snapshot or refresh calls; one service
// consumes progress, and later terminal servicing restores the refreshed selector.
void test_summary_timer_defers_snapshot_and_refresh()
{
  SummaryTimerFixture fixture;
  auto& backend = fixture.backend();
  expect(fixture.start() == BranchSummaryOutcome::Pending && fixture.summary().active(), "summary starts with deferred discovery");
  expect(!fixture.summary().service_if_due() && backend.snapshot_reads == 0, "summary does not poll before any discovery deadline");
  auto const initial_frame = fixture.frame();
  backend.operation.model_label = "updated timer model";

  // Capture after start so both explicit future deadlines necessarily cross the
  // production 250 ms cadence; no wall-clock wait or polling loop is involved.
  auto const first_deadline = Clock::now() + std::chrono::seconds{1};
  fixture.tui_timeout_handler().expire(first_deadline);
  fixture.tui_timeout_handler().expire(first_deadline + std::chrono::seconds{1});
  expect(backend.snapshot_reads == 0 && backend.refreshes == 0 && fixture.summary().active() && fixture.selectors().kind() == ActiveSelectList::BranchSummary &&
             fixture.frame() == initial_frame,
         "discovery timer batches only mark due work and leave the summary projection unchanged");
  expect(fixture.summary().service_if_due() == BranchSummaryOutcome::Pending && backend.snapshot_reads == 1 && backend.refreshes == 0,
         "one application service coalesces multiple timer batches into one progress snapshot");
  expect(!fixture.summary().service_if_due() && backend.snapshot_reads == 1, "consumed progress requires another timer deadline before polling again");

  backend.operation.phase = TuiBranchSummaryPhase::Succeeded;
  backend.operation.refresh_required = true;
  auto const progress_frame = fixture.frame();
  fixture.tui_timeout_handler().expire(first_deadline + std::chrono::seconds{2});
  expect(backend.snapshot_reads == 1 && backend.refreshes == 0 && fixture.summary().active() && fixture.selectors().kind() == ActiveSelectList::BranchSummary &&
             fixture.frame() == progress_frame,
         "a terminal backend publication cannot refresh the catalog or restore the selector inside timer dispatch");
  expect(fixture.summary().service_if_due() == BranchSummaryOutcome::Settled && backend.snapshot_reads == 2 && backend.refreshes == 1 &&
             !fixture.summary().active(),
         "application servicing performs exactly one terminal snapshot and required catalog refresh");
  auto const& restored = fixture.selectors().view();
  expect(fixture.selectors().kind() == ActiveSelectList::Session && restored && restored->items.size() == 2 && restored->query == "TIMER" &&
             restored->selected_item_index == 0 && restored->items[0].value == "second",
         "deferred settlement restores query and selected identity into reordered catalog rows");
  expect(!fixture.summary().service_if_due(), "terminal service produces its outcome only once");
  fixture.tui_timeout_handler().expire(first_deadline + std::chrono::seconds{3});
  expect(!fixture.summary().service_if_due() && backend.snapshot_reads == 2 && backend.refreshes == 1,
         "settled discovery is disarmed and cannot repeat snapshot or catalog work");
}

// Refused cancellation retains exit intent across later ContinueSession requests;
// terminal discovery can release it only in the application servicing stage.
void test_summary_timer_preserves_sticky_exit()
{
  SummaryTimerFixture fixture;
  auto& backend = fixture.backend();
  expect(fixture.start() == BranchSummaryOutcome::Pending, "exit case starts an active summary");
  expect(fixture.summary().cancel(BranchSummaryCancelMode::ExitAfterSettlement) == BranchSummaryOutcome::Pending,
         "refused cancellation leaves exit waiting for settlement");
  expect(fixture.summary().cancel(BranchSummaryCancelMode::ContinueSession) == BranchSummaryOutcome::Pending && backend.cancellations == 2 &&
             backend.canceled_generation == backend.operation.generation && backend.snapshot_reads == 0,
         "later cancellation preserves the active generation without polling a refused request");
  auto const waiting_frame = fixture.frame();
  backend.operation.phase = TuiBranchSummaryPhase::Canceled;
  auto const deadline = Clock::now() + std::chrono::seconds{1};
  fixture.tui_timeout_handler().expire(deadline);
  expect(backend.snapshot_reads == 0 && backend.refreshes == 0 && fixture.summary().active() && fixture.selectors().kind() == ActiveSelectList::BranchSummary &&
             fixture.frame() == waiting_frame,
         "canceled backend discovery leaves the modal and exit gate untouched during timer dispatch");
  expect(fixture.summary().service_if_due() == BranchSummaryOutcome::ExitReady && backend.snapshot_reads == 1 && backend.refreshes == 0 &&
             !fixture.summary().active() && !fixture.selectors().view(),
         "application servicing honors sticky exit and closes the selector exactly once");
  expect(!fixture.summary().service_if_due(), "exit outcome is consumed by its one application service");
  fixture.tui_timeout_handler().expire(deadline + std::chrono::seconds{1});
  expect(!fixture.summary().service_if_due() && backend.snapshot_reads == 1, "exit settlement retires further discovery");
}

// An accepted input cancellation may settle while a timer flag is queued; neither
// that flag nor its disarmed timer may poll a subsequently started generation.
void test_summary_timer_clears_due_across_settlement_and_restart()
{
  SummaryTimerFixture fixture;
  auto& backend = fixture.backend();
  expect(fixture.start() == BranchSummaryOutcome::Pending, "restart case starts its first generation");
  fixture.tui_timeout_handler().expire(Clock::now() + std::chrono::seconds{1});
  expect(backend.snapshot_reads == 0, "queued timer work remains unserviced before synchronous cancellation");
  backend.operation.phase = TuiBranchSummaryPhase::Canceled;
  backend.accept_cancellation = true;
  expect(fixture.summary().cancel(BranchSummaryCancelMode::ContinueSession) == BranchSummaryOutcome::Settled && backend.snapshot_reads == 1 &&
             !fixture.summary().active(),
         "accepted cancellation can settle through its immediate snapshot before timer servicing");
  expect(!fixture.summary().service_if_due() && backend.snapshot_reads == 1, "synchronous settlement clears any previously queued discovery");

  ++backend.operation.generation;
  backend.operation.phase = TuiBranchSummaryPhase::Generating;
  expect(fixture.start() == BranchSummaryOutcome::Pending && fixture.summary().active(), "the same owner admits a fresh summary generation");
  expect(!fixture.summary().service_if_due() && backend.snapshot_reads == 1, "restart does not inherit a due flag from the settled generation");
  auto const restarted_frame = fixture.frame();
  backend.operation.phase = TuiBranchSummaryPhase::Succeeded;
  backend.operation.refresh_required = true;
  fixture.tui_timeout_handler().expire(Clock::now() + std::chrono::seconds{1});
  expect(backend.snapshot_reads == 1 && backend.refreshes == 0 && fixture.frame() == restarted_frame,
         "the restarted timer also defers snapshot and refresh work until application servicing");
  expect(fixture.summary().service_if_due() == BranchSummaryOutcome::Settled && backend.snapshot_reads == 2 && backend.refreshes == 1,
         "a new due flag services the restarted generation once");
  expect(!fixture.summary().service_if_due() && backend.snapshot_reads == 2 && backend.refreshes == 1, "restarted settlement leaves no stale due work");
}

} // namespace

// The shared runner owns terminal initialization; each case retires every timer
// and offline frontend locally, with explicit deadlines and no worker or wait loop.
void run_tui_branch_summary_timer_tests()
{
  static_cast<void>(ava::tui::terminal::Context::resizeterm(24, 100));
  test_summary_timer_defers_snapshot_and_refresh();
  test_summary_timer_preserves_sticky_exit();
  test_summary_timer_clears_due_across_settlement_and_restart();
}
