// Owns a real event loop, a timer registry and (optionally) a pipe for TUI tests whose ncurses input is a regular file.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/TuiWakeup.h"
#include "ava/core/UniqueFd.h"
#include "utils/threading/EventLoop.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace tui_test_support {

// Test-thread stand-in for the event loop of TuiFrontEnd::run.
//
// Owns the utils event loop (watching one input fd and one notification Event) and the TuiEventLoop
// timer registry whose wakeup() tokens notify that Event. Construct it before every feature borrowing
// loop(); destroy those features first so their timers retire before the registry. It never services
// timer callbacks itself and creates no worker.
class TuiEventLoopFixture final
{
 public:
  // How wait() may block.
  enum class Wait
  {
    Block,     // Sleep until the earliest timer deadline, input readiness or a notification.
    ReadyOnly  // Only collect what is ready now.
  };

  // Open a pipe whose read end is the watched input; keeping the write end open prevents a false hangup.
  TuiEventLoopFixture() : TuiEventLoopFixture(open_pipe()) { }
  // Watch the borrowed input_fd, which must stay valid until this fixture is destroyed.
  explicit TuiEventLoopFixture(int input_fd) : TuiEventLoopFixture(std::pair<ava::core::UniqueFd, ava::core::UniqueFd>{}, input_fd) { }

  // Test-thread borrow valid until fixture destruction; feature timers may use it.
  [[nodiscard]] ava::tui::TuiEventLoop& loop() { return loop_; }
  // Return a token that notifies the same Event as loop().wakeup().
  [[nodiscard]] ava::tui::TuiWakeup wakeup() const { return loop_.wakeup(); }

  // Wait like TuiFrontEnd::run does, but do not dispatch anything: consume readiness and notifications.
  //
  // Records hangup and error of the input fd and whether a notification arrived.
  void wait(Wait mode = Wait::Block)
  {
    using EventLoop = utils::threading::EventLoop;
    if (mode == Wait::ReadyOnly)
      event_loop_->wait_until(std::chrono::steady_clock::now());
    else if (auto const deadline = loop_.next_deadline())
      event_loop_->wait_until(*deadline);
    else
      event_loop_->wait();
    EventLoop::WaitResult const& result = event_loop_->result();
    notified_ = (result.events() & notification_event_).any();
    if ((result.events() & input_event_).any())
    {
      auto const ready = result.ready_events(EventLoop::index(input_event_));
      terminal_hangup_ = terminal_hangup_ || has_event(ready, EventLoop::ReadyEvents::hangup);
      terminal_error_ = terminal_error_ || has_event(ready, EventLoop::ReadyEvents::error);
    }
  }

  // Whether the last wait() consumed a notification.
  [[nodiscard]] bool notified() const { return notified_; }
  // Whether any wait() saw a hangup of the input fd; stays true.
  [[nodiscard]] bool terminal_hangup() const { return terminal_hangup_; }
  // Whether any wait() saw an error on the input fd; stays true.
  [[nodiscard]] bool terminal_error() const { return terminal_error_; }

  // Test-support headers are outside generated printer source discovery.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  explicit TuiEventLoopFixture(std::pair<ava::core::UniqueFd, ava::core::UniqueFd> descriptors) : TuiEventLoopFixture(std::move(descriptors), -1) { }
  TuiEventLoopFixture(std::pair<ava::core::UniqueFd, ava::core::UniqueFd> descriptors, int input_fd)
      : descriptors_(std::move(descriptors)),
        event_loop_(std::make_shared<utils::threading::EventLoop>()),
        input_event_(event_loop_->watch_fd(input_fd >= 0 ? input_fd : descriptors_.first.get(), utils::threading::EventLoop::PollEvents::read)),
        notification_event_(event_loop_->watch_event()),
        loop_(ava::tui::TuiWakeup(event_loop_, notification_event_))
  {
  }

  // Fail construction before exposing a loop with an invalid readiness descriptor.
  static std::pair<ava::core::UniqueFd, ava::core::UniqueFd> open_pipe()
  {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe(descriptors.data()) != 0)
      throw std::system_error(errno, std::generic_category(), "create TUI test event loop pipe");
    return {ava::core::UniqueFd(descriptors[0]), ava::core::UniqueFd(descriptors[1])};
  }

  // Owned test-thread descriptors, if any; destroyed after the event loop.
  std::pair<ava::core::UniqueFd, ava::core::UniqueFd> descriptors_;
  // The utils event loop; shared only so that wakeup tokens can lock it during notify.
  std::shared_ptr<utils::threading::EventLoop> event_loop_;
  utils::threading::EventLoop::Event input_event_;
  utils::threading::EventLoop::Event notification_event_;
  // Destroyed first; all borrowed feature registrations must already be gone.
  ava::tui::TuiEventLoop loop_;
  bool notified_ = false;
  bool terminal_hangup_ = false;
  bool terminal_error_ = false;
};

} // namespace tui_test_support
