#pragma once

#include "ava/debug/print_members_on.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace ava::tui {

// Loop-owned scheduling state. Callbacks are copied before dispatch so removing
// this registration from its own callback cannot invalidate the active call.
class TuiTimerRegistration final
{
 public:
  using Clock = std::chrono::steady_clock;
  using Callback = std::function<void(Clock::time_point)>;

  explicit TuiTimerRegistration(Callback callback);
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept { return deadline_; }
  [[nodiscard]] bool due(Clock::time_point now) const noexcept;
  void cancel() noexcept;
  void arm_at(Clock::time_point deadline);
  void arm_after(Clock::duration delay, Clock::time_point now);
  void repeat(Clock::duration interval, Clock::time_point now);
  [[nodiscard]] Callback prepare_dispatch(Clock::time_point now);

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  Callback callback_;
  std::optional<Clock::time_point> deadline_;
  Clock::duration interval_{};
  std::uint64_t generation_ = 0;
};

} // namespace ava::tui
