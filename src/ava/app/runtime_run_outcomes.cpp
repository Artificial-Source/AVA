#include "sys.h"
#include "runtime_run_outcomes.h"
#include "session_run_controller.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/core/error.h"
#include "ava/core/runtime_outcome.h"

#include <optional>
#include <string>

namespace ava::app {

bool is_agent_loop_canceled_error(ava::core::Error const& error)
{
  return error.code() == ava::core::ErrorCode::Canceled;
}

StopReason outcome_reason_for_error(ava::core::Error const& error)
{
  if (is_agent_loop_canceled_error(error))
    return StopReason::UserCanceled;
  if (error.code() == ava::core::ErrorCode::ProviderEventLimit)
    return StopReason::MaxTurns;
  if (error.category() == ava::core::ErrorCategory::Tool)
    return StopReason::ToolError;
  if (error.category() == ava::core::ErrorCategory::Session || error.category() == ava::core::ErrorCategory::Io)
    return StopReason::PersistenceError;
  return StopReason::ProviderError;
}

std::optional<ava::diagnostics::RuntimeFailureClass> diagnostic_failure_class(ava::core::Error const& error) noexcept
{
  if (is_agent_loop_canceled_error(error))
    return std::nullopt;
  switch (error.category())
  {
    case ava::core::ErrorCategory::Configuration:
      return ava::diagnostics::RuntimeFailureClass::Configuration;
    case ava::core::ErrorCategory::Provider:
      return ava::diagnostics::RuntimeFailureClass::Provider;
    case ava::core::ErrorCategory::Session:
    case ava::core::ErrorCategory::Io:
      return ava::diagnostics::RuntimeFailureClass::Session;
    case ava::core::ErrorCategory::Tool:
      return ava::diagnostics::RuntimeFailureClass::Tool;
    case ava::core::ErrorCategory::Unknown:
      return ava::diagnostics::RuntimeFailureClass::Runtime;
    case ava::core::ErrorCategory::InvalidArgument:
    case ava::core::ErrorCategory::NotFound:
    case ava::core::ErrorCategory::PermissionDenied:
      return std::nullopt;
  }
  return std::nullopt;
}

ava::core::RuntimeTerminalOutcome runtime_outcome_for_stop_reason(StopReason reason) noexcept
{
  switch (reason)
  {
    case StopReason::Completed:
      return ava::core::RuntimeTerminalOutcome::Completed;
    case StopReason::UserCanceled:
      return ava::core::RuntimeTerminalOutcome::Cancelled;
    case StopReason::MaxTurns:
    case StopReason::MaxToolCalls:
    case StopReason::NoProgress:
      return ava::core::RuntimeTerminalOutcome::MaxTurnRequests;
    case StopReason::Deadline:
    case StopReason::ProviderError:
    case StopReason::ToolError:
    case StopReason::PersistenceError:
      return ava::core::RuntimeTerminalOutcome::Error;
  }
  return ava::core::RuntimeTerminalOutcome::Error;
}

StopReason stop_reason_for_runtime_outcome(ava::core::RuntimeTerminalOutcome outcome) noexcept
{
  switch (outcome)
  {
    case ava::core::RuntimeTerminalOutcome::Completed:
    case ava::core::RuntimeTerminalOutcome::MaxTokens:
    case ava::core::RuntimeTerminalOutcome::Refusal:
      return StopReason::Completed;
    case ava::core::RuntimeTerminalOutcome::MaxTurnRequests:
      return StopReason::MaxTurns;
    case ava::core::RuntimeTerminalOutcome::Cancelled:
      return StopReason::UserCanceled;
    case ava::core::RuntimeTerminalOutcome::Error:
      return StopReason::ProviderError;
  }
  return StopReason::ProviderError;
}

}  // namespace ava::app
