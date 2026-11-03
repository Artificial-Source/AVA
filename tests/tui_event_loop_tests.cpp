#include "sys.h"
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
using ava::tui::TuiWaitMode;

class EventLoopPipe final
{
 public:
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

  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] int input() const { return read_.get(); }
  [[nodiscard]] bool publish() const { return ::write(write_.get(), "x", 1) == 1; }
  [[nodiscard]] bool consume() const
  {
    char value = 0;
    return ::read(read_.get(), &value, 1) == 1 && value == 'x';
  }
  void close_writer() { write_.reset(); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  ava::core::UniqueFd read_;
  ava::core::UniqueFd write_;
  bool valid_ = false;
};

class TimerRecorder final
{
 public:
  void tick(Clock::time_point now) { ticks_.push_back(now); }
  [[nodiscard]] std::vector<Clock::time_point> const& ticks() const { return ticks_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::vector<Clock::time_point> ticks_;
};

class SpinnerFrames final
{
 public:
  void publish(std::size_t frame) { frames_.push_back(frame); }
  [[nodiscard]] std::vector<std::size_t> const& frames() const { return frames_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::vector<std::size_t> frames_;
};

enum class PeerTimerAction
{
  Cancel,
  Rearm,
  Destroy
};

// The pair owns its handles and remains at one address until they are retired.
// Either callback may run first; it invalidates the other captured generation.
class MutatingTimerPair final
{
 public:
  MutatingTimerPair(TuiEventLoop& loop, PeerTimerAction action)
      : action_(action), first_(loop.member_timer(*this, &MutatingTimerPair::first_tick)), second_(loop.member_timer(*this, &MutatingTimerPair::second_tick))
  {
  }
  MutatingTimerPair(MutatingTimerPair const&) = delete;
  MutatingTimerPair& operator=(MutatingTimerPair const&) = delete;

  void arm(Clock::time_point when)
  {
    first_->arm_at(when);
    second_->arm_at(when);
  }
  [[nodiscard]] std::size_t calls() const { return calls_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void first_tick(Clock::time_point now) { mutate(second_, now); }
  void second_tick(Clock::time_point now) { mutate(first_, now); }
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

  PeerTimerAction action_;
  std::size_t calls_ = 0;
  std::optional<TuiTimer> first_;
  std::optional<TuiTimer> second_;
};

class RearmingTimer final
{
 public:
  explicit RearmingTimer(TuiEventLoop& loop) : timer_(loop.member_timer(*this, &RearmingTimer::tick)) { }
  RearmingTimer(RearmingTimer const&) = delete;
  RearmingTimer& operator=(RearmingTimer const&) = delete;

  void start(Clock::time_point now) { timer_.repeat(10ms, now); }
  [[nodiscard]] std::size_t calls() const { return calls_; }
  [[nodiscard]] bool active() const { return timer_.active(); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void tick(Clock::time_point now)
  {
    ++calls_;
    if (calls_ == 1)
      timer_.arm_at(now);
  }

  std::size_t calls_ = 0;
  TuiTimer timer_;
};

void test_timer_deadline_replacement_and_late_repeat()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer test input pipe"))
    return;
  TuiEventLoop loop(pipe.input());
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

void test_timer_mutation_during_dispatch()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer mutation input pipe"))
    return;
  TuiEventLoop loop(pipe.input());
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

void test_timer_move_and_destruction()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create timer ownership input pipe"))
    return;
  TuiEventLoop loop(pipe.input());
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

void test_spinner_preserves_phase_and_skips_missed_ticks()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create spinner test input pipe"))
    return;
  TuiEventLoop loop(pipe.input());
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

void test_notification_before_wait_and_simultaneous_input()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create notification test input pipe"))
    return;
  TuiEventLoop loop(pipe.input());
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
  loop.wait(TuiWaitMode::Block);
  loop.service_due();
  if (!ava::tests::require(fallback.ticks().empty() && !loop.notification_failed(),
                           "notifications published before wait wake it without reaching the fallback deadline"))
    return;

  // No input is readable. If the first wait left duplicate wake hints behind,
  // this wait would return before the armed deadline becomes due.
  loop.wait(TuiWaitMode::Block);
  loop.service_due();
  if (!ava::tests::require(fallback.ticks().size() == 1, "repeated notifications coalesce and are consumed by one wait"))
    return;

  if (!ava::tests::require(pipe.publish(), "publish a final byte alongside a fresh notification"))
    return;
  deadline.arm_at(ava::tests::now_plus_seconds(2));
  copied_wake.notify();
  loop.wait(TuiWaitMode::Block);
  loop.service_due();
  if (!ava::tests::require(fallback.ticks().size() == 1 && pipe.consume(),
                           "simultaneous input and a fresh wake return together without consuming the input byte"))
    return;
  loop.wait(TuiWaitMode::Block);
  loop.service_due();
  expect(fallback.ticks().size() == 2 && !loop.notification_failed(), "simultaneous fd readiness does not leave its notification pending for another wait");
}

void test_hangup_preserves_final_input()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create hangup test input pipe"))
    return;
  TuiEventLoop loop(pipe.input());
  if (!ava::tests::require(pipe.publish(), "publish input before terminal hangup"))
    return;
  pipe.close_writer();
  loop.wait(TuiWaitMode::ReadyOnly);
  expect(loop.terminal_hangup() && !loop.terminal_error() && pipe.consume(), "terminal hangup preserves the final readable byte");
  loop.wait(TuiWaitMode::ReadyOnly);
  expect(loop.terminal_hangup() && !loop.terminal_error(), "terminal hangup stays observable after final input is drained");
}

void test_local_stop_and_retired_wakeup()
{
  EventLoopPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create stop test input pipe"))
    return;
  ava::tui::TuiWakeup retired_wake;
  retired_wake.notify();
  {
    TuiEventLoop loop(pipe.input());
    retired_wake = loop.wakeup();
    TimerRecorder fallback;
    auto deadline = loop.member_timer(fallback, &TimerRecorder::tick);
    deadline.arm_at(ava::tests::now_plus_seconds(2));
    loop.request_stop();
    loop.wait(TuiWaitMode::Block);
    expect(loop.stopped() && !loop.has_due_work(), "local stop returns from wait without waiting for a timer or utils termination acknowledgement");
  }
  // Expired capabilities must remain safe to call without retaining a loop or
  // touching a destroyed session. The wake API deliberately exposes no mailbox.
  auto retired_copy = retired_wake;
  retired_wake.notify();
  retired_copy.notify();
}

}  // namespace

void run_tui_event_loop_tests()
{
  test_timer_deadline_replacement_and_late_repeat();
  test_timer_mutation_during_dispatch();
  test_timer_move_and_destruction();
  test_spinner_preserves_phase_and_skips_missed_ticks();
  test_notification_before_wait_and_simultaneous_input();
  test_hangup_preserves_final_input();
  test_local_stop_and_retired_wakeup();
}
