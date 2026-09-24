#include "sys.h"
#include "utils/Signals.h"
#include "ava/core/Signals.h"

#include <cerrno>
#include <csignal>

namespace ava::core {

std::atomic<Signals::mask_type> Signals::s_received_{0};

//static
void Signals::signal_handler(int signal_number) noexcept
{
  mask_type const mask = to_mask(signal_number);
  s_received_.fetch_or(mask, std::memory_order_relaxed);
}

// Reserve AVA's process signals while construction is still single-threaded.
// SIGPIPE remains blocked and ignored; command parsing later selects dispositions for the foreground control signals.
Signals::Signals(utils::Badge<Application>) : signals_({SIGINT, SIGTERM, SIGPIPE, SIGHUP})
{
  DoutEntering(dc::notice, "core::Signals::Signals()");
}

void Signals::activate_handlers(std::initializer_list<int> signums)
{
  for (int const signal_number : signums)
    utils::Signal::unblock(signal_number, signal_handler);
}

void Signals::default_handlers(std::initializer_list<int> signums)
{
  for (int const signal_number : signums)
    signals_.default_handler(signal_number);
}

//static
bool Signals::reset_child_signal_state() noexcept
{
  struct sigaction action{};
  action.sa_handler = SIG_DFL;
  ::sigemptyset(&action.sa_mask);
  for (int signal_number = 1; signal_number < NSIG; ++signal_number)
    if (::sigaction(signal_number, &action, nullptr) != 0 && errno != EINVAL)
      return false;

  sigset_t empty;
  ::sigemptyset(&empty);
  return ::sigprocmask(SIG_SETMASK, &empty, nullptr) == 0;
}

} // namespace ava::core
