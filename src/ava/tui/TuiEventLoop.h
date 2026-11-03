#pragma once

#include "TuiTimer.h"
#include "TuiTimerRegistration.h"
#include "TuiWakeup.h"
#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

namespace ava::tui {

class TuiWaitState;

enum class TuiWaitMode
{
  Block,
  ReadyOnly
};

// One TUI-thread waiter and timer registry. All timer handles must be destroyed
// before this owner. Stable callback targets outlive their handles. Producers
// receive only wakeup(), and must be stopped/joined before owner destruction.
class TuiEventLoop final
{
 public:
  using Clock = std::chrono::steady_clock;

  explicit TuiEventLoop(int input_fd);
  ~TuiEventLoop();
  TuiEventLoop(TuiEventLoop const&) = delete;
  TuiEventLoop& operator=(TuiEventLoop const&) = delete;
  TuiEventLoop(TuiEventLoop&&) = delete;
  TuiEventLoop& operator=(TuiEventLoop&&) = delete;

  template <typename Target>
  [[nodiscard]] TuiTimer member_timer(Target& target, void (Target::*callback)(Clock::time_point))
  {
    return register_timer(std::bind_front(callback, &target));
  }

  void service_due(Clock::time_point now = Clock::now());
  // Wait never consumes input or calls a feature. Both ready sources must be
  // serviced by composition, including ncurses-buffered input before blocking.
  void wait(TuiWaitMode mode);
  [[nodiscard]] TuiWakeup wakeup() const noexcept;
  void request_stop() noexcept { stopped_ = true; }
  [[nodiscard]] bool stopped() const noexcept { return stopped_; }
  [[nodiscard]] bool terminal_hangup() const noexcept;
  [[nodiscard]] bool terminal_error() const noexcept;
  [[nodiscard]] bool notification_failed() const noexcept;
  [[nodiscard]] bool has_due_work(Clock::time_point now = Clock::now()) const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  friend class TuiTimer;
  [[nodiscard]] TuiTimer register_timer(TuiTimerRegistration::Callback callback);
  void remove_timer(std::uint64_t id) noexcept;
  void cancel_timer(std::uint64_t id) noexcept;
  [[nodiscard]] bool timer_active(std::uint64_t id) const noexcept;
  void arm_timer_at(std::uint64_t id, Clock::time_point deadline);
  void arm_timer_after(std::uint64_t id, Clock::duration delay, Clock::time_point now);
  void repeat_timer(std::uint64_t id, Clock::duration interval, Clock::time_point now);

  // Shared ownership pins OS resources during producer notification. Releasing
  // this share does not stop an in-flight notify; the last producer pin may own
  // destruction. Once all shares expire, later weak wakeups do nothing.
  std::shared_ptr<TuiWaitState> wait_state_;
  std::map<std::uint64_t, TuiTimerRegistration> timers_;
  std::uint64_t next_timer_id_ = 1;
  bool stopped_ = false;
  bool servicing_due_ = false;
};

} // namespace ava::tui
