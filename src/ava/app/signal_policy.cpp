#include "sys.h"
#include "ava/app/signal_policy.h"
#include "ava/core/Application.h"

#include <csignal>

namespace ava::app {

InvocationMode invocation_mode(CommandLineInvocation const& invocation)
{
  if (std::holds_alternative<ImmediateInvocation>(invocation))
    return InvocationMode::ImmediateOutput;
  if (std::holds_alternative<AcpInvocation>(invocation))
    return InvocationMode::Acp;
  if (std::holds_alternative<DoctorInvocation>(invocation))
    return InvocationMode::Doctor;
  if (std::holds_alternative<SupportExportInvocation>(invocation))
    return InvocationMode::SupportExport;
  if (std::holds_alternative<ConnectInvocation>(invocation))
    return InvocationMode::Connect;
  switch (std::get<RuntimeInvocation>(invocation).frontend)
  {
    case RuntimeFrontend::Print:
      return InvocationMode::Print;
    case RuntimeFrontend::Rpc:
      return InvocationMode::Rpc;
    case RuntimeFrontend::LineShell:
      return InvocationMode::LineShell;
    case RuntimeFrontend::Interactive:
      return InvocationMode::Interactive;
  }
  return InvocationMode::Interactive;
}

void apply_invocation_signal_policy(InvocationMode mode)
{
  auto& signals = ava::core::Application::instance().signals_manager();
  switch (signal_policy_for(mode))
  {
    case InvocationSignalPolicy::SharedBits:
      signals.activate_handlers({SIGTERM, SIGINT, SIGHUP});
      return;
    case InvocationSignalPolicy::Default:
      signals.default_handlers({SIGTERM, SIGINT, SIGHUP});
      return;
    case InvocationSignalPolicy::Deferred:
      return;
  }
}

bool ModeSignalLatch::poll() noexcept
{
  if (signaled())
    return true;

  constexpr auto mask = ava::core::Signals::to_mask(SIGTERM) | ava::core::Signals::to_mask(SIGINT) | ava::core::Signals::to_mask(SIGHUP);
  if (!ava::core::Signals::received(mask))
    return false;

  for (int const candidate : {SIGTERM, SIGINT, SIGHUP})
  {
    if (ava::core::Signals::try_obtain(candidate))
    {
      signum_.store(candidate, std::memory_order_relaxed);
      return true;
    }
  }
  return signaled();
}

}  // namespace ava::app
