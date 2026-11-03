// Deadline state for the transcript's temporary scroll-position indicator.
#pragma once

#include "ava/debug/print_members_on.h"

#include <chrono>
#include <optional>

namespace ava::tui {

// TranscriptView owns this TUI-thread value. The renderer schedules its deadline;
// this value has no callback or borrowed lifetime.
class TranscriptPositionIndicatorState final
{
 public:
  // A monotonic clock keeps wall-clock changes from affecting indicator duration.
  using Clock = std::chrono::steady_clock;
  // Preserve the one-second display period after the most recent navigation action.
  static constexpr auto kVisibleDuration = std::chrono::milliseconds(1000);
  // TUI thread: renew visibility from this navigation action, before arming its timer.
  void show(Clock::time_point now = Clock::now()) noexcept;
  // TUI thread: retire visibility when navigation/session reset removes the indicator.
  void hide() noexcept;
  // TUI thread: reports published intent; expiry is applied by expire_if_due.
  [[nodiscard]] bool visible() const noexcept;
  // TUI thread: retire a reached deadline and report whether a redraw is needed.
  [[nodiscard]] bool expire_if_due(Clock::time_point now = Clock::now()) noexcept;
  // TUI thread: copy the deadline when synchronizing the renderer's timer.
  [[nodiscard]] std::optional<Clock::time_point> expires_at() const noexcept;
  AVA_DEBUG_PRINT_MEMBERS_ON
 private:
  // Last show() sets this; hide()/expiry clear it. Accessed only on the TUI thread.
  std::optional<Clock::time_point> expires_at_ = std::nullopt;
};

} // namespace ava::tui
