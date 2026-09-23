#include "sys.h"
#include "utils/Signals.h"
#include "ava/core/Signals.h"

#include <csignal>
#include <memory>

namespace ava::core {

std::atomic<Signals::mask_type> Signals::s_received_{0};

//static
void Signals::signal_handler(int signal_number) noexcept
{
  mask_type const mask = signal_number == SIGINT ? bit_SIGINT : bit_SIGTERM;
  s_received_.fetch_or(mask, std::memory_order_relaxed);
}

// Reserve AVA's foreground signals while construction is still single-threaded.
// Registering the AVA callback here validates and records it before application workers can exist.
Signals::Signals(utils::Badge<Application>) : signals_({SIGINT, SIGTERM})
{
  DoutEntering(dc::notice, "core::Signals::Signals()");

  // At this point all signals are blocked. Registering our handler does not change that.
  signals_.register_callback(SIGINT, signal_handler);
  signals_.register_callback(SIGTERM, signal_handler);
}

void Signals::activate_handlers()
{
  // This thread receives SIGINT and SIGTERM signals.
  utils::Signal::unblock(SIGINT);
  utils::Signal::unblock(SIGTERM);
}

//static
bool Signals::reset_child_signal_state() noexcept
{
  struct sigaction action{};
  action.sa_handler = SIG_DFL;
  ::sigemptyset(&action.sa_mask);
  for (int const signal_number : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT})
    if (::sigaction(signal_number, &action, nullptr) != 0)
      return false;

  sigset_t empty;
  ::sigemptyset(&empty);
  return ::sigprocmask(SIG_SETMASK, &empty, nullptr) == 0;
}

} // namespace ava::core
