#pragma once

#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>

namespace ava::tui {

class TuiEventLoop;

// TUI-thread registration handle. Its loop and stable callback target outlive
// the handle; destruction removes the registration, while cancel only disarms it.
class TuiTimer final
{
 public:
  using Clock = std::chrono::steady_clock;

  TuiTimer() = default;
  ~TuiTimer();
  TuiTimer(TuiTimer const&) = delete;
  TuiTimer& operator=(TuiTimer const&) = delete;
  TuiTimer(TuiTimer&& other) noexcept;
  TuiTimer& operator=(TuiTimer&& other) noexcept;

  void cancel() noexcept;
  [[nodiscard]] bool active() const noexcept;
  void arm_at(Clock::time_point deadline);
  void arm_after(Clock::duration delay, Clock::time_point now = Clock::now());
  void repeat(Clock::duration interval, Clock::time_point now = Clock::now());

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  friend class TuiEventLoop;
  TuiTimer(TuiEventLoop& loop, std::uint64_t id) noexcept;
  void release() noexcept;

  TuiEventLoop* loop_ = nullptr;
  std::uint64_t id_ = 0;
};

} // namespace ava::tui
