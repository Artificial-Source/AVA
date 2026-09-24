#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/command_line.h"
#include "ava/core/Signals.h"

#include <atomic>

namespace ava::app {

// Identify the resolved operation mode whose process signal policy is selected.
enum class InvocationMode
{
  Acp,
  Rpc,
  Print,
  Connect,
  Doctor,
  SupportExport,
  ImmediateOutput,
  Interactive,
  LineShell,
};

// Describe whether dispatch activates shared bits, defaults, or frontend-owned handling.
enum class InvocationSignalPolicy
{
  SharedBits,
  Default,
  Deferred,
};

[[nodiscard]] constexpr InvocationSignalPolicy signal_policy_for(InvocationMode mode)
{
  switch (mode)
  {
    case InvocationMode::Acp:
    case InvocationMode::Rpc:
    case InvocationMode::Print:
    case InvocationMode::Connect:
    case InvocationMode::Doctor:
    case InvocationMode::SupportExport:
      return InvocationSignalPolicy::SharedBits;
    case InvocationMode::ImmediateOutput:
      return InvocationSignalPolicy::Default;
    case InvocationMode::Interactive:
    case InvocationMode::LineShell:
      return InvocationSignalPolicy::Deferred;
  }
  return InvocationSignalPolicy::Deferred;
}

// Return the signal-policy mode selected by the fully parsed invocation.
[[nodiscard]] InvocationMode invocation_mode(CommandLineInvocation const& invocation);

// Apply the process signal policy for mode after parsing has resolved the complete invocation.
//
// Deferred modes retain ownership of activation in their existing frontend.
// Call this exactly once during process dispatch.
void apply_invocation_signal_policy(InvocationMode mode);

// Latch the first pending termination signal claimed by one operation mode.
//
// poll() checks the process bit mask before atomically claiming SIGTERM,
// SIGINT, or SIGHUP. Once claimed, all later polls remain true and signum()
// returns the first claimed signal number.
class ModeSignalLatch final
{
 public:
  [[nodiscard]] bool poll() noexcept;
  [[nodiscard]] bool signaled() const noexcept { return signum_.load(std::memory_order_relaxed) != 0; }
  [[nodiscard]] int signum() const noexcept { return signum_.load(std::memory_order_relaxed); }
  void reset() noexcept { signum_.store(0, std::memory_order_relaxed); }

 private:
  std::atomic_int signum_ = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::app
