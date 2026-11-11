// Exercise OS readiness and timer ownership without a terminal decoder or provider.
#include "sys.h"
#include "tests/support/TuiEventLoopFixture.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/tui/Spinner.h"
#include "ava/tui/TuiTimeoutHandler.h"
#include "ava/tui/TuiTimerHandle.h"
#include "ava/tui/composer_internal.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using ava::tui::TuiTimeoutHandler;
using ava::tui::TuiTimerHandle;
using Fixture = tui_test_support::TuiEventLoopFixture;

// Test-thread RAII pipe for readiness and hangup cases; the loop borrows its read end.
class EventLoopPipe
{
 public:
  // Open both ends before constructing the loop; failure leaves valid() false.
  EventLoopPipe()
  {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe(descriptors.data()) != 0)
      return;
    read_.reset(descriptors[0]);
    write_.reset(descriptors[1]);
    valid_ = ::fcntl(read_.get(), F_SETFL, O_NONBLOCK) == 0 && ::fcntl(write_.get(), F_SETFL, O_NONBLOCK) == 0 &&
             ::fcntl(read_.get(), F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(write_.get(), F_SETFD, FD_CLOEXEC) == 0;
  }

  // Test-thread admission check before any descriptor-dependent assertion.
  [[nodiscard]] bool valid() const { return valid_; }
  // Borrow the read descriptor while this fixture outlives its loop.
  [[nodiscard]] int input() const { return read_.get(); }
  // Test producer: make one byte readable without going through the wake channel.
  [[nodiscard]] bool publish() const { return ::write(write_.get(), "x", 1) == 1; }
  // Test-thread consumer: acknowledge one readable byte after the loop reports input.
  [[nodiscard]] bool consume() const
  {
    char value = 0;
    return ::read(read_.get(), &value, 1) == 1 && value == 'x';
  }
  // Test thread: create HUP while leaving the read end and buffered bytes alive.
  void close_writer() { write_.reset(); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  ava::core::UniqueFd read_; // Test-owned input end; destroyed after the borrowing loop.
  ava::core::UniqueFd write_; // Test producer end; explicit close drives the HUP case.
  bool valid_ = false; // Construction result; immutable after setup on the test thread.
};

// Test-thread callback target recording supplied deadlines; outlives its timer handle.
class TimerRecorder
{
 public:
  // Timer dispatch records the pass time so late-repeat behavior can be asserted.
  void tick(Clock::time_point now) { ticks_.push_back(now); }
  // Test-thread read after dispatch; the returned borrow lasts with this recorder.
  [[nodiscard]] std::vector<Clock::time_point> const& ticks() const { return ticks_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::vector<Clock::time_point> ticks_; // Test-thread dispatch history, owned until assertions finish.
};

// Test-thread spinner sink that records published frame identities, not terminal output.
class SpinnerFrames
{
 public:
  // Spinner callback: retain each published phase for stop/restart assertions.
  void publish(std::size_t frame) { frames_.push_back(frame); }
  // Test-thread inspection after timer dispatch; borrow expires with the sink.
  [[nodiscard]] std::vector<std::size_t> const& frames() const { return frames_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::vector<std::size_t> frames_; // Test-thread callback history; no renderer is involved.
};

// Mutation exercised by whichever same-deadline callback runs first.
enum class PeerTimerAction
{
  Disarm, // Keep the peer timer but disarm its captured deadline.
  Rearm, // Replace the peer generation so the captured pass must skip it.
  Destroy // Remove the peer timer while the registry traverses its snapshot.
};

// The pair owns its handles and remains at one address until they are retired.
// Either callback may run first; it invalidates the other captured generation.
class MutatingTimerPair
{
 public:
  // Test thread: register stable targets; the registry must outlive both optional handles.
  MutatingTimerPair(TuiTimeoutHandler& tui_timeout_handler, PeerTimerAction action)
      : action_(action),
        first_(tui_timeout_handler.create_timer(*this, &MutatingTimerPair::first_tick)),
        second_(tui_timeout_handler.create_timer(*this, &MutatingTimerPair::second_tick))
  {
  }
  // Both timers retain this stable callback target until their handles retire.
  MutatingTimerPair(MutatingTimerPair const&) = delete;
  MutatingTimerPair& operator=(MutatingTimerPair const&) = delete;

  // Give both timers one deadline so either dispatch order exercises invalidation.
  void arm(Clock::time_point when)
  {
    first_->arm_at(when);
    second_->arm_at(when);
  }
  // Test-thread inspection after dispatch; counts only callbacks actually invoked.
  [[nodiscard]] std::size_t calls() const { return calls_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // First target invalidates the peer captured in the same due pass.
  void first_tick(Clock::time_point now) { mutate(second_, now); }
  // Symmetric target avoids assuming unordered timer traversal order.
  void second_tick(Clock::time_point now) { mutate(first_, now); }
  // Timer callback on the test thread: mutate timers only, never wait or paint.
  void mutate(std::optional<TuiTimerHandle>& peer, Clock::time_point now)
  {
    ++calls_;
    switch (action_)
    {
      case PeerTimerAction::Disarm:
        peer->disarm();
        break;
      case PeerTimerAction::Rearm:
        peer->arm_at(now + 10ms);
        break;
      case PeerTimerAction::Destroy:
        peer.reset();
        break;
    }
  }

  PeerTimerAction action_; // Immutable test scenario selected before registration.
  std::size_t calls_ = 0; // Test-thread callback count; observed only after dispatch.
  std::optional<TuiTimerHandle> first_; // Owned timer; optional permits destruction from its peer.
  std::optional<TuiTimerHandle> second_; // Owned peer timer; registry outlives this handle.
};

// Stable test-thread callback target that replaces its own repeat with a one-shot deadline.
class RearmingTimer
{
 public:
  // Register before arming; the borrowed registry outlives this owner and its handle.
  explicit RearmingTimer(TuiTimeoutHandler& tui_timeout_handler) : timer_(tui_timeout_handler.create_timer(*this, &RearmingTimer::tick)) { }
  // The timer keeps this callback target stable throughout the self-rearm scenario.
  RearmingTimer(RearmingTimer const&) = delete;
  RearmingTimer& operator=(RearmingTimer const&) = delete;

  // Test thread: begin repeating before the callback deliberately replaces its cadence.
  void start(Clock::time_point now) { timer_.arm_every(10ms, now); }
  // Test-thread inspection after dispatch; counts only callbacks actually invoked.
  [[nodiscard]] std::size_t calls() const { return calls_; }
  // Test-thread inspection proves the one-shot timer retires after firing.
  [[nodiscard]] bool active() { return timer_.armed(); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Timer callback replaces its generation; the registry must preserve that replacement.
  void tick(Clock::time_point now)
  {
    ++calls_;
    if (calls_ == 1)
      timer_.arm_at(now);
  }

  std::size_t calls_ = 0; // Test-thread callback count; observed only after dispatch.
  TuiTimerHandle timer_; // Test-thread handle destroyed before the borrowed registry.
};

// Protect deadline replacement and skipping missed repeat ticks instead of replaying a burst.
void test_timer_deadline_replacement_and_late_repeat()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer test input pipe"))
    return;
  TuiTimeoutHandler tui_timeout_handler;
  TimerRecorder one_shot;
  TimerRecorder repeating;
  auto timer = tui_timeout_handler.create_timer(one_shot, &TimerRecorder::tick);
  auto repeat = tui_timeout_handler.create_timer(repeating, &TimerRecorder::tick);
  auto const start = Clock::time_point{};
  expect(!timer.armed() && !repeat.armed(), "member timer registrations begin disarmed");

  timer.arm_at(start + 10ms);
  timer.arm_after(20ms, start);
  tui_timeout_handler.expire(start + 19ms);
  expect(one_shot.ticks().empty(), "rearming replaces the old one-shot deadline");
  tui_timeout_handler.expire(start + 20ms);
  tui_timeout_handler.expire(start + 100ms);
  expect(one_shot.ticks() == std::vector{start + 20ms} && !timer.armed(), "one-shot dispatch uses the supplied timestamp and disarms before later service");

  auto const repeat_start = start + 100ms;
  repeat.arm_every(10ms, repeat_start);
  tui_timeout_handler.expire(repeat_start + 9ms);
  expect(repeating.ticks().empty(), "a repeating timer starts after its first full interval");
  tui_timeout_handler.expire(repeat_start + 35ms);
  tui_timeout_handler.expire(repeat_start + 35ms);
  tui_timeout_handler.expire(repeat_start + 39ms);
  expect(repeating.ticks() == std::vector{repeat_start + 35ms} && repeat.armed(), "an overdue repeat fires once and skips missed callbacks");
  tui_timeout_handler.expire(repeat_start + 40ms);
  repeat.disarm();
  tui_timeout_handler.expire(repeat_start + 100ms);
  expect(repeating.ticks() == std::vector{repeat_start + 35ms, repeat_start + 40ms} && !repeat.armed(),
         "late repeat preserves its original phase and cancellation suppresses later ticks");
}

// Protect a due-timer snapshot when another callback cancels, rearms or destroys its peer.
void test_timer_mutation_during_dispatch()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer mutation input pipe"))
    return;
  TuiTimeoutHandler tui_timeout_handler;
  auto due = Clock::time_point{} + 10ms;
  for (auto action : {PeerTimerAction::Disarm, PeerTimerAction::Rearm, PeerTimerAction::Destroy})
  {
    MutatingTimerPair pair(tui_timeout_handler, action);
    pair.arm(due);
    tui_timeout_handler.expire(due);
    expect(pair.calls() == 1, "cancel, rearm, or destruction suppresses a peer timer's already captured due generation");
    tui_timeout_handler.expire(due + 9ms);
    expect(pair.calls() == 1, "a changed due registration is not replayed by the following service pass");
    tui_timeout_handler.expire(due + 10ms);
    expect(pair.calls() == (action == PeerTimerAction::Rearm ? 2U : 1U), "only explicit rearming admits a later peer callback");
    due += 30ms;
  }

  RearmingTimer self(tui_timeout_handler);
  self.start(due - 10ms);
  tui_timeout_handler.expire(due);
  expect(self.calls() == 1 && self.active(), "self-rearming for now does not recurse in the current due batch");
  tui_timeout_handler.expire(due);
  expect(self.calls() == 2 && !self.active(), "callback rearming replaces the repeat and becomes eligible on the next service pass");
}

// Protect timer ownership when timer handles move or disappear before dispatch.
void test_timer_move_and_destruction()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer ownership input pipe"))
    return;
  TuiTimeoutHandler tui_timeout_handler;
  TimerRecorder source_target;
  TimerRecorder displaced_target;
  auto source = tui_timeout_handler.create_timer(source_target, &TimerRecorder::tick);
  auto const due = Clock::time_point{} + 10ms;
  source.arm_at(due);
  auto moved = std::move(source);
  expect(!source.armed() && moved.armed(), "moving a timer transfers its armed registration");
  tui_timeout_handler.expire(due);
  expect(source_target.ticks().size() == 1, "a moved timer fires exactly once on its original target");

  auto destination = tui_timeout_handler.create_timer(displaced_target, &TimerRecorder::tick);
  destination.arm_at(due + 10ms);
  moved.arm_at(due + 10ms);
  destination = std::move(moved);
  tui_timeout_handler.expire(due + 10ms);
  expect(!moved.armed() && source_target.ticks().size() == 2 && displaced_target.ticks().empty(),
         "move assignment retires the destination registration before taking the source");
  {
    auto destroyed = tui_timeout_handler.create_timer(displaced_target, &TimerRecorder::tick);
    destroyed.arm_at(due + 20ms);
  }
  tui_timeout_handler.expire(due + 20ms);
  expect(displaced_target.ticks().empty(), "destroying an armed registration prevents its callback");
}

// Protect spinner phase through stop/restart and avoid catch-up animation after a delayed turn.
void test_spinner_preserves_phase_and_skips_missed_ticks()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create spinner test input pipe"))
    return;
  TuiTimeoutHandler tui_timeout_handler;
  SpinnerFrames frames;
  ava::tui::Spinner spinner(tui_timeout_handler, std::bind_front(&SpinnerFrames::publish, &frames));
  auto const before_start = Clock::now();
  spinner.start(7);
  auto const after_start = Clock::now();
  tui_timeout_handler.expire(before_start + 119ms);
  expect(spinner.active() && frames.frames().empty() && ava::tui::detail::kProcessingIndicatorFrameDelay == 120ms,
         "spinner preserves the starting phase and schedules its first update after 120ms");
  tui_timeout_handler.expire(after_start + 120ms);
  tui_timeout_handler.expire(after_start + 900ms);
  expect(frames.frames() == std::vector<std::size_t>{8, 9}, "late spinner servicing advances one visible phase without catch-up frames");
  spinner.stop();
  tui_timeout_handler.expire(after_start + 1200ms);
  expect(!spinner.active() && frames.frames() == std::vector<std::size_t>{8, 9}, "stopping the spinner retires future phase publication");
}

// Protect publication before waiting and retain both notification and input readiness in one result.
void test_notification_before_wait_and_simultaneous_input()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create notification test input pipe"))
    return;
  Fixture event_loop(pipe.input());
  TuiTimeoutHandler& tui_timeout_handler = event_loop.tui_timeout_handler();
  TimerRecorder fallback;
  auto deadline = tui_timeout_handler.create_timer(fallback, &TimerRecorder::tick);
  auto wake = event_loop.wakeup();
  auto copied_wake = wake;

  // A fallback deadline bounds a regressed lost notification without requiring
  // another thread or a sleep to establish that publication preceded the wait.
  deadline.arm_at(ava::tests::now_plus_seconds(2));
  wake.notify();
  copied_wake.notify();
  wake.notify();
  event_loop.wait();
  tui_timeout_handler.expire(Clock::now());
  if (!ava::tests::require(fallback.ticks().empty() && event_loop.notified(),
                           "notifications published before wait wake it without reaching the fallback deadline"))
    return;

  // No input is readable. If the first wait left duplicate wake hints behind,
  // this wait would return before the armed deadline becomes due.
  event_loop.wait();
  tui_timeout_handler.expire(Clock::now());
  if (!ava::tests::require(fallback.ticks().size() == 1 && !event_loop.notified(), "repeated notifications coalesce and are consumed by one wait"))
    return;

  if (!ava::tests::require(pipe.publish(), "publish a final byte alongside a fresh notification"))
    return;
  deadline.arm_at(ava::tests::now_plus_seconds(2));
  copied_wake.notify();
  event_loop.wait();
  tui_timeout_handler.expire(Clock::now());
  if (!ava::tests::require(fallback.ticks().size() == 1 && event_loop.notified() && pipe.consume(),
                           "simultaneous input and a fresh wake return together without consuming the input byte"))
    return;
  event_loop.wait();
  tui_timeout_handler.expire(Clock::now());
  expect(fallback.ticks().size() == 2 && !event_loop.notified(), "simultaneous fd readiness does not leave its notification pending for another wait");
}

// Protect the final readable bytes when hangup and input arrive together.
void test_hangup_preserves_final_input()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create hangup test input pipe"))
    return;
  Fixture event_loop(pipe.input());
  if (!ava::tests::require(pipe.publish(), "publish input before terminal hangup"))
    return;
  pipe.close_writer();
  event_loop.wait(Fixture::Wait::ReadyOnly);
  expect(event_loop.terminal_hangup() && !event_loop.terminal_error() && pipe.consume(), "terminal hangup preserves the final readable byte");
  event_loop.wait(Fixture::Wait::ReadyOnly);
  expect(event_loop.terminal_hangup() && !event_loop.terminal_error(), "terminal hangup stays observable after final input is drained");
}

// Protect the earliest-deadline query that TuiFrontEnd::run uses to choose between wait and wait_until.
void test_next_deadline()
{
  TuiTimeoutHandler tui_timeout_handler;
  TimerRecorder first;
  TimerRecorder second;
  auto early = tui_timeout_handler.create_timer(first, &TimerRecorder::tick);
  auto late = tui_timeout_handler.create_timer(second, &TimerRecorder::tick);
  auto const start = Clock::time_point{} + 1s;
  expect(!tui_timeout_handler.next_deadline(), "a registry without armed timers has no deadline");
  late.arm_at(start + 20ms);
  early.arm_at(start + 10ms);
  expect(tui_timeout_handler.next_deadline() == start + 10ms, "the earliest armed deadline wins");
  early.disarm();
  expect(tui_timeout_handler.next_deadline() == start + 20ms, "a canceled timer no longer constrains the wait");
  tui_timeout_handler.expire(start + 20ms);
  expect(!tui_timeout_handler.next_deadline() && second.ticks().size() == 1, "a dispatched one-shot timer no longer constrains the wait");
}

// Protect creating and destroying disarmed timers on other threads through a const registry.
void test_timer_creation_on_other_threads()
{
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kKeptPerThread = 200;
  TuiTimeoutHandler tui_timeout_handler;
  TuiTimeoutHandler const& const_registry = tui_timeout_handler;
  std::array<TimerRecorder, kThreads> recorders;
  std::array<std::vector<TuiTimerHandle>, kThreads> handles;
  {
    std::vector<std::jthread> threads;
    for (std::size_t index = 0; index < kThreads; ++index)
    {
      threads.emplace_back([&const_registry, &recorder = recorders[index], &owned = handles[index]] {
        for (std::size_t count = 0; count < kKeptPerThread; ++count)
        {
          owned.push_back(const_registry.create_timer(recorder, &TimerRecorder::tick));
          // This one has to be gone again, without disturbing the timers of the other threads.
          [[maybe_unused]] auto const discarded = const_registry.create_timer(recorder, &TimerRecorder::tick);
        }
      });
    }
  }
  expect(!tui_timeout_handler.next_deadline(), "timers created on other threads start out disarmed");

  auto const deadline = Clock::time_point{} + 1s;
  for (auto& owned : handles)
    for (auto& handle : owned)
      handle.arm_at(deadline);
  tui_timeout_handler.expire(deadline);
  for (auto const& recorder : recorders)
    expect(recorder.ticks().size() == kKeptPerThread, "every timer created on another thread has its own identity and runs once");
}

// Protect the deadline that expire returns, which is what TuiFrontEnd::run sleeps until.
void test_expire_returns_next_deadline()
{
  TuiTimeoutHandler tui_timeout_handler;
  TimerRecorder first;
  TimerRecorder second;
  auto early = tui_timeout_handler.create_timer(first, &TimerRecorder::tick);
  auto late = tui_timeout_handler.create_timer(second, &TimerRecorder::tick);
  auto const start = Clock::time_point{} + 1s;
  expect(!tui_timeout_handler.expire(start), "without armed timers expire returns no deadline");
  late.arm_at(start + 20ms);
  early.arm_at(start + 10ms);
  expect(tui_timeout_handler.expire(start) == start + 10ms && first.ticks().empty(), "before any deadline expire runs nothing and returns the earliest");
  expect(tui_timeout_handler.expire(start + 10ms) == start + 20ms && first.ticks().size() == 1 && second.ticks().empty(),
         "expire runs the due timer and returns the deadline of the next one");
  early.arm_every(5ms, start + 10ms);
  expect(tui_timeout_handler.expire(start + 12ms) == start + 15ms, "a newly armed earlier deadline is returned");
  expect(tui_timeout_handler.expire(start + 20ms) == start + 25ms && first.ticks().size() == 2 && second.ticks().size() == 1,
         "expire runs every due timer and returns the rescheduled repeat");
  late.arm_at(start + 22ms);
  tui_timeout_handler.stop();
  expect(!tui_timeout_handler.expire(start + 30ms) && first.ticks().size() == 2 && second.ticks().size() == 1 && !tui_timeout_handler.next_deadline(),
         "after stop expire runs nothing and returns no deadline");
}

// Protect waking the TUI thread when another thread arms a timer before the deadline it sleeps until.
void test_arming_on_other_threads_wakes_the_loop()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer wakeup test input pipe"))
    return;
  Fixture event_loop(pipe.input());
  TuiTimeoutHandler& tui_timeout_handler = event_loop.tui_timeout_handler();
  // The test thread plays the TUI thread.
  tui_timeout_handler.attach(event_loop.wakeup());
  TimerRecorder recorder;
  auto timer = tui_timeout_handler.create_timer(recorder, &TimerRecorder::tick);
  // The owner of the handle moves to another thread for the duration of one arm.
  auto const arm_on_other_thread = [&timer](Clock::time_point deadline) { std::jthread([&timer, deadline] { timer.arm_at(deadline); }).join(); };
  auto const woken = [&event_loop] {
    event_loop.wait(Fixture::Wait::ReadyOnly);
    return event_loop.notified();
  };
  auto const now = Clock::now();

  expect(!tui_timeout_handler.expire(now), "the loop sleeps without deadline");
  arm_on_other_thread(now + 1h);
  expect(woken(), "arming on another thread wakes a loop that sleeps without deadline");
  arm_on_other_thread(now + 30min);
  expect(!woken(), "arming again before the loop called expire does not wake it again");
  expect(tui_timeout_handler.expire(now) == now + 30min, "expire returns the deadline that was armed on another thread");
  arm_on_other_thread(now + 2h);
  expect(!woken(), "a deadline after the one the loop sleeps until does not wake it");
  expect(tui_timeout_handler.expire(now) == now + 2h, "rearming replaced the deadline");
  arm_on_other_thread(now + 10min);
  expect(woken(), "a deadline before the one the loop sleeps until wakes it");
  expect(tui_timeout_handler.expire(now) == now + 10min, "the loop sleeps until the new deadline");
  timer.arm_at(now + 1min);
  expect(!woken(), "arming on the TUI thread itself never wakes it");
  expect(tui_timeout_handler.next_deadline() == now + 1min, "next_deadline sees what the TUI thread armed after expire");
  arm_on_other_thread(now + 30s);
  expect(woken(), "next_deadline recorded the earlier deadline that the loop now sleeps until");
  tui_timeout_handler.stop();
  arm_on_other_thread(now + 1s);
  expect(!woken() && !tui_timeout_handler.expire(now + 1h) && recorder.ticks().empty(), "after stop arming wakes nothing and nothing runs");
}

// Protect safe notification through an expired weak wake handle.
void test_retired_wakeup()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create retired wakeup test input pipe"))
    return;
  ava::tui::TuiWakeup retired_wake;
  retired_wake.notify();
  {
    Fixture event_loop(pipe.input());
    retired_wake = event_loop.wakeup();
  }
  // Expired capabilities must remain safe to call without retaining a loop or
  // touching a destroyed session. The wake API deliberately exposes no mailbox.
  auto retired_copy = retired_wake;
  retired_wake.notify();
  retired_copy.notify();
}

}  // namespace

// Public owner contracts also cover inbox changes used by the event loop.
// Run the direct inbox and dispatcher contracts from their separate test file.
void run_tui_runtime_owner_tests();

// Run scheduler, readiness and owner regressions without decoding terminal input.
void run_tui_timeout_handler_tests()
{
  run_tui_runtime_owner_tests();
  test_timer_deadline_replacement_and_late_repeat();
  test_timer_mutation_during_dispatch();
  test_timer_move_and_destruction();
  test_spinner_preserves_phase_and_skips_missed_ticks();
  test_notification_before_wait_and_simultaneous_input();
  test_hangup_preserves_final_input();
  test_next_deadline();
  test_timer_creation_on_other_threads();
  test_expire_returns_next_deadline();
  test_arming_on_other_threads_wakes_the_loop();
  test_retired_wakeup();
}
