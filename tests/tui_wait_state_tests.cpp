// Exercise TuiWaitState's public descriptor observations and producer lifetime
// contract. Its void wait result does not expose wake reasons or notification
// coalescing; bounded waits below make no latency claim or failure-injection claim.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/tui/TuiWaitState.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace {

using ava::tui::TuiWaitState;

// Own both pipe ends on the test thread. Declare this before the waiter so its
// borrowed descriptor remains open until the wait resources have been destroyed.
class WaitStatePipe final
{
 public:
  // Acquire nonblocking descriptors so a failed readiness observation cannot
  // make the subsequent payload check block; retain ownership on setup failure.
  WaitStatePipe()
  {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe(descriptors.data()) != 0)
      return;
    read_.reset(descriptors[0]);
    write_.reset(descriptors[1]);
    valid_ = ::fcntl(read_.get(), F_SETFL, O_NONBLOCK) == 0 && ::fcntl(write_.get(), F_SETFL, O_NONBLOCK) == 0 &&
             ::fcntl(read_.get(), F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(write_.get(), F_SETFD, FD_CLOEXEC) == 0;
  }

  // Check fixture setup before constructing a waiter with either borrowed end.
  [[nodiscard]] bool valid() const { return valid_; }
  // Borrow the read end until the waiter has been destroyed.
  [[nodiscard]] int input() const { return read_.get(); }
  // Borrow the write end to observe Linux EPOLLERR when its reader disappears.
  [[nodiscard]] int output() const { return write_.get(); }
  // Publish one byte while both ends remain open, before the test calls wait.
  [[nodiscard]] bool publish() const { return ::write(write_.get(), "x", 1) == 1; }
  // Check that waiting has preserved the queued payload for its actual consumer.
  [[nodiscard]] bool consume() const
  {
    char value = 0;
    return ::read(read_.get(), &value, 1) == 1 && value == 'x';
  }
  // End input while leaving the descriptor watched by the waiter alive.
  void close_writer() { write_.reset(); }
  // Create a real descriptor error while leaving the watched write end alive.
  void close_reader() { read_.reset(); }

  // This fixture has internal linkage and needs no generated descriptor printer.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Sole ownership of the readable end; only the test thread closes it.
  ava::core::UniqueFd read_;
  // Sole ownership of the writable end; only the test thread closes it.
  ava::core::UniqueFd write_;
  // Set once by construction after both descriptors have their required flags.
  bool valid_ = false;
};

// Readiness preserves input, and peer closure latches hangup without confusing
// it with a descriptor error. The borrowed read end stays open throughout.
void test_input_readiness_and_hangup_latch()
{
  WaitStatePipe pipe;
  if (!ava::tests::require(pipe.valid(), "create wait-state input pipe"))
    return;
  TuiWaitState state(pipe.input());
  expect(!state.terminal_hangup() && !state.terminal_error() && !state.notification_failed(), "wait-state observations begin clear");

  if (!ava::tests::require(pipe.publish(), "queue input before waiting"))
    return;
  state.wait(ava::tests::now_plus_seconds(2));
  expect(pipe.consume() && !state.terminal_hangup() && !state.terminal_error(), "readable input is preserved without reporting terminal failure");

  if (!ava::tests::require(pipe.publish(), "queue final input before peer closure"))
    return;
  pipe.close_writer();
  state.wait(ava::tests::now_plus_seconds(2));
  expect(state.terminal_hangup() && !state.terminal_error() && pipe.consume(), "hangup is reported while the final input byte remains readable");
  state.notify();
  state.wait(ava::tests::now_plus_seconds(2));
  expect(state.terminal_hangup() && !state.terminal_error(), "hangup remains observable after input drainage and another wait");
}

// Linux reports EPOLLERR on a pipe's write end once its last reader closes,
// including when the registration requested only read readiness.
void test_descriptor_error_latch()
{
  WaitStatePipe pipe;
  if (!ava::tests::require(pipe.valid(), "create wait-state descriptor-error pipe"))
    return;
  TuiWaitState state(pipe.output());
  pipe.close_reader();
  state.wait(ava::tests::now_plus_seconds(2));
  expect(state.terminal_error() && !state.terminal_hangup(), "closed pipe reader produces a descriptor error rather than input hangup");
  state.notify();
  state.wait(ava::tests::now_plus_seconds(2));
  expect(state.terminal_error() && !state.terminal_hangup(), "descriptor error remains observable on a subsequent wait");
}

// Join establishes publication-before-wait without scheduler guesses. The
// payload owns its synchronization; notify carries only the wake hint.
void test_producer_publication_before_wait()
{
  WaitStatePipe pipe;
  if (!ava::tests::require(pipe.valid(), "create wait-state producer pipe"))
    return;
  TuiWaitState state(pipe.input());
  std::atomic<int> payload{0};
  std::jthread producer([&] {
    payload.store(42, std::memory_order_release);
    state.notify();
  });
  producer.join();
  state.wait(ava::tests::now_plus_seconds(2));
  expect(payload.load(std::memory_order_acquire) == 42 && !state.notification_failed(), "producer publishes its payload before a successful notification");
  expect(!state.terminal_hangup() && !state.terminal_error(), "application notification does not invent terminal failure");
}

// Reproduce the public shared/weak ownership pattern used by producer wakeups.
// This exercises an acquired pin, not an injected race inside TuiWakeup::notify.
void test_shared_pin_survives_owner_release()
{
  WaitStatePipe pipe;
  if (!ava::tests::require(pipe.valid(), "create wait-state lifetime pipe"))
    return;
  // Shared ownership lets a producer pin retain only the wait resources after
  // the original owner's share is released; the pipe outlives both shares.
  auto owner = std::make_shared<TuiWaitState>(pipe.input());
  std::weak_ptr<TuiWaitState> observer = owner;
  auto pin = observer.lock();
  if (!ava::tests::require(static_cast<bool>(pin), "acquire producer pin while owner is alive"))
    return;
  owner.reset();
  expect(!observer.expired(), "acquired producer pin keeps the wait resources alive after owner release");
  pin->notify();
  pin->wait(ava::tests::now_plus_seconds(2));
  expect(!pin->notification_failed() && !pin->terminal_hangup() && !pin->terminal_error(), "surviving pin still owns usable notification and wait resources");
  pin.reset();
  expect(observer.expired(), "releasing the final producer pin destroys the wait state");
}

} // namespace

// Keep each public-API case independently scoped so descriptor and thread
// teardown finishes before the next case constructs a fresh waiter.
void run_tui_wait_state_tests()
{
  test_input_readiness_and_hangup_latch();
  test_descriptor_error_latch();
  test_producer_publication_before_wait();
  test_shared_pin_survives_owner_release();
}
