// Exercises shutdown failures and worker-release ordering through real TUI sessions.
// Each isolated case uses a fresh initialized terminal and scoped call-through wrappers.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/ActiveRunInbox.h"
#include "ava/tui/ActiveRunOutcome.h"
#include "ava/tui/TuiSession.h"
#include "ava/core/Application.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

// Private exception text must never appear in fixed shutdown diagnostics.
constexpr char kPrivateFailure[] = "R9_PRIVATE_PROMPT_AND_CREDENTIAL_SENTINEL";
// Completion throws this first; a later release error must not expose it either.
constexpr char kPrivateOuterFailure[] = "R9_PRIVATE_OUTER_COMPLETION_SENTINEL";

// Chosen on the TUI thread before launch; the worker only reads this immutable choice.
enum class Fault
{
  QueueReceipts, // Fail after join, so Session must retain a nonzero cleanup outcome.
  QueueReturnedError, // Return a queue error during shutdown, without throwing.
  CompletionReturnedError, // Return the same error during ordinary completion.
  StopRequest, // Fail before cancellation, leaving the worker's stop flag unchanged.
  QueuedPrompts, // Fail while releasing requests that the TUI has not displayed.
  ActivePrompt, // Fail while retiring callbacks for the displayed host prompt.
  PluginWaiters, // Fail at the last release step before a worker join is permitted.
  OuterException, // Fail a release from complete()'s catch of an earlier private error.
};

// Emit complete fixed markers before a possible abort; no test content is logged.
void marker(char const* value)
{
  std::fputs(value, stderr);
  std::fflush(stderr);
}

// A scoped probe borrows only owners declared before Session. The worker touches
// promises/futures and the atomic flag; all remaining fields belong to the TUI.
struct ShutdownProbe
{
  // Scenario chosen before Session or its worker exists.
  Fault fault = Fault::QueueReceipts;
  // Borrowed loop captured during real attachment; invalid after Session dies.
  ava::tui::TuiEventLoop* loop = nullptr;
  // Single worker publication proving the worker reached its held wait.
  std::promise<void> entered;
  // TUI-only reader waits before attempting any release-stage failure.
  std::future<void> entered_future = entered.get_future();
  // TUI release used only on the recoverable path or scope unwind.
  std::promise<void> release;
  // Worker waits here, with a finite fallback if the external supervisor fails.
  std::future<void> release_future = release.get_future();
  // Worker stores before returning; queue finish must observe true after join.
  std::atomic_bool worker_returned{false};
  // TUI-only idempotence for release from successful cleanup and unwind.
  bool released = false;
  // Armed only by the queue hook after ordinary Session construction.
  bool armed = false;
  // True only once shutdown reaches the first pre-join release operation.
  bool cleaning = false;
  // Each pre-join stage must be attempted, even if an earlier one throws.
  std::array<unsigned, 4> attempts{};
  // Queue finish must run exactly once after the worker has returned.
  unsigned queue_finishes = 0;
  // Real inbox retirement must still happen after queue finish throws or returns an error.
  unsigned inbox_retirements = 0;

  // Release before Session destruction on any ordinary exception path.
  void release_worker()
  {
    if (!released)
    {
      released = true;
      release.set_value();
    }
  }

  // Test synchronization and borrowed pointers never use generated traversal.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Only the TUI thread installs this pointer; every unscoped call forwards.
thread_local ShutdownProbe* shutdown_probe = nullptr;

// Restore call-through behavior before destroying the borrowed observations.
class ProbeScope final
{
 public:
  // Probe is declared before this scope and outlives Session and all its workers.
  explicit ProbeScope(ShutdownProbe& probe) : previous_(std::exchange(shutdown_probe, &probe)) { }
  // Clear the pointer after Session has completed destruction.
  ~ProbeScope() { shutdown_probe = previous_; }
  // A registered borrow has exactly one restoration owner.
  ProbeScope(ProbeScope const&) = delete;
  ProbeScope& operator=(ProbeScope const&) = delete;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
 private:
  // Prior nested scope, if any; its owner outlives this scope.
  ShutdownProbe* previous_;
};

// Keep the real terminal FILE while replacing its non-epollable test tmpfile.
class InputPipe final
{
 public:
  // Save the input descriptor before installing an empty, live pipe.
  InputPipe() : stream_(ava::core::Application::instance().terminal_context().input_stream()), saved_(::dup(::fileno(stream_)))
  {
    if (!saved_)
      throw std::system_error(errno, std::generic_category(), "save shutdown test input");
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) != 0)
      throw std::system_error(errno, std::generic_category(), "shutdown test pipe");
    ava::core::UniqueFd reader(descriptors[0]);
    peer_.reset(descriptors[1]);
    if (::dup2(reader.get(), ::fileno(stream_)) < 0)
      throw std::system_error(errno, std::generic_category(), "replace shutdown test input");
    std::clearerr(stream_);
    ava::tui::runtime_input::reset_input_decoder();
  }
  // Session and workers are already gone; restore Application's original fd.
  ~InputPipe()
  {
    ava::tui::runtime_input::clear_startup_input_queue();
    ava::tui::runtime_input::reset_input_decoder();
    expect(::dup2(saved_.get(), ::fileno(stream_)) >= 0, "restore shutdown test input");
    std::clearerr(stream_);
  }
  // One guard alone owns descriptor restoration.
  InputPipe(InputPipe const&) = delete;
  InputPipe& operator=(InputPipe const&) = delete;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
 private:
  // Process-owned stream is borrowed until descriptor restoration.
  FILE* stream_;
  // Original input endpoint, restored after Session destruction.
  ava::core::UniqueFd saved_;
  // Write end kept open so empty input does not report end-of-file.
  ava::core::UniqueFd peer_;
};

// Declared after Session, so exception unwinding releases before its destructor joins.
class ReleaseWorker final
{
 public:
  // Probe is declared before Session and outlives the complete joined worker.
  explicit ReleaseWorker(ShutdownProbe& probe) : probe_(&probe) { }
  // Release on normal and exceptional returns; an expected abort bypasses this.
  ~ReleaseWorker() { probe_->release_worker(); }
  // One scope owns this final release action.
  ReleaseWorker(ReleaseWorker const&) = delete;
  ReleaseWorker& operator=(ReleaseWorker const&) = delete;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
 private:
  // Borrowed stack probe whose worker channels survive Session destruction.
  ShutdownProbe* probe_;
};

// Explicit supervisor-only selector; ordinary suite execution uses queue receipts.
Fault selected_fault()
{
  auto const* value = std::getenv("AVA_TUI_SHUTDOWN_FAULT");
  if (!value || std::string_view(value) == "queue_receipts")
    return Fault::QueueReceipts;
  if (std::string_view(value) == "queue_returned_error")
    return Fault::QueueReturnedError;
  if (std::string_view(value) == "completion_returned_error")
    return Fault::CompletionReturnedError;
  if (std::string_view(value) == "stop_request")
    return Fault::StopRequest;
  if (std::string_view(value) == "queued_prompts")
    return Fault::QueuedPrompts;
  if (std::string_view(value) == "active_prompt")
    return Fault::ActivePrompt;
  if (std::string_view(value) == "plugin_waiters")
    return Fault::PluginWaiters;
  if (std::string_view(value) == "outer_exception")
    return Fault::OuterException;
  throw std::runtime_error("unknown shutdown test fault");
}

// These cases reach complete() before shutdown; other workers stay held until release.
bool uses_completion_path(Fault fault)
{
  return fault == Fault::OuterException || fault == Fault::CompletionReturnedError;
}

// Post-join failures return to Session; pre-join release failures must terminate.
bool returns_after_cleanup(Fault fault)
{
  return fault == Fault::QueueReceipts || fault == Fault::QueueReturnedError || fault == Fault::CompletionReturnedError;
}

// Pure-value regression: composition cannot discard cleanup, signal, or I/O state.
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

// Exercise real Session shutdown with a held worker, or completion that throws
// before cleanup also fails. Returned queue errors must also preserve exit status 1;
// the enclosing-exception case protects private content during fatal cleanup.
void test_cleanup()
{
  InputPipe input;
  ShutdownProbe probe;
  probe.fault = selected_fault();
  if (!returns_after_cleanup(probe.fault))
  {
    // Intentional fatal termination must not leave a large core artifact.
    rlimit const no_core{0, 0};
    if (::setrlimit(RLIMIT_CORE, &no_core) != 0)
      throw std::system_error(errno, std::generic_category(), "disable shutdown test core dump");
  }
  tui_test_support::TuiFrontEndFixture frontend;
  // These callbacks store pointer values, not reference captures. Probe is declared
  // before frontend/Session and survives their callbacks and the complete worker join.
  frontend.hooks().create_active_run_queues = [state = &probe](ava::event::EventEnvelopeSink) {
    if (!state->loop)
      throw std::runtime_error("shutdown test did not observe loop attachment");
    ava::tui::TuiActiveRunQueues queues;
    queues.finish = [state](bool stopped) -> ava::core::VoidResult {
      ++state->queue_finishes;
      expect((stopped || uses_completion_path(state->fault)) && state->worker_returned.load(),
             "queue receipts execute after stop or ordinary completion, after worker join");
      marker("R9_QUEUE_FINISH\n");
      if (state->fault == Fault::QueueReturnedError || state->fault == Fault::CompletionReturnedError)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, kPrivateFailure));
      throw std::runtime_error(state->fault == Fault::OuterException ? kPrivateOuterFailure : kPrivateFailure);
    };
    state->armed = true;
    // Let completion cases reach real complete(); the others enter shutdown
    // while the worker is held, independently of provider completion timing.
    if (!uses_completion_path(state->fault))
      state->loop->request_stop();
    return queues;
  };
  frontend.hooks().on_submit = [state = &probe](std::string const&, ava::tui::TuiSubmitContext) {
    marker("R9_WORKER_WAITING\n");
    state->entered.set_value();
    // This fallback also bounds a manually launched child. The supervisor normally
    // kills/reaps a join regression first; either path is a test failure.
    if (!uses_completion_path(state->fault) && state->release_future.wait_until(ava::tests::now_plus_seconds(30)) != std::future_status::ready)
      marker("R9_WORKER_FALLBACK\n");
    state->worker_returned.store(true);
    marker("R9_WORKER_RETURNED\n");
    return ava::tui::TuiSubmitResult{};
  };
  ava::tui::runtime_input::RuntimeInput text;
  text.event.key = ava::tui::terminal::Key::Character;
  text.event.character = 'r';
  text.event.text = "run";
  text.text = "run";
  expect(ava::tui::runtime_input::enqueue_startup_input(std::move(text)), "seed shutdown submission");
  ava::tui::runtime_input::RuntimeInput enter;
  enter.event.key = ava::tui::terminal::Key::Enter;
  expect(ava::tui::runtime_input::enqueue_startup_input(std::move(enter)), "seed shutdown Enter key");
  ProbeScope scope(probe);
  {
    ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
    ReleaseWorker release(probe);
    int const status = session.run([] { return true; });
    expect(returns_after_cleanup(probe.fault), "pre-join release failure must terminate instead of returning from shutdown");
    expect(status == 1, "post-join queue finish exception or returned error gives session exit status 1");
    marker(status == 1 ? "R9_SESSION_EXIT_1\n" : "R9_SESSION_WRONG_EXIT\n");
  }
  if (probe.fault == Fault::CompletionReturnedError)
    expect(probe.attempts == std::array<unsigned, 4>{}, "ordinary completion needs no exceptional shutdown release pass");
  else
    expect(probe.attempts == std::array<unsigned, 4>{1, 1, 1, 1}, "shutdown attempts each release stage once");
  expect(probe.queue_finishes == 1 && probe.inbox_retirements == 1 && probe.worker_returned.load(),
         "post-join failure still retires inbox and cannot repeat queue completion during destruction");
}

} // namespace

// Original external method symbols; GNU ld resolves these past the test wrappers.
extern "C" void shutdown_real_attach(ava::tui::RuntimePluginUiCoordinator*,
                                     ava::tui::TuiEventLoop&) asm("__real__ZN3ava3tui26RuntimePluginUiCoordinator11attach_loopERNS0_12TuiEventLoopE");
extern "C" bool shutdown_real_stop(ava::tui::ActiveRunInbox*) asm("__real__ZN3ava3tui14ActiveRunInbox12request_stopEv");
extern "C" void shutdown_real_queued(ava::tui::RuntimePromptCoordinator*) asm("__real__ZN3ava3tui24RuntimePromptCoordinator21fail_pending_requestsEv");
extern "C" ava::tui::PromptServiceOutcome shutdown_real_active(ava::tui::RuntimePromptCoordinator*, ava::tui::PromptCancelReason) asm(
    "__real__ZN3ava3tui24RuntimePromptCoordinator13cancel_activeENS0_18PromptCancelReasonE");
extern "C" void shutdown_real_plugin(ava::tui::RuntimePluginUiCoordinator*) asm("__real__ZN3ava3tui26RuntimePluginUiCoordinator17finish_submissionEv");
extern "C" void shutdown_real_retire(ava::tui::ActiveRunInbox*) asm("__real__ZN3ava3tui14ActiveRunInbox6retireEv");

// Wrapped names keep the usual C++ method ABI, including the explicit this pointer.
extern "C" void shutdown_wrap_attach(ava::tui::RuntimePluginUiCoordinator*,
                                     ava::tui::TuiEventLoop&) asm("__wrap__ZN3ava3tui26RuntimePluginUiCoordinator11attach_loopERNS0_12TuiEventLoopE");
extern "C" bool shutdown_wrap_stop(ava::tui::ActiveRunInbox*) asm("__wrap__ZN3ava3tui14ActiveRunInbox12request_stopEv");
extern "C" void shutdown_wrap_queued(ava::tui::RuntimePromptCoordinator*) asm("__wrap__ZN3ava3tui24RuntimePromptCoordinator21fail_pending_requestsEv");
extern "C" ava::tui::PromptServiceOutcome shutdown_wrap_active(ava::tui::RuntimePromptCoordinator*, ava::tui::PromptCancelReason) asm(
    "__wrap__ZN3ava3tui24RuntimePromptCoordinator13cancel_activeENS0_18PromptCancelReasonE");
extern "C" void shutdown_wrap_plugin(ava::tui::RuntimePluginUiCoordinator*) asm("__wrap__ZN3ava3tui26RuntimePluginUiCoordinator17finish_submissionEv");
extern "C" void shutdown_wrap_retire(ava::tui::ActiveRunInbox*) asm("__wrap__ZN3ava3tui14ActiveRunInbox6retireEv");

// Observe real construction, then borrow the public loop for this scoped test.
extern "C" void shutdown_wrap_attach(ava::tui::RuntimePluginUiCoordinator* coordinator, ava::tui::TuiEventLoop& loop)
{
  shutdown_real_attach(coordinator, loop);
  if (shutdown_probe)
    shutdown_probe->loop = &loop;
}

// Establish either a held worker or complete()'s enclosing catch before faulting.
extern "C" bool shutdown_wrap_stop(ava::tui::ActiveRunInbox* inbox)
{
  auto* probe = shutdown_probe;
  if (probe && probe->armed)
  {
    probe->cleaning = true;
    ++probe->attempts[0];
    if (probe->fault == Fault::OuterException)
    {
      // This is an actual complete() catch, after the first queue callback threw.
      // Observing current_exception does not replace or clear that outer error.
      bool const inside_completion_catch = std::current_exception() && probe->queue_finishes == 1 && probe->worker_returned.load();
      marker(inside_completion_catch ? "R9_OUTER_EXCEPTION_ACTIVE\n" : "R9_OUTER_EXCEPTION_MISSING\n");
    }
    else if (probe->entered_future.wait_until(ava::tests::now_plus_seconds(5)) == std::future_status::ready)
      marker("R9_WORKER_HELD\n");
    else
      marker("R9_WORKER_MISSING\n");
    marker("R9_STAGE_STOP\n");
    if (probe->fault == Fault::StopRequest)
      throw std::runtime_error(kPrivateFailure);
  }
  return shutdown_real_stop(inbox);
}

// Fail only this scoped release operation; unrelated prompt cleanup still forwards.
extern "C" void shutdown_wrap_queued(ava::tui::RuntimePromptCoordinator* coordinator)
{
  auto* probe = shutdown_probe;
  if (probe && probe->cleaning)
  {
    ++probe->attempts[1];
    marker("R9_STAGE_QUEUED\n");
    if (probe->fault == Fault::QueuedPrompts || probe->fault == Fault::OuterException)
      throw std::runtime_error(kPrivateFailure);
  }
  shutdown_real_queued(coordinator);
}

// An earlier exception must not prevent the active prompt release attempt.
extern "C" ava::tui::PromptServiceOutcome shutdown_wrap_active(ava::tui::RuntimePromptCoordinator* coordinator, ava::tui::PromptCancelReason reason)
{
  auto* probe = shutdown_probe;
  if (probe && probe->cleaning)
  {
    ++probe->attempts[2];
    marker("R9_STAGE_ACTIVE\n");
    if (probe->fault == Fault::ActivePrompt)
      throw std::runtime_error(kPrivateFailure);
  }
  return shutdown_real_active(coordinator, reason);
}

// Release a held worker only on the recoverable path. The outer-error case
// already joined during complete(); other fatal scenarios must remain held.
extern "C" void shutdown_wrap_plugin(ava::tui::RuntimePluginUiCoordinator* coordinator)
{
  auto* probe = shutdown_probe;
  if (probe && probe->cleaning)
  {
    ++probe->attempts[3];
    marker("R9_STAGE_PLUGIN\n");
    if (probe->fault == Fault::PluginWaiters)
      throw std::runtime_error(kPrivateFailure);
  }
  shutdown_real_plugin(coordinator);
  if (probe && probe->cleaning && (probe->fault == Fault::QueueReceipts || probe->fault == Fault::QueueReturnedError))
    probe->release_worker();
}

// Real inbox retirement proves cleanup continued beyond a throwing or returned queue error.
extern "C" void shutdown_wrap_retire(ava::tui::ActiveRunInbox* inbox)
{
  shutdown_real_retire(inbox);
  if (shutdown_probe && (shutdown_probe->cleaning || (shutdown_probe->armed && shutdown_probe->fault == Fault::CompletionReturnedError)))
  {
    ++shutdown_probe->inbox_retirements;
    shutdown_probe->cleaning = false;
    shutdown_probe->armed = false;
    marker("R9_INBOX_RETIRED\n");
  }
}

// Called by the existing isolated tui_session dispatcher after terminal setup.
void run_tui_shutdown_test_case(std::string_view test_case)
{
  if (test_case == "shutdown_outcome")
    test_outcome();
  else if (test_case == "shutdown_cleanup")
    test_cleanup();
  else
    expect(false, "unknown shutdown test case");
}
