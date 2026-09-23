#pragma once

#include "ava/debug/print_members_on.h"
#include "utils/Signals.h"
#include "utils/Badge.h"
#include <atomic>

namespace ava::core {

class Application;

// Own AVA's SIGINT/SIGTERM registration and atomic pending-signal bits.
//
// Constructed as member of Application before worker threads are created.
// Construction blocks both signals and installs a handler that records their
// receipt in s_received_.
//
// clear_signal() atomically claims and clears the selected bit, but note
// that repeated occurrences of the same signal may coalesce; these bits are
// not counters.
//
class Signals final
{
 public:
  using mask_type = uint32_t;

  static constexpr mask_type bit_SIGINT = 1;
  static constexpr mask_type bit_SIGTERM = 2;

 private:
  utils::Signals signals_;
  static std::atomic<mask_type> s_received_;

  static void signal_handler(int signal_number) noexcept;       // Sets one bit in s_received_.

 public:
  Signals(utils::Badge<Application>);

  // Must be called, after ncurses initialization, to unblock the signals.
  void activate_handlers();

  // Returns WasTrue if any of the signals in `signals` are pending.
  static bool received(mask_type signals)
  {
    return (s_received_.load(std::memory_order::relaxed) & signals) != 0;
  }

  // May also be used to clear more than one signal, but then the return value should be ignored.
  // Otherwise, return true if the signal was successfully claimed for this thread to be handled.
  static bool try_obtain(mask_type signal)
  {
    mask_type prev = s_received_.fetch_and(~signal, std::memory_order_relaxed);
    return (prev & signal) != 0;
  }

  // Reset of SIGPIPE, SIGINT, SIGTERM, SIGHUP and SIGQUIT to their default dispositions, then unblock all signals.
  // Call only in the forked child before exec; do not resume application execution afterward.
  // If exec fails, terminate the child with _exit().
  static bool reset_child_signal_state() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::core
