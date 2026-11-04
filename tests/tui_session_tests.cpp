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
#include <vector>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

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
    ava::tui::runtime_input::clear_startup_input_queue();
  }
  // Destroy only after Session joined all workers; discard retained keys before
  // restoring the original descriptor so another case cannot inherit input.
  ~SessionInput()
  {
    ava::tui::runtime_input::clear_startup_input_queue();
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
  bool failure_armed = false;
  probe.after_prompt = [&](ava::tui::PromptServiceOutcome outcome) {
    if (!failure_armed && outcome == ava::tui::PromptServiceOutcome::Active)
    {
      probe.fail_paint_flush = true;
      failure_armed = true;
      reach_frame_deadline(probe);
    }
  };
  seed_submission();
  auto const exit_code = run_session(frontend, probe);
  expect(exit_code == 1 && failure_armed && probe.failed_paint_flushes == 1 && !probe.timed_out && worker_io.load() && worker_returned.load(),
         "actual deferred prompt paint failure returns Io to its joined worker before shutdown");
  expect(std::ranges::any_of(probe.turns, [](Turn const& turn) { return turn.painted == false; }),
         "the real frame renderer reports the injected terminal flush failure");
}

} // namespace

// Exact aliases let GNU ld interpose C++ member calls without a production test
// hook. Member ABI is represented explicitly by the leading borrowed this pointer.
// Call the production timer dispatcher with its original timestamp and borrowed loop.
extern "C" void session_real_due(TuiEventLoop*, Clock::time_point) asm(
    "__real__ZN3ava3tui12TuiEventLoop11service_dueENSt6chrono10time_pointINS2_3_V212steady_clockENS2_8durationIlSt5ratioILl1ELl1000000000EEEEEE");
// Record the TUI due stage before forwarding; optional injection happens after control.
extern "C" void session_wrap_due(TuiEventLoop*, Clock::time_point) asm(
    "__wrap__ZN3ava3tui12TuiEventLoop11service_dueENSt6chrono10time_pointINS2_3_V212steady_clockENS2_8durationIlSt5ratioILl1ELl1000000000EEEEEE");
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

// Prompt failure scenarios act only after the real claim and scheduling complete.
extern "C" ava::tui::PromptServiceOutcome session_wrap_prompt(ava::tui::RuntimePromptCoordinator* coordinator, std::function<bool()> stop_requested,
                                                              std::function<void()> before_prompt)
{
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
  if (test_case == "turn_order")
    test_turn_order();
  else if (test_case == "close_after_submit")
    test_close_after_submit_drains_input();
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
