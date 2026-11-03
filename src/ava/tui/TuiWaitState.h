#pragma once

#include "ava/debug/print_members_on.h"
#include "utils/threading/EventLoop.h"

#include <atomic>
#include <chrono>
#include <optional>

namespace ava::tui {

// Shared only to pin OS waiter resources during an in-flight producer notify.
// The TUI owner is the sole waiter; it disables admission before releasing its
// share. The watched input descriptor is borrowed and must outlive the owner.
class TuiWaitState final
{
 public:
  using Clock = std::chrono::steady_clock;

  explicit TuiWaitState(int input_fd);
  void notify() noexcept;
  void disable() noexcept;
  void wait(std::optional<Clock::time_point> deadline);
  [[nodiscard]] bool terminal_hangup() const noexcept { return terminal_hangup_; }
  [[nodiscard]] bool terminal_error() const noexcept { return terminal_error_; }
  [[nodiscard]] bool notification_failed() const noexcept;

  // Owns opaque platform waiter resources and concurrently mutated atomics;
  // generated traversal has no safe streaming contract for these internals.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  utils::threading::EventLoop waiter_;
  utils::threading::EventLoop::Event terminal_event_;
  utils::threading::EventLoop::Event application_event_;
  std::atomic_bool enabled_{true};
  std::atomic_bool notification_failed_{false};
  bool terminal_hangup_ = false;
  bool terminal_error_ = false;
};

} // namespace ava::tui
