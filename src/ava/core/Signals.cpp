#include "sys.h"
#include "utils/Signals.h"
#include "ava/core/Signals.h"

#include <cerrno>
#include <csignal>

#include "debug.h"

#ifdef CWDEBUG
namespace debug::ostream_operators {

std::ostream& operator<<(std::ostream& os, std::initializer_list<int> const& signums)
{
  os << '{';
  char const* separator = "";
  for (int signum : signums)
  {
    os << separator << utils::signum_to_str(signum);
    separator = ", ";
  }
  os << '}';
  return os;
}

} // namespace debug::ostream_operators
#endif

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
Signals::Signals(utils::Badge<Application>) : signals_({SIGHUP, SIGINT, SIGPIPE, SIGTERM})
{
  DoutEntering(dc::signals, "core::Signals::Signals()");
}

void Signals::activate_handlers(std::initializer_list<int> signums)
{
  DoutEntering(dc::signals, "Signals::activate_handlers(" << signums << ")");

  for (int const signal_number : signums)
    utils::Signal::unblock(signal_number, signal_handler);
}

void Signals::default_handlers(std::initializer_list<int> signums)
{
  DoutEntering(dc::signals, "Signals::default_handlers(" << signums << ")");

  for (int const signal_number : signums)
    signals_.default_handler(signal_number);
}

//static
bool Signals::reset_child_signal_state() noexcept
{
  DoutEntering(dc::signals, "Signals::reset_child_signal_state()");

  struct sigaction action{};
  action.sa_handler = SIG_DFL;
  ::sigemptyset(&action.sa_mask);
  for (int signal_number = 1; signal_number < NSIG; ++signal_number)
    if (::sigaction(signal_number, &action, nullptr) != 0 && errno != EINVAL)
      return false;

  // Unblock all signals.
  utils::SignalSet empty;
  return empty.setmask();
}

} // namespace ava::core
