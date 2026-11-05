// Exercise value-only TUI shutdown outcome composition.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/tui/ActiveRunOutcome.h"

namespace {

void test_outcome()
{
  using ava::tui::ActiveRunOutcome;
  auto const continuing = ActiveRunOutcome::continue_session();
  auto const closed = ActiveRunOutcome::close_session();
  expect(!continuing.break_loop() && !continuing.cleanup_failed() && !closed.cleanup_failed(), "ordinary outcomes contain no cleanup failure");
  auto const cleanup = continuing.with_cleanup_failure();
  expect(cleanup.break_loop() && cleanup.cleanup_failed() && !cleanup.terminal_write_failed() && !cleanup.terminal_signal_received(),
         "cleanup failure alone requests exit without inventing I/O failure or signal");
  auto const first = cleanup.with_terminal_write_failure().with_terminal_signal();
  auto const last = closed.with_terminal_signal().with_terminal_write_failure().with_cleanup_failure();
  auto const twice = last.with_cleanup_failure();
  for (auto const& result : {first, last, twice})
    expect(result.break_loop() && result.cleanup_failed() && result.terminal_write_failed() && result.terminal_signal_received(),
           "cleanup composition preserves every independent failure regardless of modifier order");
  expect(!continuing.break_loop() && !continuing.cleanup_failed(), "cleanup modifier leaves its source value unchanged");
}

} // namespace

void run_tui_shutdown_tests()
{
  test_outcome();
}
