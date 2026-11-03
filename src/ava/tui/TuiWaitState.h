#pragma once

#include "ava/debug/print_members_on.h"
#include "utils/threading/EventLoop.h"

#include <atomic>
#include <chrono>
#include <optional>

namespace ava::tui {

// TuiEventLoop owns these OS wait resources and is their sole waiter. Producers
// briefly lock a weak TuiWakeup while notifying. That shared pin can keep this
// object alive after the loop and allow its destruction on a producer thread.
// It protects only wait resources: owners still join producers before destroying
// other objects they borrow. The input descriptor is borrowed until the loop ends.
class TuiWaitState final
{
 public:
  using Clock = std::chrono::steady_clock;

  explicit TuiWaitState(int input_fd);
  // Any thread, after publishing its payload. The caller must retain this state
  // throughout notify; TuiWakeup supplies that pin without retaining any UI owner.
  void notify() noexcept;
  void wait(std::optional<Clock::time_point> deadline);
  [[nodiscard]] bool terminal_hangup() const noexcept { return terminal_hangup_; }
  [[nodiscard]] bool terminal_error() const noexcept { return terminal_error_; }
  [[nodiscard]] bool notification_failed() const noexcept;

  // utils::threading::EventLoop has no printer for its platform wait resources.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  utils::threading::EventLoop waiter_;
  utils::threading::EventLoop::Event terminal_event_;
  utils::threading::EventLoop::Event application_event_;
  std::atomic_bool notification_failed_{false};
  bool terminal_hangup_ = false;
  bool terminal_error_ = false;
};

} // namespace ava::tui
