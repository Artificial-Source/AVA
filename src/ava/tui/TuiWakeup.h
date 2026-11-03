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

  // The weak control block is an opaque concurrent lifetime resource, without a
  // generated streaming contract. Do not lock/inspect it for debug printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  friend class TuiEventLoop;
  explicit TuiWakeup(std::shared_ptr<TuiWaitState> const& state) noexcept;

  std::weak_ptr<TuiWaitState> state_;
};

} // namespace ava::tui
