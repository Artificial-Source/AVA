#pragma once

#include "utils/Badge.h"
#include "utils/Signals.h"
#include "ava/debug/print_members_on.h"

#include <atomic>

namespace ava::core {

class Application;

// Own AVA's foreground-signal reservation and atomic pending-signal bits.
//
// Constructed as member of Application before worker threads are created.
// Construction blocks the reserved signals and deliberately leaves them ignored.
// Process orchestration later selects either shared bit handlers or default dispositions for foreground control signals.
// SIGPIPE remains ignored and blocked for the entire Application lifetime, while interactive TUI activation remains deferred until ncurses is ready.
//
// try_obtain() atomically claims and clears the selected bit, but note
// that repeated occurrences of the same signal may coalesce; these bits are
// not counters.
//
class Signals final
{
 public:
  using mask_type = uint32_t;
  static_assert(std::atomic<mask_type>::is_always_lock_free, "signal handlers require a lock-free pending-bit atomic");
  static constexpr mask_type to_mask(int signum) { return mask_type{1} << (signum - 1); }

 private:
  utils::Signals signals_;
  static std::atomic<mask_type> s_received_;

  static void signal_handler(int signal_number) noexcept;       // Sets one bit in s_received_.

 public:
  Signals(utils::Badge<Application>);

  // Install the shared bit callback and unblock the mode-selected signals.
  // TUI calls this only after ncurses initialization; process orchestration owns all other calls.
  void activate_handlers(std::initializer_list<int> signums);
  void default_handlers(std::initializer_list<int> signums);

  // Returns WasTrue if any of the signals in `signals` are pending.
  static bool received(mask_type signals) { return (s_received_.load(std::memory_order::relaxed) & signals) != 0; }

  // May also be used to clear more than one signal, but then the return value should be ignored.
  // Otherwise, return true if the signal was successfully claimed for this thread to be handled.
  static bool try_obtain(mask_type signal)
  {
    mask_type prev = s_received_.fetch_and(~signal, std::memory_order_relaxed);
    return (prev & signal) != 0;
  }

  static bool try_obtain(int signum) { return try_obtain(to_mask(signum)); }

  // Reset all catchable signals to their default dispositions, then unblock all signals.
  // Call only in the forked child before exec; do not resume application execution afterward.
  // If exec fails, terminate the child with _exit().
  static bool reset_child_signal_state() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::core
