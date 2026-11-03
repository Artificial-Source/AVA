// Gives producers a payload-free wakeup token without borrowing a TUI owner.
#pragma once

#include "ava/debug/print_members_on.h"

#include <memory>

namespace ava::tui {

class TuiEventLoop;
class TuiWaitState;

// Copyable token issued by TuiEventLoop and stored by worker/mailbox owners. It
// owns only a weak reference; notify may run concurrently on producer threads.
// Each successful lock retains just the OS waiter through that call. Tokens may
// outlive the loop and still lock while another notifier holds a share; after all
// shares expire, notify is harmless. Copy/move/destruction follow ordinary object
// synchronization rules: do not replace or destroy a token while another thread
// calls it. Token destruction never joins producers or protects their other objects.
class TuiWakeup final
{
 public:
  // Any thread: create an inert token for owners that have not attached to a loop.
  TuiWakeup() = default;
  // Any thread with this token alive: first publish payload under its mailbox's
  // synchronization, then request a coalescing wake. Empty/expired tokens do nothing;
  // a successful lock pins wait resources only and may release their last share here.
  void notify() const noexcept;

  // Printing reports only weak lifetime metadata; it never locks or prints the waiter.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Only the loop issues attached tokens, avoiding producer access to the waiter.
  friend class TuiEventLoop;
  // TUI thread while the loop's shared state is alive: borrow its control block
  // weakly, so storing the returned token cannot keep a session alive.
  explicit TuiWakeup(std::shared_ptr<TuiWaitState> const& state) noexcept;

  // Initialized at token construction/copy and only read during notify. Weak
  // ownership permits late calls without retaining the loop or callback targets.
  std::weak_ptr<TuiWaitState> state_;
};

} // namespace ava::tui
