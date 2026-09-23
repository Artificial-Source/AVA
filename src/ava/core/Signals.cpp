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

Signals::~Signals()
{
  DoutEntering(dc::notice, "core::Signals::~Signals()");

  // Ordinary teardown leaves AVA's foreground signals blocked and ignored.
  utils::Signal::block_and_unregister(SIGINT);
  utils::Signal::block_and_unregister(SIGTERM);
  // No recorded foreground signal may remain observable after teardown.
  s_received_ = 0;
}

void Signals::activate_handlers()
{
  // This thread receives SIGINT and SIGTERM signals.
  utils::Signal::unblock(SIGINT);
  utils::Signal::unblock(SIGTERM);
}

} // namespace ava::core
