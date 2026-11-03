#pragma once

#include "ava/debug/print_members_on.h"

#include <memory>

namespace ava::tui {

class TuiEventLoop;
class TuiWaitState;

// Copyable producer capability. A successful weak lock pins only actual wait
// resources for notify; it never retains a session, timer target or projection.
class TuiWakeup final
{
 public:
  TuiWakeup() = default;
  void notify() const noexcept;

  // Printing reports only weak lifetime metadata; it never locks or prints the waiter.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  friend class TuiEventLoop;
  explicit TuiWakeup(std::shared_ptr<TuiWaitState> const& state) noexcept;

  std::weak_ptr<TuiWaitState> state_;
};

} // namespace ava::tui
