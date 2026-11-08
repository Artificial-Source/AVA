// Exercise OS readiness and timer ownership without a terminal decoder or provider.
#include "sys.h"
#include "tests/support/TuiEventLoopFixture.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/tui/Spinner.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/TuiTimer.h"
#include "ava/tui/composer_internal.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using ava::tui::TuiEventLoop;
using ava::tui::TuiTimer;
using Fixture = tui_test_support::TuiEventLoopFixture;

// Test-thread RAII pipe for readiness and hangup cases; the loop borrows its read end.
class EventLoopPipe final
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
class TimerRecorder final
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
class SpinnerFrames final
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
  Cancel, // Keep the peer registration but disarm its captured deadline.
  Rearm, // Replace the peer generation so the captured pass must skip it.
  Destroy // Remove the peer registration while the loop traverses its snapshot.
};

// The pair owns its handles and remains at one address until they are retired.
// Either callback may run first; it invalidates the other captured generation.
class MutatingTimerPair final
{
 public:
  // Test thread: register stable targets; the loop must outlive both optional handles.
  MutatingTimerPair(TuiEventLoop& loop, PeerTimerAction action)
      : action_(action), first_(loop.member_timer(*this, &MutatingTimerPair::first_tick)), second_(loop.member_timer(*this, &MutatingTimerPair::second_tick))
  {
  }
  // Both registrations retain this stable callback target until their handles retire.
  MutatingTimerPair(MutatingTimerPair const&) = delete;
  MutatingTimerPair& operator=(MutatingTimerPair const&) = delete;

  // Give both registrations one deadline so either dispatch order exercises invalidation.
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
  // Symmetric target avoids assuming unordered registration traversal order.
  void second_tick(Clock::time_point now) { mutate(first_, now); }
  // Timer callback on the test thread: mutate registration only, never wait or paint.
  void mutate(std::optional<TuiTimer>& peer, Clock::time_point now)
  {
    ++calls_;
    switch (action_)
    {
      case PeerTimerAction::Cancel:
        peer->cancel();
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
  std::optional<TuiTimer> first_; // Owned registration; optional permits destruction from its peer.
  std::optional<TuiTimer> second_; // Owned peer registration; loop outlives this handle.
};

// Stable test-thread callback target that replaces its own repeat with a one-shot deadline.
class RearmingTimer final
{
 public:
  // Register before arming; the borrowed loop outlives this owner and its handle.
  explicit RearmingTimer(TuiEventLoop& loop) : timer_(loop.member_timer(*this, &RearmingTimer::tick)) { }
  // The registration keeps this callback target stable throughout the self-rearm scenario.
  RearmingTimer(RearmingTimer const&) = delete;
  RearmingTimer& operator=(RearmingTimer const&) = delete;

  // Test thread: begin repeating before the callback deliberately replaces its cadence.
  void start(Clock::time_point now) { timer_.repeat(10ms, now); }
  // Test-thread inspection after dispatch; counts only callbacks actually invoked.
  [[nodiscard]] std::size_t calls() const { return calls_; }
  // Test-thread inspection proves the one-shot registration retires after firing.
  [[nodiscard]] bool active() const { return timer_.active(); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Timer callback replaces its generation; the loop must preserve that replacement.
  void tick(Clock::time_point now)
  {
    ++calls_;
    if (calls_ == 1)
      timer_.arm_at(now);
  }

  std::size_t calls_ = 0; // Test-thread callback count; observed only after dispatch.
  TuiTimer timer_; // Test-thread handle destroyed before the borrowed loop.
};

// Protect deadline replacement and skipping missed repeat ticks instead of replaying a burst.
void test_timer_deadline_replacement_and_late_repeat()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer test input pipe"))
    return;
  TuiEventLoop loop{ava::tui::TuiWakeup{}};
  TimerRecorder one_shot;
  TimerRecorder repeating;
  auto timer = loop.member_timer(one_shot, &TimerRecorder::tick);
  auto repeat = loop.member_timer(repeating, &TimerRecorder::tick);
  auto const start = Clock::time_point{};
  expect(!timer.active() && !repeat.active(), "member timer registrations begin disarmed");

  timer.arm_at(start + 10ms);
  timer.arm_after(20ms, start);
  loop.service_due(start + 19ms);
  expect(one_shot.ticks().empty(), "rearming replaces the old one-shot deadline");
  loop.service_due(start + 20ms);
  loop.service_due(start + 100ms);
  expect(one_shot.ticks() == std::vector{start + 20ms} && !timer.active(), "one-shot dispatch uses the supplied timestamp and disarms before later service");

  auto const repeat_start = start + 100ms;
  repeat.repeat(10ms, repeat_start);
  loop.service_due(repeat_start + 9ms);
  expect(repeating.ticks().empty(), "a repeating timer starts after its first full interval");
  loop.service_due(repeat_start + 35ms);
  loop.service_due(repeat_start + 35ms);
  loop.service_due(repeat_start + 39ms);
  expect(repeating.ticks() == std::vector{repeat_start + 35ms} && repeat.active(), "an overdue repeat fires once and skips missed callbacks");
  loop.service_due(repeat_start + 40ms);
  repeat.cancel();
  loop.service_due(repeat_start + 100ms);
  expect(repeating.ticks() == std::vector{repeat_start + 35ms, repeat_start + 40ms} && !repeat.active(),
         "late repeat preserves its original phase and cancellation suppresses later ticks");
}

// Protect a due-timer snapshot when another callback cancels, rearms or destroys its peer.
void test_timer_mutation_during_dispatch()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer mutation input pipe"))
    return;
  TuiEventLoop loop{ava::tui::TuiWakeup{}};
  auto due = Clock::time_point{} + 10ms;
  for (auto action : {PeerTimerAction::Cancel, PeerTimerAction::Rearm, PeerTimerAction::Destroy})
  {
    MutatingTimerPair pair(loop, action);
    pair.arm(due);
    loop.service_due(due);
    expect(pair.calls() == 1, "cancel, rearm, or destruction suppresses a peer timer's already captured due generation");
    loop.service_due(due + 9ms);
    expect(pair.calls() == 1, "a changed due registration is not replayed by the following service pass");
    loop.service_due(due + 10ms);
    expect(pair.calls() == (action == PeerTimerAction::Rearm ? 2U : 1U), "only explicit rearming admits a later peer callback");
    due += 30ms;
  }

  RearmingTimer self(loop);
  self.start(due - 10ms);
  loop.service_due(due);
  expect(self.calls() == 1 && self.active(), "self-rearming for now does not recurse in the current due batch");
  loop.service_due(due);
  expect(self.calls() == 2 && !self.active(), "callback rearming replaces the repeat and becomes eligible on the next service pass");
}

// Protect registration ownership when timer handles move or disappear before dispatch.
void test_timer_move_and_destruction()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer ownership input pipe"))
    return;
  TuiEventLoop loop{ava::tui::TuiWakeup{}};
  TimerRecorder source_target;
  TimerRecorder displaced_target;
  auto source = loop.member_timer(source_target, &TimerRecorder::tick);
  auto const due = Clock::time_point{} + 10ms;
  source.arm_at(due);
  auto moved = std::move(source);
  expect(!source.active() && moved.active(), "moving a timer transfers its armed registration");
  loop.service_due(due);
  expect(source_target.ticks().size() == 1, "a moved timer fires exactly once on its original target");

  auto destination = loop.member_timer(displaced_target, &TimerRecorder::tick);
  destination.arm_at(due + 10ms);
  moved.arm_at(due + 10ms);
  destination = std::move(moved);
  loop.service_due(due + 10ms);
  expect(!moved.active() && source_target.ticks().size() == 2 && displaced_target.ticks().empty(),
         "move assignment retires the destination registration before taking the source");
  {
    auto destroyed = loop.member_timer(displaced_target, &TimerRecorder::tick);
    destroyed.arm_at(due + 20ms);
  }
  loop.service_due(due + 20ms);
  expect(displaced_target.ticks().empty(), "destroying an armed registration prevents its callback");
}

// Protect spinner phase through stop/restart and avoid catch-up animation after a delayed turn.
void test_spinner_preserves_phase_and_skips_missed_ticks()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create spinner test input pipe"))
    return;
  TuiEventLoop loop{ava::tui::TuiWakeup{}};
  SpinnerFrames frames;
  ava::tui::Spinner spinner(loop, std::bind_front(&SpinnerFrames::publish, &frames));
  auto const before_start = Clock::now();
  spinner.start(7);
  auto const after_start = Clock::now();
  loop.service_due(before_start + 119ms);
  expect(spinner.active() && frames.frames().empty() && ava::tui::detail::kProcessingIndicatorFrameDelay == 120ms,
         "spinner preserves the starting phase and schedules its first update after 120ms");
  loop.service_due(after_start + 120ms);
  loop.service_due(after_start + 900ms);
  expect(frames.frames() == std::vector<std::size_t>{8, 9}, "late spinner servicing advances one visible phase without catch-up frames");
  spinner.stop();
  loop.service_due(after_start + 1200ms);
  expect(!spinner.active() && frames.frames() == std::vector<std::size_t>{8, 9}, "stopping the spinner retires future phase publication");
}

// Protect publication before waiting and retain both notification and input readiness in one result.
void test_notification_before_wait_and_simultaneous_input()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create notification test input pipe"))
    return;
  Fixture event_loop(pipe.input());
  TuiEventLoop& loop = event_loop.loop();
  TimerRecorder fallback;
  auto deadline = loop.member_timer(fallback, &TimerRecorder::tick);
  auto wake = loop.wakeup();
  auto copied_wake = wake;

  // A fallback deadline bounds a regressed lost notification without requiring
  // another thread or a sleep to establish that publication preceded the wait.
  deadline.arm_at(ava::tests::now_plus_seconds(2));
  wake.notify();
  copied_wake.notify();
  wake.notify();
  event_loop.wait();
  loop.service_due();
  if (!ava::tests::require(fallback.ticks().empty() && event_loop.notified(),
                           "notifications published before wait wake it without reaching the fallback deadline"))
    return;

  // No input is readable. If the first wait left duplicate wake hints behind,
  // this wait would return before the armed deadline becomes due.
  event_loop.wait();
  loop.service_due();
  if (!ava::tests::require(fallback.ticks().size() == 1 && !event_loop.notified(), "repeated notifications coalesce and are consumed by one wait"))
    return;

  if (!ava::tests::require(pipe.publish(), "publish a final byte alongside a fresh notification"))
    return;
  deadline.arm_at(ava::tests::now_plus_seconds(2));
  copied_wake.notify();
  event_loop.wait();
  loop.service_due();
  if (!ava::tests::require(fallback.ticks().size() == 1 && event_loop.notified() && pipe.consume(),
                           "simultaneous input and a fresh wake return together without consuming the input byte"))
    return;
  event_loop.wait();
  loop.service_due();
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
  TuiEventLoop loop{ava::tui::TuiWakeup{}};
  TimerRecorder first;
  TimerRecorder second;
  auto early = loop.member_timer(first, &TimerRecorder::tick);
  auto late = loop.member_timer(second, &TimerRecorder::tick);
  auto const start = Clock::time_point{} + 1s;
  expect(!loop.next_deadline(), "a registry without armed timers has no deadline");
  late.arm_at(start + 20ms);
  early.arm_at(start + 10ms);
  expect(loop.next_deadline() == start + 10ms, "the earliest armed deadline wins");
  early.cancel();
  expect(loop.next_deadline() == start + 20ms, "a canceled timer no longer constrains the wait");
  loop.service_due(start + 20ms);
  expect(!loop.next_deadline() && second.ticks().size() == 1, "a dispatched one-shot timer no longer constrains the wait");
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
    retired_wake = event_loop.loop().wakeup();
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
void run_tui_event_loop_tests()
{
  run_tui_runtime_owner_tests();
  test_timer_deadline_replacement_and_late_repeat();
  test_timer_mutation_during_dispatch();
  test_timer_move_and_destruction();
  test_spinner_preserves_phase_and_skips_missed_ticks();
  test_notification_before_wait_and_simultaneous_input();
  test_hangup_preserves_final_input();
  test_next_deadline();
  test_retired_wakeup();
}
