// Exercise the real TuiSession turn with native ncurses input and real worker
// mailboxes. GNU ld call-through wrappers observe external stage calls; only a
// scoped TUI-thread probe may arrange a boundary or inject an I/O failure.
// Each registered case runs in its own process with an initialized Application
// Context. No wrapper changes production decoding, waiting, draining or painting.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/ActiveRunInbox.h"
#include "ava/tui/TuiSession.h"
#include "ava/tui/terminal/BasicWindow.h"
#include "ava/core/Application.h"
#include "ava/core/UniqueFd.h"
#include "utils/Signals.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Run isolated shutdown fault cases after the common terminal setup below.
void run_tui_shutdown_test_case(std::string_view test_case);

// Direct access to the real latch lets the fault test inspect it without adding
// an artificial Session control marker to its phase observations.
extern "C" bool session_real_notification(ava::tui::TuiEventLoop const*) noexcept asm("__real__ZNK3ava3tui12TuiEventLoop19notification_failedEv");

namespace {

using ava::tui::TuiEventLoop;
using ava::tui::TuiWaitMode;
using ava::tui::runtime_input::InputPollKind;
using Clock = TuiEventLoop::Clock;

// The six composition stages are recorded at external calls, without exposing
// TuiSession's private owners or dispatching any feature from a wrapper.
enum class Stage
{
  Control, // Session checks the notification-failure latch during control.
  Due, // The real timer dispatcher is about to run.
  Application, // The active run is about to consume its real inbox.
  Input, // The first real poll in this turn is about to decode native input.
  Paint, // The real renderer is about to test and consume its deadline.
  Wait, // The real waiter receives Session's chosen mode.
};

// TUI-only observations for one turn; shutdown drains are excluded because they
// execute outside the consume wrapper. Values remain owned after Session dies.
struct Turn
{
  // Ordered external phase entries; repeated entries reveal extra timer/update passes.
  std::vector<Stage> stages{Stage::Control};
  // Every decoder result, including protocol replies and the empty sentinel.
  std::vector<InputPollKind> input;
  // Actual sizes returned by Inbox::drain while ActiveRun::consume is executing.
  std::vector<std::size_t> drains;
  // Real consume results establish that provider projection preceded input.
  std::vector<ava::tui::RuntimeEventDrainResult> application_results;
  // Session's requested wait policy, absent for closing and stopped turns.
  std::optional<TuiWaitMode> wait;
  // Calls flushing the Application output stream inside this turn's real paint.
  std::size_t flushes = 0;
  // Actual paint result, absent when control/input stopped the turn first.
  std::optional<bool> painted;

  // Internal-linkage observations have no generated printer and contain no owners.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Stack-owned instrumentation, accessed only on the registered TUI thread.
// Callback captures borrow test locals declared before Session and survive its
// worker join. Producer threads never see this thread_local probe.
struct SessionProbe
{
  // TUI-only copies identify which decoded character actually reached active
  // dispatch. Retained-key assertions must not infer dispatch from decoder counts.
  std::vector<char> active_dispatched_characters;
  // Copied into the real audit endpoint's call-through tee. This callback may run
  // on either thread; its synchronized captures must survive Session's worker join.
  std::function<void(ava::event::RuntimeEvent const&, bool)> after_audit;

  // Completed and current turn observations, retained until assertions finish.
  std::vector<Turn> turns;
  // Borrowed only while Session exists; wrappers capture its public loop address.
  TuiEventLoop* loop = nullptr;
  // Application-owned stream identity filters unrelated libc flushes.
  FILE* output = ava::core::Application::instance().terminal_context().output_stream();
  // Tracks nested drain calls so teardown drains never count as application turns.
  bool consuming = false;
  // Limits fflush observations to the actual deferred frame paint.
  bool painting = false;
  // Arms exactly one 8-byte write fault around an explicit wakeup.notify call.
  bool fail_notify_write = false;
  // Counts injected writes; assertions require exactly one real eventfd attempt.
  std::size_t failed_notify_writes = 0;
  // Arms one terminal flush failure only after the real prompt claim is active.
  bool fail_paint_flush = false;
  // Counts the injected flush so a timeout cannot masquerade as paint-failure coverage.
  std::size_t failed_paint_flushes = 0;
  // Real-time bound also covers regressions that spin through closing control.
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
  // Only the turn-order and frame-deadline cases postpone recurring timers,
  // so unrelated control/spinner/reload ticks cannot change their wait policy.
  bool defer_recurring_timers = false;
  // Record production origins before any fixture postponement; construction arms
  // the control and display timers before a worker or host prompt can add timers.
  std::optional<Clock::time_point> control_epoch;
  // The first 500 ms registration belongs to display reload; it shares the control origin.
  std::optional<Clock::time_point> display_epoch;
  // A test failure records whether the guard, rather than the scenario, stopped execution.
  bool timed_out = false;
  // A conservative cadence origin recorded after every real paint entry returns.
  Clock::time_point previous_paint = Clock::now();
  // Runs before real wait, after Session already selected its policy.
  std::function<void(TuiEventLoop&, TuiWaitMode)> before_wait;
  // Runs before real due dispatch, after this turn's control stage finished.
  std::function<void(TuiEventLoop&, Clock::time_point)> before_due;
  // Runs after real dispatch to inspect callback effects before application work.
  std::function<void(TuiEventLoop&)> after_due;
  // Runs after a real decoded result, before Session's retained-key signal check.
  std::function<void(ava::tui::runtime_input::InputPollResult const&)> after_poll;
  // Runs after the real coordinator reports its prompt claim outcome.
  std::function<void(ava::tui::PromptServiceOutcome)> after_prompt;
  // Runs before claiming a queued prompt, allowing typeahead to precede its paint.
  std::function<void(ava::tui::RuntimePromptCoordinator&)> before_prompt;
  // Runs before a real synchronous paint, including prompt ownership handoffs.
  std::function<void()> before_handoff;
  // Runs after actual handoff I/O, before the caller can release a prompt result.
  std::function<void()> after_handoff;

  // Return the current TUI turn; all instrumented stages follow its control marker.
  Turn& turn() { return turns.back(); }

  // Stop a regressed loop through its public API without hiding the timeout failure.
  void check_deadline()
  {
    if (Clock::now() >= deadline)
    {
      timed_out = true;
      if (loop)
        loop->request_stop();
    }
  }

  // This test callback aggregate has internal linkage and borrows scoped fixtures.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Only the TUI thread installs this nonowning pointer, for less than the probe's
// lifetime. Every wrapper immediately forwards when no case has installed it.
thread_local SessionProbe* session_probe = nullptr;

// Restores instrumentation even when a real Session operation throws. Declare
// this before Session so its destructor and worker join still have a live probe.
class ProbeScope final
{
 public:
  // Borrow the stack probe until this guard restores its predecessor.
  explicit ProbeScope(SessionProbe& probe) : previous_(std::exchange(session_probe, &probe)) { }
  // Restore before destroying the borrowed probe; workers never use this pointer.
  ~ProbeScope() { session_probe = previous_; }
  // Instrumentation scope ownership cannot be duplicated.
  ProbeScope(ProbeScope const&) = delete;
  // A scope cannot be rebound while wrappers are executing.
  ProbeScope& operator=(ProbeScope const&) = delete;

  // Internal-linkage RAII guard deliberately avoids generated traversal.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Borrowed predecessor, normally null; the surrounding scope outlives this guard.
  SessionProbe* previous_;
};

// Preserve a Context FILE descriptor while changing the underlying endpoint.
// FILE ownership always stays with Application's process-lifetime fixture.
class StreamRedirect final
{
 public:
  // Duplicate before replacement; a failed dup leaves the existing stream intact.
  explicit StreamRedirect(FILE* stream) : stream_(stream), saved_(::dup(::fileno(stream)))
  {
    if (!saved_)
      throw std::system_error(errno, std::generic_category(), "save Context stream");
  }
  // Restore while the Context is alive and clear errors left by /dev/full or EOF.
  ~StreamRedirect()
  {
    if (changed_)
      expect(::dup2(saved_.get(), ::fileno(stream_)) >= 0, "restore Context stream descriptor");
    std::clearerr(stream_);
  }
  // One guard owns the saved descriptor and its restoration obligation.
  StreamRedirect(StreamRedirect const&) = delete;
  // Restoration cannot transfer to a different borrowed FILE.
  StreamRedirect& operator=(StreamRedirect const&) = delete;
  // Replace the endpoint without changing the FILE or the fd registered with epoll.
  void replace(int descriptor)
  {
    if (::dup2(descriptor, ::fileno(stream_)) < 0)
      throw std::system_error(errno, std::generic_category(), "replace Context stream");
    changed_ = true;
    std::clearerr(stream_);
  }

  // Internal-linkage descriptor guard deliberately avoids generated traversal.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Process-owned stream borrowed until restoration, before terminal teardown.
  FILE* stream_;
  // Original open-file description retained independently of the replaced fd.
  ava::core::UniqueFd saved_;
  // Avoid replacing a stream that this guard only saved and never changed.
  bool changed_ = false;
};

// Own a pollable pipe at the Context input fd before constructing Session.
// Keep the watched descriptor open through Session destruction; closing only the
// opposite endpoint produces real EPOLLHUP or EPOLLERR instead of fd reuse.
class SessionInput final
{
 public:
  // Error mode watches the write end; ordinary mode watches the read end.
  explicit SessionInput(bool error_mode = false) : redirect_(ava::core::Application::instance().terminal_context().input_stream())
  {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) != 0)
      throw std::system_error(errno, std::generic_category(), "Session test pipe");
    ava::core::UniqueFd reader(descriptors[0]);
    ava::core::UniqueFd writer(descriptors[1]);
    redirect_.replace(error_mode ? writer.get() : reader.get());
    peer_ = error_mode ? std::move(reader) : std::move(writer);
    ava::tui::runtime_input::reset_input_decoder();
  }
  // Destroy only after Session joined all workers; discard retained keys before
  // restoring the original descriptor so another case cannot inherit input.
  ~SessionInput()
  {
    ava::tui::runtime_input::reset_input_decoder();
    for (std::size_t remaining = 0; remaining < 256; ++remaining)
      if (ava::tui::runtime_input::poll_curses_input().kind == InputPollKind::Empty)
        break;
  }
  // TUI test thread, ordinary mode only: enqueue real fd bytes after close is
  // requested. The nonblocking pipe makes fixture failure return instead of hang.
  [[nodiscard]] bool write_bytes(std::string_view bytes) const
  {
    return ::write(peer_.get(), bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
  }
  // TUI test thread: inspect only the kernel pipe, separately from ncurses replay.
  // Return no count if the descriptor cannot supply this fixture observation.
  [[nodiscard]] std::optional<std::size_t> pending_pipe_bytes() const
  {
    int count = 0;
    auto* stream = ava::core::Application::instance().terminal_context().input_stream();
    if (::ioctl(::fileno(stream), FIONREAD, &count) != 0 || count < 0)
      return std::nullopt;
    return static_cast<std::size_t>(count);
  }
  // Closing the peer latches terminal failure in the next actual epoll wait.
  void close_peer() { peer_.reset(); }

  // Internal-linkage pipe fixture deliberately avoids generated traversal.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Restored last, after the retained peer and every Session borrower are gone.
  StreamRedirect redirect_;
  // Sole remaining peer; the watched endpoint is owned by the existing FILE fd.
  ava::core::UniqueFd peer_;
};

// Install Application's real bit-setting handler after terminal initialization.
// Isolated cases start with AVA's reserved SIGTERM blocked and ignored; restore
// that supported baseline after Session has claimed the injected signal.
class SessionSignals final
{
 public:
  // Preserve the exact thread mask; no custom signal handler enters production.
  SessionSignals() : previous_mask_(utils::SignalSet::blocked_signals)
  {
    static_cast<void>(ava::core::Signals::try_obtain(SIGTERM));
    ava::core::Application::instance().signals_manager().activate_handlers({SIGTERM});
  }
  // The supported unregister route also clears utils' debug registration state.
  ~SessionSignals()
  {
    utils::Signal::block_and_unregister(SIGTERM);
    // Restore the reservation too: Application still owns its Signals manager.
    // default_handlers would replace an already registered callback, which the
    // utils debug contract forbids; unregister/reserve restores ignored state.
    utils::Signal::instance().reserve({SIGTERM}, 0);
    static_cast<void>(ava::core::Signals::try_obtain(SIGTERM));
    expect(previous_mask_.setmask(), "restore Session signal mask");
  }

  // Internal-linkage signal guard deliberately avoids generated traversal.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI-thread signal mask from before activation, restored after the worker join.
  utils::SignalSet previous_mask_;
};

// Keep the real provider worker unfinished while the TUI drains its finite event
// batch. The worker waits at most five real seconds even if a test exits early.
class WorkerGate final
{
 public:
  // Create both synchronization channels before the Session starts its worker.
  WorkerGate() : published_(publication_.get_future()), released_(release_.get_future().share()) { }
  // Worker-only publication after the scenario has reached its required setup point.
  void published() { publication_.set_value(); }
  // TUI-only bounded wait prevents a missing worker from hanging the case.
  bool wait_for_publication() { return published_.wait_for(std::chrono::seconds(5)) == std::future_status::ready; }
  // Worker-only bounded wait; the TUI normally releases it at the final wait.
  bool wait_for_release() { return released_.wait_for(std::chrono::seconds(5)) == std::future_status::ready; }
  // TUI-only idempotent release used by both normal completion and unwind cleanup.
  void release()
  {
    if (!opened_)
    {
      opened_ = true;
      release_.set_value();
    }
  }

  // Internal-linkage synchronization fixture deliberately avoids generated traversal.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Worker owns the single publication action; TUI waits on the future below.
  std::promise<void> publication_;
  // TUI-only reader of the worker's happens-before publication boundary.
  std::future<void> published_;
  // TUI owns the single release action, guarded by opened_.
  std::promise<void> release_;
  // Worker reads this future while the TUI keeps the promise alive through join.
  std::shared_future<void> released_;
  // TUI-only bookkeeping; the worker synchronizes through released_ instead.
  bool opened_ = false;
};

// Declare after Session so unwinding releases the held worker before Session's
// destructor joins it. The gate itself is declared before Session and outlives it.
class ReleaseWorker final
{
 public:
  // Borrow the gate until the enclosing Session scope exits.
  explicit ReleaseWorker(WorkerGate& gate) : gate_(gate) { }
  // Release without relying on any assertion or final wait being reached.
  ~ReleaseWorker() { gate_.release(); }

  // Internal-linkage cleanup guard deliberately avoids generated traversal.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Borrowed synchronization owner whose lifetime extends beyond Session's join.
  WorkerGate& gate_;
};

// Push native bytes in reverse because ncurses unget_wch implements a stack.
// This bypasses the startup queue's capacity without bypassing the real decoder.
void native_input(std::string_view bytes)
{
  for (auto it = bytes.rbegin(); it != bytes.rend(); ++it)
    ava::tui::terminal::BasicWindow::unget_wch(static_cast<wchar_t>(static_cast<unsigned char>(*it)));
}

// Seed a two-item startup submission; Session still performs ordinary dispatch
// and launches its normal ActiveRun worker from the retained Enter key.
void seed_submission()
{
  ava::tui::runtime_input::RuntimeInput text;
  text.event.key = ava::tui::terminal::Key::Character;
  text.event.character = 'r';
  text.event.text = "run";
  text.text = "run";
  expect(ava::tui::runtime_input::enqueue_startup_input(std::move(text)), "seed Session draft");
  ava::tui::runtime_input::RuntimeInput enter;
  enter.event.key = ava::tui::terminal::Key::Enter;
  expect(ava::tui::runtime_input::enqueue_startup_input(std::move(enter)), "seed Session submit key");
}

// Guarantee frame eligibility using real time, with no deadline upper-bound
// assertion and no global clock interposition affecting workers or OS waits.
void reach_frame_deadline(SessionProbe const& probe)
{
  std::this_thread::sleep_until(probe.previous_paint + ava::tui::FrameScheduler::kFrameInterval);
}

// Keep a test-arranged idle wait bounded while still executing the real waiter.
void notify_before_block(TuiEventLoop& loop, TuiWaitMode mode)
{
  if (mode == TuiWaitMode::Block)
    loop.wakeup().notify();
}

// Run a complete public Session while fixture and probe borrows remain alive.
// The Session destructor runs before the probe scope restores wrapper forwarding.
int run_session(tui_test_support::TuiFrontEndFixture& frontend, SessionProbe& probe)
{
  ProbeScope scope(probe);
  ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
  return session.run([] { return true; });
}

// Assert each complete active turn's six phases exactly once, rejecting the old
// extra due/application pass between paint and wait as well as reordered input.
void expect_active_turn(Turn const& turn)
{
  expect(turn.stages == std::vector<Stage>{Stage::Control, Stage::Due, Stage::Application, Stage::Input, Stage::Paint, Stage::Wait},
         "Session active turn is control, due, application, input, paint, wait exactly once");
}

// T2/T4/B2/B3: the held worker cannot publish completion while 300 real events
// drain 256/44 and one discarded protocol reply plus 64 keys drain 32/32/1.
void test_turn_order()
{
  SessionInput input;
  WorkerGate gate;
  std::atomic_bool worker_finished{false};
  std::atomic_bool published_ok{true};
  std::atomic_bool release_observed{false};
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.hooks().on_submit = [&](std::string const&, ava::tui::TuiSubmitContext context) {
    for (std::size_t index = 0; index < 300; ++index)
    {
      ava::event::MessagePayload payload;
      payload.text = "x";
      if (!context.event_sink(ava::event::RuntimeEvent{{}, ava::event::MessageUpdateEvent{.payload = std::move(payload)}}))
        published_ok.store(false);
    }
    gate.published();
    release_observed.store(gate.wait_for_release());
    worker_finished.store(true);
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  probe.defer_recurring_timers = true;
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    if (probe.turns.size() == 1)
    {
      expect(gate.wait_for_publication(), "worker publishes before buffered-input turn");
      native_input(std::string("\x1b[?5u") + std::string(64, 'a'));
      reach_frame_deadline(probe);
    }
    expect(!worker_finished.load(), "worker remains unfinished through bounded live drains");
    if (probe.turns.size() == 4)
    {
      gate.release();
      loop.request_stop();
    }
    notify_before_block(loop, mode);
  };
  seed_submission();
  int exit_code = -1;
  {
    ProbeScope scope(probe);
    ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
    ReleaseWorker release(gate);
    exit_code = session.run([] { return true; });
  }
  expect(exit_code == 0 && !probe.timed_out && published_ok.load() && release_observed.load() && worker_finished.load(),
         "Session exits normally and joins its released provider worker");
  if (!ava::tests::require(probe.turns.size() == 4, "Session uses three buffered-input turns after submission"))
    return;
  for (std::size_t index = 1; index < 4; ++index)
    expect_active_turn(probe.turns[index]);
  auto const& first = probe.turns[1];
  auto const& second = probe.turns[2];
  auto const& final = probe.turns[3];
  expect(first.drains == std::vector<std::size_t>{256} && second.drains == std::vector<std::size_t>{44} && final.drains == std::vector<std::size_t>{0},
         "real live inbox obeys its 256-event budget before provider completion");
  expect(first.application_results == std::vector{ava::tui::RuntimeEventDrainResult::UpdatedNoRender} &&
             second.application_results == std::vector{ava::tui::RuntimeEventDrainResult::UpdatedNoRender},
         "provider updates are projected before input at an eligible frame deadline");
  expect(first.input.size() == 32 && first.input.front() == InputPollKind::Discarded && std::ranges::count(first.input, InputPollKind::Input) == 31 &&
             second.input == std::vector<InputPollKind>(32, InputPollKind::Input) && final.input == std::vector{InputPollKind::Input, InputPollKind::Empty},
         "discarded Kitty reply consumes budget without hiding the following 64 native keys");
  expect(first.wait == TuiWaitMode::ReadyOnly && second.wait == TuiWaitMode::ReadyOnly && final.wait == TuiWaitMode::Block,
         "full input/event batches stay runnable; exhausted queues block without an extra paint turn");
  expect(first.flushes > 0, "eligible provider/input frame actually flushes at the same paint stage");
}

// R1: real fd bytes arriving after empty-draft Ctrl+D must be decoded and
// discarded while the worker is held, so level-triggered readiness cannot spin.
void test_close_after_submit_drains_input()
{
  SessionInput input;
  WorkerGate gate;
  std::atomic_size_t submissions{0};
  std::atomic_bool worker_stopped{false};
  std::atomic_bool released{false};
  tui_test_support::TuiFrontEndFixture frontend;
  // These copied pointers borrow locals that outlive Session. ReleaseWorker
  // opens the gate before Session's destructor joins, including on exceptions.
  frontend.hooks().on_submit = [gate = &gate, submissions = &submissions, stopped = &worker_stopped, released = &released](std::string const&,
                                                                                                                           ava::tui::TuiSubmitContext context) {
    if (submissions->fetch_add(1) != 0)
      return ava::tui::TuiSubmitResult{};
    gate->published();
    released->store(gate->wait_for_release());
    stopped->store(context.cancel_requested && context.cancel_requested());
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  std::optional<std::size_t> first_drain_turn;
  std::size_t decoded = 0;
  std::size_t drain_turns = 0;
  bool drained = false;
  bool turn_limit_reached = false;
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    if (probe.turns.size() == 1)
    {
      expect(gate.wait_for_publication(), "close test worker is running before Ctrl+D");
      native_input("\x04");
      notify_before_block(loop, mode);
      return;
    }
    if (!first_drain_turn)
    {
      // Ctrl+D was dispatched in this turn. Send actual pipe data only now,
      // rather than using startup replay which does not make epoll readable.
      first_drain_turn = probe.turns.size();
      expect(input.write_bytes(std::string(65, 'x') + "\n"), "write fd bytes after close request");
      return;
    }
    if (drained || turn_limit_reached)
      return;
    drain_turns = probe.turns.size() - *first_drain_turn;
    decoded += static_cast<std::size_t>(std::ranges::count(probe.turn().input, InputPollKind::Input));
    drained = decoded == 66 && !probe.turn().input.empty() && probe.turn().input.back() == InputPollKind::Empty;
    if (drained)
    {
      expect(input.pending_pipe_bytes() == std::optional<std::size_t>{0}, "closing drain empties the watched kernel pipe");
      gate.release();
      return;
    }
    if (drain_turns >= 3)
    {
      // This detects the old spin by work count, independently of timer timing.
      // Always release before requesting shutdown, which joins the held worker.
      turn_limit_reached = true;
      gate.release();
      loop.request_stop();
    }
  };
  seed_submission();
  int exit_code = -1;
  {
    ProbeScope scope(probe);
    ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
    ReleaseWorker release(gate);
    exit_code = session.run([] { return true; });
  }
  expect(exit_code == 0 && !probe.timed_out && released.load() && worker_stopped.load(), "close drain releases and joins a canceled worker normally");
  expect(drained && drain_turns <= 3 && !turn_limit_reached, "66 bytes after close drain in at most three input turns without a readable-fd spin");
  expect(submissions.load() == 1, "discarded close-state Enter never starts another submission");
}

// B3: an ordinary key returned after the prior frame's cadence deadline must
// flush in this turn without a second timer/application pass after input.
void test_frame_deadline()
{
  SessionInput input;
  tui_test_support::TuiFrontEndFixture frontend;
  SessionProbe probe;
  probe.defer_recurring_timers = true;
  bool delayed = false;
  probe.after_poll = [&](ava::tui::runtime_input::InputPollResult const& result) {
    if (probe.turns.size() == 2 && result.kind == InputPollKind::Input)
    {
      reach_frame_deadline(probe);
      delayed = true;
    }
  };
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    if (probe.turns.size() == 1)
      native_input("q");
    else
      loop.request_stop();
    notify_before_block(loop, mode);
  };
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 0 && !probe.timed_out && delayed, "real input decode crosses the frame cadence boundary");
  if (!ava::tests::require(probe.turns.size() == 2, "deadline case completes in its input turn"))
    return;
  auto const& turn = probe.turns[1];
  expect(turn.stages == std::vector<Stage>{Stage::Control, Stage::Due, Stage::Input, Stage::Paint, Stage::Wait} && turn.flushes > 0 && turn.painted == true &&
             turn.wait == TuiWaitMode::Block,
         "due input frame flushes once at turn end and leaves no stale runnable timer work");
}

// T4: keep the watched fd valid, then close its peer after buffering more than
// one input budget. HUP is a normal close; a watched write end reports EPOLLERR.
void test_closing_drain(bool descriptor_error)
{
  SessionInput input(descriptor_error);
  tui_test_support::TuiFrontEndFixture frontend;
  std::atomic_size_t submissions{0};
  frontend.hooks().on_submit = [&](std::string const&, ava::tui::TuiSubmitContext) {
    ++submissions;
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  bool observed_descriptor_state = false;
  probe.before_wait = [&](TuiEventLoop&, TuiWaitMode) {
    native_input(std::string(64, 'x') + "\n");
    input.close_peer();
  };
  probe.after_poll = [&](ava::tui::runtime_input::InputPollResult const&) {
    if (probe.turns.size() > 1)
      observed_descriptor_state = descriptor_error ? probe.loop->terminal_error() : probe.loop->terminal_hangup();
  };
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == (descriptor_error ? 1 : 0) && !probe.timed_out && observed_descriptor_state && submissions.load() == 0,
         "real terminal close/error drains buffered Enter without submitting");
  if (!ava::tests::require(probe.turns.size() == 4, "closing drain consumes 65 native keys across three bounded turns"))
    return;
  for (std::size_t index = 1; index < 4; ++index)
    expect(probe.turns[index].stages == std::vector<Stage>{Stage::Control, Stage::Input},
           "closing turns contain no timer, application, paint or repeated wait work");
  expect(probe.turns[1].input == std::vector<InputPollKind>(32, InputPollKind::Input) &&
             probe.turns[2].input == std::vector<InputPollKind>(32, InputPollKind::Input) &&
             probe.turns[3].input == std::vector{InputPollKind::Input, InputPollKind::Empty},
         "closing drain preserves the 32-result input budget through the final empty sentinel");
}

// B6: the actual idle 100 ms timer supplies the wake. Inject SIGTERM after the
// next control stage, immediately before real dispatch, to expose any callback
// that improperly claims signals or performs cancellation inside service_due.
void test_control_timer()
{
  SessionInput input;
  SessionSignals signals;
  tui_test_support::TuiFrontEndFixture frontend;
  SessionProbe probe;
  bool injected = false;
  bool pending_after_dispatch = false;
  bool running_after_dispatch = false;
  probe.before_due = [&](TuiEventLoop&, Clock::time_point) {
    if (probe.turns.size() == 2)
    {
      expect(::raise(SIGTERM) == 0, "inject SIGTERM before real control-timer dispatch");
      injected = true;
    }
  };
  probe.after_due = [&](TuiEventLoop& loop) {
    if (probe.turns.size() == 2)
    {
      pending_after_dispatch = ava::core::Signals::received(ava::core::Signals::to_mask(SIGTERM));
      running_after_dispatch = !loop.stopped();
    }
  };
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    // The first Block is deliberately untouched: the real timer must wake it.
    if (probe.turns.size() > 1)
      notify_before_block(loop, mode);
  };
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 130 && !probe.timed_out && injected && pending_after_dispatch && running_after_dispatch,
         "control timer only wakes; SIGTERM remains pending until the next control checkpoint");
  expect(probe.control_epoch && probe.display_epoch && probe.control_epoch == probe.display_epoch,
         "Session arms control and display reload from the same clock sample");
  expect(probe.turns.size() == 3 && probe.turns.front().wait == TuiWaitMode::Block && probe.turns.back().stages == std::vector<Stage>{Stage::Control},
         "idle timer wake reaches a later control-only signal exit");
}

// T4: SIGTERM arriving immediately after a real character decode consumes that
// retained key at Session's second signal checkpoint; the queued Enter cannot
// submit a draft before the signal outcome returns 130.
void test_signal_after_decode()
{
  SessionInput input;
  SessionSignals signals;
  tui_test_support::TuiFrontEndFixture frontend;
  std::atomic_size_t submissions{0};
  frontend.hooks().on_submit = [&](std::string const&, ava::tui::TuiSubmitContext) {
    ++submissions;
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  bool injected = false;
  probe.after_poll = [&](ava::tui::runtime_input::InputPollResult const& result) {
    if (!injected && result.kind == InputPollKind::Input)
    {
      injected = true;
      expect(::raise(SIGTERM) == 0, "inject SIGTERM after native character decode");
    }
  };
  native_input("x\n");
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 130 && injected && !probe.timed_out && submissions.load() == 0, "post-decode SIGTERM prevents retained-key dispatch and submission");
  expect(probe.turns.size() == 1 && probe.turns.front().input == std::vector{InputPollKind::Input} &&
             probe.turns.front().stages == std::vector<Stage>{Stage::Control, Stage::Due, Stage::Input},
         "signal stops the input batch after its retained character without paint or wait");
}

// T4: inject EIO only into the explicit notification's eight-byte eventfd write.
// The actual notify catches it, actual wait observes the latch, and Session's
// next control stage converts that transport failure into exit status one.
void test_notification_failure()
{
  SessionInput input;
  tui_test_support::TuiFrontEndFixture frontend;
  SessionProbe probe;
  bool failure_latched = false;
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode) {
    probe.fail_notify_write = true;
    loop.wakeup().notify();
    probe.fail_notify_write = false;
    failure_latched = session_real_notification(&loop);
  };
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 1 && failure_latched && !probe.timed_out && probe.failed_notify_writes == 1,
         "failed real eventfd notification reaches Session's I/O-failure exit");
  expect(probe.turns.size() == 2 && probe.turns.back().stages == std::vector<Stage>{Stage::Control},
         "notification failure prevents another due, application, input, paint or wait stage");
}

// B7: claim a real worker prompt, then inject a failing terminal flush during
// the real deferred paint. Session must retire that claim with Io
// before shutdown can replace its result with an interruption.
void test_prompt_paint(bool permission)
{
  SessionInput input;
  tui_test_support::TuiFrontEndFixture frontend;
  std::atomic_bool worker_io{false};
  std::atomic_bool worker_returned{false};
  frontend.hooks().on_submit = [&](std::string const&, ava::tui::TuiSubmitContext context) {
    if (permission)
    {
      ava::permissions::PermissionPrompt prompt;
      prompt.operation = ava::permissions::Operation::RunCommand;
      prompt.mode = ava::core::Mode::Build;
      prompt.command = "echo permission";
      prompt.tool_name = "bash";
      auto result = context.permission_resolver(prompt);
      worker_io.store(!result && result.error().category() == ava::core::ErrorCategory::Io && result.error().message() == "failed to render permission prompt");
    }
    else
    {
      ava::agent::QuestionPrompt prompt{.header = "Question", .question = "Choose", .options = {{.value = "done", .label = "Done"}}};
      auto result = context.question_resolver(prompt);
      worker_io.store(!result && result.error().category() == ava::core::ErrorCategory::Io && result.error().message() == "failed to render question prompt");
    }
    worker_returned.store(true);
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  bool prompt_active = false;
  bool failure_armed = false;
  probe.after_prompt = [&](ava::tui::PromptServiceOutcome outcome) { prompt_active = prompt_active || outcome == ava::tui::PromptServiceOutcome::Active; };
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    if (!failure_armed && prompt_active)
    {
      // Startup painted synchronously and the input stage drained typeahead.
      // A fresh navigation key requests the ordinary deferred repaint under test.
      native_input("\t");
      probe.fail_paint_flush = true;
      failure_armed = true;
      reach_frame_deadline(probe);
      notify_before_block(loop, mode);
    }
  };
  seed_submission();
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 1 && failure_armed && probe.failed_paint_flushes == 1 && !probe.timed_out && worker_io.load() && worker_returned.load(),
         "actual deferred prompt paint failure returns Io to its joined worker before shutdown");
  expect(std::ranges::any_of(probe.turns, [](Turn const& turn) { return turn.painted == false; }),
         "the real frame renderer reports the injected terminal flush failure");
}

// R18: a SIGTERM pending at an active turn's control checkpoint requests worker
// cancellation before due timers or input, then preserves exit 130 through join.
void test_active_signal_at_control()
{
  SessionInput input;
  SessionSignals signals;
  WorkerGate gate;
  // A second bounded publication lets the worker sample cancellation while the
  // TUI remains at the before-due boundary, before any later phase can cancel it.
  WorkerGate cancellation_checked;
  std::atomic_size_t submissions{0};
  std::atomic_bool released{false};
  std::atomic_bool worker_stopped{false};
  std::atomic_bool worker_returned{false};
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.hooks().on_submit = [gate = &gate, checked = &cancellation_checked, submissions = &submissions, released = &released, stopped = &worker_stopped,
                                returned = &worker_returned](std::string const&, ava::tui::TuiSubmitContext context) {
    if (submissions->fetch_add(1) != 0)
      return ava::tui::TuiSubmitResult{};
    gate->published();
    released->store(gate->wait_for_release());
    stopped->store(context.cancel_requested && context.cancel_requested());
    returned->store(true);
    checked->published();
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  bool injected = false;
  bool control_checked = false;
  bool cancelled_before_due = false;
  probe.before_wait = [gate = &gate, probe = &probe, injected = &injected](TuiEventLoop& loop, TuiWaitMode mode) {
    if (probe->turns.size() == 1)
    {
      expect(gate->wait_for_publication(), "active control worker reaches its bounded gate");
      expect(::raise(SIGTERM) == 0, "inject SIGTERM before the next active control checkpoint");
      *injected = true;
      expect(ava::core::Signals::received(ava::core::Signals::to_mask(SIGTERM)), "active control signal is pending before wait returns");
    }
    notify_before_block(loop, mode);
  };
  probe.before_due = [gate = &gate, checked = &cancellation_checked, probe = &probe, control_checked = &control_checked, cancelled = &cancelled_before_due,
                      stopped = &worker_stopped, returned = &worker_returned](TuiEventLoop&, Clock::time_point) {
    if (probe->turns.size() != 2)
      return;
    *control_checked = true;
    expect(!returned->load(), "active worker remains held until the control checkpoint finishes");
    bool const signal_claimed = !ava::core::Signals::received(ava::core::Signals::to_mask(SIGTERM));
    expect(probe->turn().input.empty(), "active control cancellation precedes all input in its turn");
    gate->release();
    bool const checked_in_time = checked->wait_for_publication();
    *cancelled = signal_claimed && checked_in_time && stopped->load();
  };
  seed_submission();
  int exit_code = -1;
  {
    ProbeScope scope(probe);
    ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
    // On unwind this releases before Session's destructor joins; the ordinary
    // path above releases before run() performs its own shutdown/join.
    ReleaseWorker release(gate);
    exit_code = session.run([] { return true; });
  }
  expect(exit_code == 130 && !probe.timed_out && injected && control_checked && cancelled_before_due,
         "active control SIGTERM cancels before due dispatch and preserves signal exit through worker settlement");
  expect(submissions.load() == 1 && released.load() && worker_returned.load(), "active control test releases and joins exactly one submitted worker");
}

// R18: SIGTERM delivered after decoding x consumes x before active dispatch.
// The held worker keeps the run active, so following y/Enter may still drain
// through the close-state active handler before cancellation settles with 130.
void test_active_signal_after_decode()
{
  SessionInput input;
  SessionSignals signals;
  WorkerGate gate;
  std::atomic_size_t submissions{0};
  std::atomic_bool released{false};
  std::atomic_bool worker_stopped{false};
  std::atomic_bool worker_returned{false};
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.hooks().on_submit = [gate = &gate, submissions = &submissions, released = &released, stopped = &worker_stopped, returned = &worker_returned](
                                   std::string const&, ava::tui::TuiSubmitContext context) {
    if (submissions->fetch_add(1) != 0)
      return ava::tui::TuiSubmitResult{};
    gate->published();
    released->store(gate->wait_for_release());
    stopped->store(context.cancel_requested && context.cancel_requested());
    returned->store(true);
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  bool injected = false;
  bool claimed_before_following_key = false;
  probe.after_poll = [probe = &probe, injected = &injected, claimed = &claimed_before_following_key](ava::tui::runtime_input::InputPollResult const& result) {
    if (probe->turns.size() < 2 || result.kind != InputPollKind::Input || result.input.event.key != ava::tui::terminal::Key::Character)
      return;
    if (!*injected && result.input.event.character == 'x')
    {
      *injected = true;
      expect(::raise(SIGTERM) == 0, "inject active SIGTERM after the real retained x decode");
    }
    else if (*injected && result.input.event.character == 'y')
      *claimed = !ava::core::Signals::received(ava::core::Signals::to_mask(SIGTERM));
  };
  probe.before_wait = [gate = &gate, probe = &probe, injected = &injected, returned = &worker_returned](TuiEventLoop& loop, TuiWaitMode mode) {
    if (probe->turns.size() == 1)
    {
      expect(gate->wait_for_publication(), "post-decode signal worker is active before native keys are buffered");
      native_input("xy\n");
    }
    else if (*injected && !returned->load())
    {
      // Release only after the entire input stage, so worker completion cannot
      // suppress the following-key observation or explain x's missing dispatch.
      gate->release();
    }
    notify_before_block(loop, mode);
  };
  seed_submission();
  int exit_code = -1;
  {
    ProbeScope scope(probe);
    ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
    ReleaseWorker release(gate);
    exit_code = session.run([] { return true; });
  }
  expect(exit_code == 130 && !probe.timed_out && injected && claimed_before_following_key && released.load() && worker_stopped.load() && worker_returned.load(),
         "post-decode active SIGTERM is claimed before another key and cancels the joined worker with signal exit");
  expect(std::ranges::count(probe.active_dispatched_characters, 'x') == 0 && std::ranges::count(probe.active_dispatched_characters, 'y') == 1,
         "the signal's retained x never enters active dispatch while the following y still reaches close-state drainage");
  expect(submissions.load() == 1, "following Enter cannot submit another run after active signal cancellation");
}

// R18: a genuine descriptor HUP after a permission frame is visible returns Deny
// and emits its interrupted audit. Observe the real sink publication because HUP
// skips application consumption and shutdown retires the inbox without draining.
void test_permission_hangup()
{
  SessionInput input;
  WorkerGate gate;
  std::atomic_size_t submissions{0};
  std::atomic_bool released{false};
  std::atomic_bool worker_denied{false};
  std::atomic_bool worker_returned{false};
  // All tee observations are synchronized even if future audit emission moves
  // onto the worker; these owners survive the last copied callback and join.
  std::atomic_size_t audit_requests{0};
  std::atomic_size_t audit_denials{0};
  std::atomic_size_t interrupted_denials{0};
  std::atomic_bool audit_forward_failed{false};
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.hooks().on_submit = [gate = &gate, submissions = &submissions, released = &released, denied = &worker_denied, returned = &worker_returned](
                                   std::string const&, ava::tui::TuiSubmitContext context) {
    if (submissions->fetch_add(1) != 0)
      return ava::tui::TuiSubmitResult{};
    ava::permissions::PermissionPrompt prompt;
    prompt.operation = ava::permissions::Operation::RunCommand;
    prompt.mode = ava::core::Mode::Build;
    prompt.command = "echo permission";
    prompt.tool_name = "bash";
    prompt.permission_request_id = "r18-hup-permission";
    gate->published();
    auto result = context.permission_resolver(prompt);
    denied->store(result && *result == ava::permissions::PermissionResolution::Deny);
    released->store(gate->wait_for_release());
    returned->store(true);
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  bool claimed = false;
  bool visible_before_hup = false;
  bool observed_hup = false;
  std::optional<std::size_t> closed_after_turn;
  probe.after_audit = [requests = &audit_requests, denials = &audit_denials, interrupted = &interrupted_denials, failed = &audit_forward_failed](
                          ava::event::RuntimeEvent const& event, bool accepted) {
    auto const* provider = std::get_if<ava::event::ProviderEvent>(&event.payload());
    if (!provider || provider->payload.permission_request_ids.size() != 1 || provider->payload.permission_request_ids.front() != "r18-hup-permission")
      return;
    if (!accepted)
      failed->store(true);
    auto const& payload = provider->payload;
    if (payload.status == "tui:permission_request")
      requests->fetch_add(1);
    if (payload.status == "tui:permission_deny")
    {
      denials->fetch_add(1);
      if (payload.error_details == "interrupted" && payload.text == "permission denied: interrupted" && payload.tool == "bash")
        interrupted->fetch_add(1);
    }
  };
  probe.after_prompt = [probe = &probe, claimed = &claimed](ava::tui::PromptServiceOutcome outcome) {
    if (!*claimed && outcome == ava::tui::PromptServiceOutcome::Active)
    {
      *claimed = true;
      // Active means the real coordinator owns the claim. Make its requested
      // frame eligible, then wait for an actual successful flush before HUP.
      reach_frame_deadline(*probe);
    }
  };
  probe.before_wait = [input = &input, gate = &gate, probe = &probe, claimed = &claimed, visible = &visible_before_hup, closed = &closed_after_turn](
                          TuiEventLoop& loop, TuiWaitMode mode) {
    if (probe->turns.size() == 1)
      expect(gate->wait_for_publication(), "HUP worker reaches the permission resolver before closure");
    if (!*closed && *claimed && probe->turn().painted == true && probe->turn().flushes > 0)
    {
      *visible = true;
      *closed = probe->turns.size();
      input->close_peer();
      // run() joins during shutdown; release before it enters the HUP control
      // turn. The resolver still keeps the worker pending until real cancellation.
      gate->release();
    }
    notify_before_block(loop, mode);
  };
  probe.after_poll = [probe = &probe, observed = &observed_hup](ava::tui::runtime_input::InputPollResult const&) {
    if (probe->loop && probe->loop->terminal_hangup())
      *observed = true;
  };
  seed_submission();
  int exit_code = -1;
  {
    ProbeScope scope(probe);
    ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
    ReleaseWorker release(gate);
    exit_code = session.run([] { return true; });
  }
  expect(exit_code == 0 && !probe.timed_out && visible_before_hup && observed_hup && released.load() && worker_denied.load() && worker_returned.load(),
         "real HUP after a flushed permission frame returns Deny and joins the worker with normal close exit");
  expect(submissions.load() == 1 && audit_requests.load() == 1 && audit_denials.load() == 1 && interrupted_denials.load() == 1 && !audit_forward_failed.load(),
         "HUP forwards exactly one correlated permission request and interrupted denial through the original audit sink");
  if (!ava::tests::require(closed_after_turn.has_value(), "permission HUP occurs only after a visible prompt"))
    return;
  for (std::size_t index = *closed_after_turn; index < probe.turns.size(); ++index)
    expect(probe.turns[index].stages == std::vector<Stage>{Stage::Control, Stage::Input},
           "permission HUP closes through control/input without an application drain, deferred paint, or another wait");
}

// S1: keys already in startup replay, ncurses, or the fd cannot answer a newly
// painted prompt. The empty boundary permits a later deliberate key instead.
void test_prompt_input_boundary(bool permission, int source)
{
  SessionInput input;
  std::atomic_bool answered{false};
  std::atomic_bool returned{false};
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.hooks().on_submit = [permission, answered = &answered, returned = &returned](std::string const&, ava::tui::TuiSubmitContext context) {
    if (permission)
    {
      ava::permissions::PermissionPrompt prompt;
      prompt.operation = ava::permissions::Operation::RunCommand;
      prompt.mode = ava::core::Mode::Build;
      prompt.command = "echo security";
      prompt.tool_name = "bash";
      auto result = context.permission_resolver(prompt);
      answered->store(result && *result == ava::permissions::PermissionResolution::Allow);
    }
    else
    {
      ava::agent::QuestionPrompt prompt{.header = "Security", .question = "Choose", .options = {{.value = "yes", .label = "Yes"}}};
      auto result = context.question_resolver(prompt);
      answered->store(result && result->selected_options == std::vector<std::string>{"yes"});
    }
    returned->store(true);
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  ava::tui::RuntimePromptCoordinator* prompts = nullptr;
  bool buffered = false;
  bool active = false;
  bool fresh_sent = false;
  bool stale_ignored = false;
  char const key = permission ? 'a' : '1';
  probe.before_prompt = [&](ava::tui::RuntimePromptCoordinator& coordinator) {
    prompts = &coordinator;
    if (buffered || !coordinator.has_pending_requests())
      return;
    buffered = true;
    if (source == 0)
    {
      // More than one turn's budget proves quarantine persists across batches.
      for (std::size_t count = 0; count < 63; ++count)
      {
        ava::tui::runtime_input::RuntimeInput stale;
        stale.event = {.key = ava::tui::terminal::Key::Character, .character = key, .text = std::string(1, key), .mouse_column = 0, .mouse_row = 0};
        stale.text = std::string(1, key);
        expect(ava::tui::runtime_input::enqueue_startup_input(std::move(stale)), "queue stale startup answer key");
      }
    }
    else if (source == 1)
      native_input(std::string(60, key) + "\n");
    else
      expect(input.write_bytes(std::string("\xff", 1) + key + "\n"), "queue invalid UTF-8 followed by stale answer bytes");
  };
  probe.after_prompt = [&](ava::tui::PromptServiceOutcome outcome) { active = active || outcome == ava::tui::PromptServiceOutcome::Active; };
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    if (buffered && prompts && !prompts->has_active() && !fresh_sent)
    {
      loop.request_stop();
      return;
    }
    if (active && !fresh_sent && !probe.turn().input.empty() && probe.turn().input.back() == InputPollKind::Empty)
    {
      stale_ignored = prompts && prompts->has_active() && !returned.load();
      fresh_sent = true;
      native_input(std::string(1, key));
      notify_before_block(loop, mode);
    }
    if (returned.load())
      loop.request_stop();
  };
  seed_submission();
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 0 && !probe.timed_out && buffered && active && stale_ignored && fresh_sent && returned.load() && answered.load(),
         "pre-prompt buffered input is discarded; only a fresh key after drainage answers");
}

// S1: actual synchronous paint failure at startup or answer must release the
// resolver with Io. A selected Allow must not escape before terminal I/O succeeds.
void test_prompt_handoff_failure(bool permission, bool at_answer, std::size_t target_handoff = 1)
{
  SessionInput input;
  std::atomic_bool io{false};
  std::atomic_bool returned{false};
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.hooks().on_submit = [permission, io = &io, returned = &returned](std::string const&, ava::tui::TuiSubmitContext context) {
    if (permission)
    {
      ava::permissions::PermissionPrompt prompt;
      prompt.operation = ava::permissions::Operation::RunCommand;
      prompt.mode = ava::core::Mode::Build;
      prompt.command = "echo security";
      prompt.tool_name = "bash";
      auto result = context.permission_resolver(prompt);
      io->store(!result && result.error().category() == ava::core::ErrorCategory::Io);
    }
    else
    {
      ava::agent::QuestionPrompt prompt{.header = "Security", .question = "Choose", .options = {{.value = "yes", .label = "Yes"}}};
      auto result = context.question_resolver(prompt);
      io->store(!result && result.error().category() == ava::core::ErrorCategory::Io);
    }
    returned->store(true);
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  bool active = false;
  bool fresh_sent = false;
  bool inject = false;
  bool injected = false;
  std::size_t answer_handoffs = 0;
  char const key = permission ? 'a' : '1';
  probe.before_prompt = [&](ava::tui::RuntimePromptCoordinator& coordinator) {
    if (!at_answer && coordinator.has_pending_requests())
      inject = true;
  };
  probe.after_prompt = [&](ava::tui::PromptServiceOutcome outcome) { active = active || outcome == ava::tui::PromptServiceOutcome::Active; };
  probe.after_poll = [&](ava::tui::runtime_input::InputPollResult const& result) {
    if (at_answer && fresh_sent && result.kind == InputPollKind::Input && result.input.event.character == key)
      inject = true;
  };
  probe.before_handoff = [&] {
    if (inject && !injected && ++answer_handoffs == target_handoff)
    {
      injected = true;
      probe.fail_paint_flush = true;
    }
  };
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    if (at_answer && active && !fresh_sent)
    {
      fresh_sent = true;
      native_input(std::string(1, key));
      notify_before_block(loop, mode);
    }
  };
  seed_submission();
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 1 && !probe.timed_out && injected && probe.failed_paint_flushes == 1 && io.load() && returned.load(),
         "failed prompt handoff paint returns Io instead of permission or question success");
  expect(answer_handoffs == target_handoff, "fault reaches the requested startup/answer handoff");
  expect(!at_answer || (active && fresh_sent), "answer failure is injected only after a visible prompt and fresh key");
}

// S1: a visible permission fails closed on explicit shutdown, eventfd failure,
// an exception while repainting, or a signal during a successful answer paint.
void test_permission_exit(int cause)
{
  SessionInput input;
  SessionSignals signals;
  std::atomic_bool denied{false};
  std::atomic_bool returned{false};
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.hooks().on_submit = [denied = &denied, returned = &returned](std::string const&, ava::tui::TuiSubmitContext context) {
    ava::permissions::PermissionPrompt prompt;
    prompt.operation = ava::permissions::Operation::RunCommand;
    prompt.mode = ava::core::Mode::Build;
    prompt.command = "echo security";
    prompt.tool_name = "bash";
    auto result = context.permission_resolver(prompt);
    denied->store(result && *result == ava::permissions::PermissionResolution::Deny);
    returned->store(true);
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  bool active = false;
  bool triggered = false;
  bool answer_sent = false;
  bool caught = false;
  std::size_t answer_handoffs = 0;
  bool signal_sent = false;
  probe.after_prompt = [&](ava::tui::PromptServiceOutcome outcome) { active = active || outcome == ava::tui::PromptServiceOutcome::Active; };
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    if (!active || triggered || answer_sent)
      return;
    if (cause == 0)
    {
      triggered = true;
      loop.request_stop();
    }
    else if (cause == 1)
    {
      triggered = true;
      probe.fail_notify_write = true;
      loop.wakeup().notify();
      probe.fail_notify_write = false;
    }
    else
    {
      answer_sent = true;
      native_input("a");
      notify_before_block(loop, mode);
    }
  };
  probe.before_handoff = [&] {
    if (!answer_sent || triggered)
      return;
    if (++answer_handoffs != (cause == 4 ? 2u : 1u))
      return;
    triggered = true;
    if (cause == 2)
      throw std::runtime_error("injected prompt handoff exception");
  };
  probe.after_handoff = [&] {
    if (cause >= 3 && triggered && !signal_sent)
    {
      signal_sent = true;
      expect(::raise(SIGTERM) == 0, "inject signal after the requested successful permission answer paint");
    }
  };
  seed_submission();
  int exit_code = -1;
  try
  {
    exit_code = run_session(frontend, probe);
  }
  catch (std::runtime_error const& error)
  {
    caught = std::string_view(error.what()) == "injected prompt handoff exception";
  }
  expect(active && triggered && !probe.timed_out && denied.load() && returned.load(), "pending permission never allows on exit or exception");
  if (cause >= 3)
    expect(signal_sent && answer_handoffs == (cause == 4 ? 2u : 1u), "signal reaches the requested answer paint boundary");
  expect(cause == 2 ? caught : exit_code == (cause >= 3 ? 130 : cause == 1 ? 1 : 0), "permission exit preserves its original reason");
}

// S1: a previously granted session recipe must still return Io if its automatic
// status handoff fails. The first request establishes the grant through fresh input.
void test_reused_permission_grant_paint_failure()
{
  SessionInput input;
  std::atomic_bool first_granted{false};
  std::atomic_bool second_io{false};
  std::atomic_bool returned{false};
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.set_session_identity_metadata("security-grant-session");
  frontend.hooks().on_submit = [first = &first_granted, second = &second_io, returned = &returned](std::string const&, ava::tui::TuiSubmitContext context) {
    ava::permissions::CommandPermissionMetadata metadata;
    metadata.level = ava::command::CommandLevel::Standard;
    metadata.containment_available = true;
    metadata.containment_status = ava::permissions::CommandContainmentStatus::Available;
    metadata.backend_maximum_scope = ava::command::InteractiveScope::Session;
    metadata.global_recipe_key = "security-grant-global";
    metadata.workspace_recipe_key = "security-grant-workspace";
    metadata.effective_allowed_scopes = {ava::command::InteractiveScope::Once, ava::command::InteractiveScope::Session};
    ava::permissions::PermissionPrompt prompt;
    prompt.operation = ava::permissions::Operation::RunCommand;
    prompt.mode = ava::core::Mode::Build;
    prompt.command = "echo security";
    prompt.tool_name = "bash";
    prompt.command_metadata = std::move(metadata);
    auto granted = context.permission_resolver(prompt);
    first->store(granted && *granted == ava::permissions::PermissionResolution::AllowSessionGrant);
    auto reused = context.permission_resolver(prompt);
    second->store(!reused && reused.error().category() == ava::core::ErrorCategory::Io);
    returned->store(true);
    return ava::tui::TuiSubmitResult{};
  };
  SessionProbe probe;
  std::size_t requests = 0;
  bool first_active = false;
  bool fresh_sent = false;
  bool fault_injected = false;
  probe.before_prompt = [&](ava::tui::RuntimePromptCoordinator& coordinator) {
    if (coordinator.has_pending_requests())
      ++requests;
  };
  probe.after_prompt = [&](ava::tui::PromptServiceOutcome outcome) {
    if (requests == 1 && outcome == ava::tui::PromptServiceOutcome::Active)
      first_active = true;
  };
  probe.before_wait = [&](TuiEventLoop& loop, TuiWaitMode mode) {
    if (first_active && !fresh_sent)
    {
      fresh_sent = true;
      native_input("s");
      notify_before_block(loop, mode);
    }
  };
  probe.before_handoff = [&] {
    if (requests == 2 && !fault_injected)
    {
      fault_injected = true;
      probe.fail_paint_flush = true;
    }
  };
  seed_submission();
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 1 && !probe.timed_out && requests == 2 && first_active && fresh_sent && first_granted.load() && fault_injected &&
             probe.failed_paint_flushes == 1 && second_io.load() && returned.load(),
         "a matching session grant cannot bypass failure of its automatic handoff paint");
}

} // namespace

// Preserve active dispatch; only copy the character at its actual entry boundary.
extern "C" ava::tui::TuiInputOutcome session_real_active_dispatch(ava::tui::ActiveRunInput*, ava::tui::runtime_input::RuntimeInput const&) asm(
    "__real__ZN3ava3tui14ActiveRunInput8dispatchERKNS0_13runtime_input12RuntimeInputE");
// Observe the retained-key boundary without changing the active input outcome.
extern "C" ava::tui::TuiInputOutcome session_wrap_active_dispatch(ava::tui::ActiveRunInput*, ava::tui::runtime_input::RuntimeInput const&) asm(
    "__wrap__ZN3ava3tui14ActiveRunInput8dispatchERKNS0_13runtime_input12RuntimeInputE");
// Keep coordinator audit installation, its mutex, and endpoint retirement intact.
extern "C" void session_real_set_audit_sink(ava::tui::RuntimePromptCoordinator*, ava::event::RuntimeEventSink) asm(
    "__real__ZN3ava3tui24RuntimePromptCoordinator14set_audit_sinkESt8functionIFSt8expectedIvNS_4core5ErrorEERKNS_5event12RuntimeEventEEE");
// Tee a nonempty endpoint while preserving every real publication and its result.
extern "C" void session_wrap_set_audit_sink(ava::tui::RuntimePromptCoordinator*, ava::event::RuntimeEventSink) asm(
    "__wrap__ZN3ava3tui24RuntimePromptCoordinator14set_audit_sinkESt8functionIFSt8expectedIvNS_4core5ErrorEERKNS_5event12RuntimeEventEEE");

// Exact aliases let GNU ld interpose C++ member calls without a production test
// hook. Member ABI is represented explicitly by the leading borrowed this pointer.
// Call the production timer dispatcher with its original timestamp and borrowed loop.
extern "C" void session_real_due(TuiEventLoop*, Clock::time_point) asm(
    "__real__ZN3ava3tui12TuiEventLoop11service_dueENSt6chrono10time_pointINS2_3_V212steady_clockENS2_8durationIlSt5ratioILl1ELl1000000000EEEEEE");
// Record the TUI due stage before forwarding; optional injection happens after control.
extern "C" void session_wrap_due(TuiEventLoop*, Clock::time_point) asm(
    "__wrap__ZN3ava3tui12TuiEventLoop11service_dueENSt6chrono10time_pointINS2_3_V212steady_clockENS2_8durationIlSt5ratioILl1ELl1000000000EEEEEE");
// Preserve timer registration and interval; selected fixtures adjust only its origin.
extern "C" void session_real_repeat(TuiEventLoop*, std::uint64_t, Clock::duration, Clock::time_point) asm(
    "__real__ZN3ava3tui12TuiEventLoop12repeat_timerEmNSt6chrono8durationIlSt5ratioILl1ELl1000000000EEEENS2_10time_pointINS2_3_V212steady_clockES6_EE");
// Deferral is scoped to fixtures that assert frame/input wait policy in isolation.
extern "C" void session_wrap_repeat(TuiEventLoop*, std::uint64_t, Clock::duration, Clock::time_point) asm(
    "__wrap__ZN3ava3tui12TuiEventLoop12repeat_timerEmNSt6chrono8durationIlSt5ratioILl1ELl1000000000EEEENS2_10time_pointINS2_3_V212steady_clockES6_EE");
// Preserve the production waiter and its borrowed loop resources.
extern "C" void session_real_wait(TuiEventLoop*, TuiWaitMode) asm("__real__ZN3ava3tui12TuiEventLoop4waitENS0_11TuiWaitModeE");
// Observe the selected policy before the real wait; no owner state is exposed.
extern "C" void session_wrap_wait(TuiEventLoop*, TuiWaitMode) asm("__wrap__ZN3ava3tui12TuiEventLoop4waitENS0_11TuiWaitModeE");
// Mark the TUI control stage while returning the real atomic failure latch.
extern "C" bool session_wrap_notification(TuiEventLoop const*) noexcept asm("__wrap__ZNK3ava3tui12TuiEventLoop19notification_failedEv");
// Keep production ActiveRun application updates and their event budget intact.
extern "C" ava::tui::RuntimeEventDrainResult session_real_consume(ava::tui::ActiveRun*, std::size_t) asm("__real__ZN3ava3tui9ActiveRun7consumeEm");
// Scope TUI inbox observations to the real application-update call.
extern "C" ava::tui::RuntimeEventDrainResult session_wrap_consume(ava::tui::ActiveRun*, std::size_t) asm("__wrap__ZN3ava3tui9ActiveRun7consumeEm");
// Take an owned event batch through the production mailbox lock.
extern "C" std::vector<ava::tui::QueuedTuiEvent> session_real_drain(ava::tui::ActiveRunInbox*, std::size_t) asm("__real__ZN3ava3tui14ActiveRunInbox5drainEm");
// Measure real batch size without changing queue ownership or publication order.
extern "C" std::vector<ava::tui::QueuedTuiEvent> session_wrap_drain(ava::tui::ActiveRunInbox*, std::size_t) asm("__wrap__ZN3ava3tui14ActiveRunInbox5drainEm");
// Use the production decoder and startup queue on the registered TUI thread.
extern "C" ava::tui::runtime_input::InputPollResult session_real_poll() asm("__real__ZN3ava3tui13runtime_input17poll_curses_inputEv");
// Observe each real decoded result before Session dispatches its retained input.
extern "C" ava::tui::runtime_input::InputPollResult session_wrap_poll() asm("__wrap__ZN3ava3tui13runtime_input17poll_curses_inputEv");
// Keep production frame eligibility, drawing and failure propagation intact.
extern "C" bool session_real_paint(ava::tui::RuntimeRenderer*) asm("__real__ZN3ava3tui15RuntimeRenderer17paint_ready_frameEv");
// Scope terminal-flush observations to the real TUI paint operation.
extern "C" bool session_wrap_paint(ava::tui::RuntimeRenderer*) asm("__wrap__ZN3ava3tui15RuntimeRenderer17paint_ready_frameEv");
// Preserve the real synchronous paint used when a host prompt takes or releases input.
extern "C" bool session_real_handoff(ava::tui::RuntimeRenderer*) asm("__real__ZN3ava3tui15RuntimeRenderer17paint_for_handoffEv");
// Observe terminal I/O during handoff without changing its result or scheduling.
extern "C" bool session_wrap_handoff(ava::tui::RuntimeRenderer*) asm("__wrap__ZN3ava3tui15RuntimeRenderer17paint_for_handoffEv");
// Use the real coordinator claim and its lifetime-controlled callbacks.
extern "C" ava::tui::PromptServiceOutcome session_real_prompt(ava::tui::RuntimePromptCoordinator*, std::function<bool()>, std::function<void()>) asm(
    "__real__ZN3ava3tui24RuntimePromptCoordinator13start_pendingESt8functionIFbvEES2_IFvvEE");
// Allow fault injection only after the real claim outcome is available.
extern "C" ava::tui::PromptServiceOutcome session_wrap_prompt(ava::tui::RuntimePromptCoordinator*, std::function<bool()>, std::function<void()>) asm(
    "__wrap__ZN3ava3tui24RuntimePromptCoordinator13start_pendingESt8functionIFbvEES2_IFvvEE");
// Forward all writes to libc unless the TUI probe arms a single wake failure.
extern "C" ssize_t __real_write(int, void const*, std::size_t);
// Inject only the scoped eventfd-sized write; producer threads always forward.
extern "C" ssize_t __wrap_write(int, void const*, std::size_t);
// Perform libc flushing before recording or injecting a terminal failure.
extern "C" int __real_fflush(FILE*);
// Observe only Context output during paint; an armed fault lasts one flush.
extern "C" int __wrap_fflush(FILE*);

// A following key may still reach the closing active handler; record actual
// characters separately so consuming the signal's retained key is observable.
extern "C" ava::tui::TuiInputOutcome session_wrap_active_dispatch(ava::tui::ActiveRunInput* input_owner, ava::tui::runtime_input::RuntimeInput const& input)
{
  if (auto* probe = session_probe; probe && input.event.key == ava::tui::terminal::Key::Character)
    probe->active_dispatched_characters.push_back(input.event.character);
  return session_real_active_dispatch(input_owner, input);
}

// The copied observer never borrows thread_local SessionProbe. Its test-owned
// atomics outlive Session; the original mailbox endpoint owns its normal state.
// Empty replacements remain empty, preserving coordinator audit retirement.
extern "C" void session_wrap_set_audit_sink(ava::tui::RuntimePromptCoordinator* coordinator, ava::event::RuntimeEventSink sink)
{
  if (auto* probe = session_probe; probe && sink && probe->after_audit)
  {
    sink = [sink = std::move(sink), observe = probe->after_audit](ava::event::RuntimeEvent const& event) -> ava::core::VoidResult {
      auto result = sink(event);
      observe(event, result.has_value());
      return result;
    };
  }
  session_real_set_audit_sink(coordinator, std::move(sink));
}

// Observe exactly the real due call, with optional signal injection at its boundary.
extern "C" void session_wrap_due(TuiEventLoop* loop, Clock::time_point now)
{
  auto* probe = session_probe;
  if (probe)
  {
    probe->loop = loop;
    probe->turn().stages.push_back(Stage::Due);
    if (probe->before_due)
      probe->before_due(*loop, now);
  }
  session_real_due(loop, now);
  if (probe && probe->after_due)
    probe->after_due(*loop);
}

// Recurring timers remain real registrations, but these two cases cannot reach
// them before their watchdog. One-shot frame timers retain their original timing.
extern "C" void session_wrap_repeat(TuiEventLoop* loop, std::uint64_t id, Clock::duration interval, Clock::time_point now)
{
  if (auto* probe = session_probe)
  {
    if (interval == std::chrono::milliseconds(100) && !probe->control_epoch)
      probe->control_epoch = now;
    if (interval == std::chrono::milliseconds(500) && !probe->display_epoch)
      probe->display_epoch = now;
  }
  if (auto* probe = session_probe; probe && probe->defer_recurring_timers)
    now = probe->deadline + std::chrono::seconds(1);
  session_real_repeat(loop, id, interval, now);
}

// Preserve the mode chosen by Session and always execute the real wait afterward.
extern "C" void session_wrap_wait(TuiEventLoop* loop, TuiWaitMode mode)
{
  if (auto* probe = session_probe)
  {
    probe->loop = loop;
    probe->turn().stages.push_back(Stage::Wait);
    probe->turn().wait = mode;
    probe->check_deadline();
    if (probe->before_wait)
      probe->before_wait(*loop, mode);
  }
  session_real_wait(loop, mode);
}

// This external Session call marks control without invoking any control action;
// TuiEventLoop's own same-object internal calls do not pass through GNU ld wraps.
extern "C" bool session_wrap_notification(TuiEventLoop const* loop) noexcept
{
  if (auto* probe = session_probe)
  {
    probe->turns.emplace_back();
    probe->check_deadline();
  }
  return session_real_notification(loop);
}

// Nest real inbox observations inside actual application consumption only.
extern "C" ava::tui::RuntimeEventDrainResult session_wrap_consume(ava::tui::ActiveRun* run, std::size_t maximum)
{
  auto* probe = session_probe;
  if (probe)
  {
    probe->turn().stages.push_back(Stage::Application);
    probe->consuming = true;
  }
  auto const result = session_real_consume(run, maximum);
  if (probe)
  {
    probe->consuming = false;
    probe->turn().application_results.push_back(result);
  }
  return result;
}

// Record returned sizes while preserving the real mailbox's mutex and FIFO work.
extern "C" std::vector<ava::tui::QueuedTuiEvent> session_wrap_drain(ava::tui::ActiveRunInbox* inbox, std::size_t maximum)
{
  auto result = session_real_drain(inbox, maximum);
  if (auto* probe = session_probe; probe && probe->consuming)
    probe->turn().drains.push_back(result.size());
  return result;
}

// Count the decoder's genuine three-way results; scenario callbacks run only
// after native decoding and before Session can dispatch the retained key.
extern "C" ava::tui::runtime_input::InputPollResult session_wrap_poll()
{
  auto* probe = session_probe;
  if (probe && probe->turn().input.empty())
    probe->turn().stages.push_back(Stage::Input);
  auto result = session_real_poll();
  if (probe)
  {
    probe->turn().input.push_back(result.kind);
    if (probe->after_poll)
      probe->after_poll(result);
  }
  return result;
}

// Observe actual painting and bound fflush counting to this call's dynamic extent.
extern "C" bool session_wrap_paint(ava::tui::RuntimeRenderer* renderer)
{
  auto* probe = session_probe;
  if (probe)
  {
    probe->turn().stages.push_back(Stage::Paint);
    probe->painting = true;
  }
  bool const result = session_real_paint(renderer);
  if (probe)
  {
    probe->painting = false;
    probe->turn().painted = result;
    probe->previous_paint = Clock::now();
  }
  return result;
}

// Bound flush observation to actual handoff painting. Initial Session painting
// precedes the first loop turn, so it has no per-turn observation to update.
extern "C" bool session_wrap_handoff(ava::tui::RuntimeRenderer* renderer)
{
  auto* probe = session_probe;
  if (!probe || probe->turns.empty())
    return session_real_handoff(renderer);
  if (probe->before_handoff)
    probe->before_handoff();
  auto const previous = std::exchange(probe->painting, true);
  bool const result = session_real_handoff(renderer);
  probe->painting = previous;
  if (probe->after_handoff)
    probe->after_handoff();
  return result;
}

// Prompt scenarios surround the real claim; buffered keys can be staged before
// startup, while after_prompt observes the completed paint/claim outcome.
extern "C" ava::tui::PromptServiceOutcome session_wrap_prompt(ava::tui::RuntimePromptCoordinator* coordinator, std::function<bool()> stop_requested,
                                                              std::function<void()> before_prompt)
{
  if (auto* probe = session_probe; probe && probe->before_prompt)
    probe->before_prompt(*coordinator);
  auto const result = session_real_prompt(coordinator, std::move(stop_requested), std::move(before_prompt));
  if (auto* probe = session_probe; probe && probe->after_prompt)
    probe->after_prompt(result);
  return result;
}

// Fail exactly one eventfd-sized write in the explicitly armed TUI notification;
// all worker notifications, terminal output and unrelated descriptors forward.
extern "C" ssize_t __wrap_write(int descriptor, void const* data, std::size_t size)
{
  if (auto* probe = session_probe; probe && probe->fail_notify_write && size == sizeof(std::uint64_t))
  {
    probe->fail_notify_write = false;
    ++probe->failed_notify_writes;
    errno = EIO;
    return -1;
  }
  return __real_write(descriptor, data, size);
}

// Count Context flushes inside the real deferred paint. One armed failure tests
// propagation through Context and renderer; a /dev/full endpoint alone is not
// sufficient because ncurses can consume its write error before this final flush.
extern "C" int __wrap_fflush(FILE* stream)
{
  auto const result = __real_fflush(stream);
  if (auto* probe = session_probe; probe && probe->painting && stream == probe->output)
  {
    ++probe->turn().flushes;
    if (std::exchange(probe->fail_paint_flush, false))
    {
      ++probe->failed_paint_flushes;
      errno = EIO;
      return EOF;
    }
  }
  return result;
}

// The core runner initializes Application's Context first and dispatches exactly
// one of these cases per process, preserving ncurses and signal isolation.
void run_tui_session_test_case(std::string_view test_case)
{
  static_cast<void>(ava::tui::terminal::Context::resizeterm(24, 120));
  // resizeterm can enqueue KEY_RESIZE. Drain that setup event before observing
  // Session so the idle control-timer case has no unrelated frame wake pending.
  ava::tui::runtime_input::clear_startup_input_queue();
  for (std::size_t remaining = 0; remaining < 256; ++remaining)
    if (ava::tui::runtime_input::poll_curses_input().kind == InputPollKind::Empty)
      break;
  if (test_case == "shutdown_outcome" || test_case == "shutdown_cleanup")
    run_tui_shutdown_test_case(test_case);
  else if (test_case == "turn_order")
    test_turn_order();
  else if (test_case == "close_after_submit")
    test_close_after_submit_drains_input();
  else if (test_case == "active_signal_at_control")
    test_active_signal_at_control();
  else if (test_case == "active_signal_after_decode")
    test_active_signal_after_decode();
  else if (test_case == "permission_hangup")
    test_permission_hangup();
  else if (test_case == "prompt_security_permission" || test_case == "prompt_security_question")
  {
    bool const permission = test_case == "prompt_security_permission";
    for (int source : {0, 1, 2})
      test_prompt_input_boundary(permission, source);
    for (bool at_answer : {false, true})
      test_prompt_handoff_failure(permission, at_answer);
    if (permission)
    {
      test_prompt_handoff_failure(true, true, 2);
      for (int cause : {0, 1, 2, 3, 4})
        test_permission_exit(cause);
      test_reused_permission_grant_paint_failure();
    }
  }
  else if (test_case == "frame_deadline")
    test_frame_deadline();
  else if (test_case == "hangup")
    test_closing_drain(false);
  else if (test_case == "descriptor_error")
    test_closing_drain(true);
  else if (test_case == "control_timer")
    test_control_timer();
  else if (test_case == "signal_after_decode")
    test_signal_after_decode();
  else if (test_case == "notification_failure")
    test_notification_failure();
  else if (test_case == "prompt_paint_permission")
    test_prompt_paint(true);
  else if (test_case == "prompt_paint_question")
    test_prompt_paint(false);
  else
    expect(false, "unknown TuiSession case: " + std::string(test_case));
}
