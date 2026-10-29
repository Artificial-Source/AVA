#include "sys.h"
#include "ava/process/deadline.h"
#include "ava/process/environment.h"
#include "ava/plugin/runner.h"
#include "ava/plugin/runner_support.h"
#include "ava/core/error.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

namespace ava::plugin {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kPluginTerminationGrace = 100ms;
constexpr auto kPluginCleanupBudget = 2s;
constexpr auto kPluginSettlementObservationBudget = 250ms;
constexpr auto kPluginMaximumShutdownGrace = 5s;

ava::core::Error cleanup_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
}

Clock::time_point plugin_lifetime_deadline(PluginRunnerOptions const& options) noexcept
{
  auto deadline = ava::process::saturating_deadline_add(Clock::now(), options.startup_timeout);
  deadline = ava::process::saturating_deadline_add(deadline, options.request_timeout);
  deadline = ava::process::saturating_deadline_add(deadline, kPluginUiCommandDeadlineMax);
  return ava::process::saturating_deadline_add(deadline, kPluginCleanupBudget);
}

ava::core::Result<std::filesystem::path> absolute_working_directory(PluginManifest const& manifest, PluginRunnerOptions const& options)
{
  auto working_directory = child_working_dir(manifest, options);
  if (working_directory.empty())
    return std::unexpected(plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin working directory is unavailable", manifest));
  if (!working_directory.is_absolute())
  {
    std::error_code error;
    working_directory = std::filesystem::absolute(working_directory, error);
    if (error || !working_directory.is_absolute())
      return std::unexpected(plugin_error(ava::core::ErrorCategory::Io, "failed to resolve the plugin working directory", manifest));
  }
  return working_directory.lexically_normal();
}

std::string plugin_executable(PluginManifest const& manifest, std::filesystem::path const& working_directory)
{
  auto const& command = manifest.entrypoint.command;
  std::filesystem::path const requested(command);
  if (command.find('/') == std::string::npos || requested.is_absolute())
    return command;
  return (working_directory / requested).lexically_normal().string();
}

}  // namespace

PluginProcess::PluginProcess(PluginManifest manifest, PluginRunnerOptions options, ava::process::ProcessScopeV1 operation_scope,
                             Clock::time_point startup_deadline)
    : manifest_(std::move(manifest)),
      options_(std::move(options)),
      startup_deadline_(startup_deadline == Clock::time_point{} ? ava::process::saturating_deadline_add(Clock::now(), options_.startup_timeout)
                                                                : startup_deadline),
      operation_scope_(std::move(operation_scope))
{
  options_.process_scope.reset();
}

PluginProcess::~PluginProcess() noexcept
{
  try
  {
    standard_input_.close();
    if (process_handle_.valid() && !settlement_)
    {
      if (settlement_error_)
      {
        // A bounded explicit wait may fail while the Supervisor still owns a
        // runnable group. Destruction gets one fresh bounded observation so a
        // sticky reporting error cannot suppress owner cleanup.
        settlement_error_.reset();
        cleanup_deadline_.reset();
        observation_deadline_.reset();
      }
      auto const cleanup_deadline = cleanup_deadline_.value_or(ava::process::saturating_deadline_add(Clock::now(), kPluginCleanupBudget));
      auto const observation_deadline =
          observation_deadline_.value_or(ava::process::saturating_deadline_add(cleanup_deadline, kPluginSettlementObservationBudget));
      static_cast<void>(settle_until(ava::process::TerminationReasonV1::OwnerShutdown, cleanup_deadline, observation_deadline));
    }
  }
  catch (...)
  {
  }
  close_endpoints();
}

ava::core::Result<std::unique_ptr<PluginProcess>> PluginProcess::start(PluginManifest manifest, PluginRunnerOptions options, CancelCallback cancel_requested)
{
  if (manifest.entrypoint.command.empty())
    return std::unexpected(plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin entrypoint command must not be empty", manifest));
  if (options.workspace_dir.empty())
  {
    std::error_code error;
    options.workspace_dir = std::filesystem::current_path(error);
    if (error || options.workspace_dir.empty())
      return std::unexpected(plugin_error(ava::core::ErrorCategory::Io, "plugin workspace directory is unavailable", manifest));
  }
  if (options.startup_timeout < 50ms || options.startup_timeout > 30s)
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin startup timeout is out of bounds", manifest);
    error.with_context("min_ms", "50");
    error.with_context("max_ms", "30000");
    return std::unexpected(std::move(error));
  }
  if (options.request_timeout < 50ms || options.request_timeout > 30s)
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin request timeout is out of bounds", manifest);
    error.with_context("min_ms", "50");
    error.with_context("max_ms", "30000");
    return std::unexpected(std::move(error));
  }
  if (options.max_record_bytes == 0 || options.max_record_bytes > kPluginRunnerMaxRecordBytes || options.max_stderr_bytes == 0 ||
      options.max_stderr_bytes > kPluginRunnerMaxStderrBytes)
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin runner byte limits are out of bounds", manifest);
    error.with_context("max_record_bytes", std::to_string(kPluginRunnerMaxRecordBytes));
    error.with_context("max_stderr_bytes", std::to_string(kPluginRunnerMaxStderrBytes));
    return std::unexpected(std::move(error));
  }
  if (!options.process_scope)
    return std::unexpected(plugin_error(ava::core::ErrorCategory::Configuration, "plugin process authority is required", manifest));
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("plugin startup canceled", manifest));

  auto operation_scope = options.process_scope->operation();
  if (!operation_scope)
  {
    auto error = plugin_error(ava::core::ErrorCategory::Configuration, "failed to derive plugin operation process authority", manifest);
    error.with_context("cause", operation_scope.error().message());
    return std::unexpected(std::move(error));
  }
  options.process_scope.reset();

  auto const startup_deadline = ava::process::saturating_deadline_add(Clock::now(), options.startup_timeout);
  auto process = std::make_unique<PluginProcess>(std::move(manifest), std::move(options), std::move(*operation_scope), startup_deadline);
  if (auto launched = process->launch(cancel_requested); !launched)
    return std::unexpected(std::move(launched.error()));
  if (auto initialized = process->initialize(cancel_requested); !initialized)
    return std::unexpected(std::move(initialized.error()));
  return process;
}

PluginManifest const& PluginProcess::manifest() const noexcept
{
  return manifest_;
}

PluginInitialization const& PluginProcess::initialization() const noexcept
{
  return initialization_;
}

std::string const& PluginProcess::stderr_tail() const noexcept
{
  return stderr_tail_;
}

bool PluginProcess::stderr_truncated() const noexcept
{
  return stderr_truncated_;
}

ava::core::VoidResult PluginProcess::launch(CancelCallback const& cancel_requested)
{
  auto working_directory = absolute_working_directory(manifest_, options_);
  if (!working_directory)
    return std::unexpected(std::move(working_directory.error()));
  auto environment = ava::process::make_plugin_environment_v1(*working_directory);
  if (!environment)
    return std::unexpected(plugin_error(ava::core::ErrorCategory::Configuration, "failed to create the plugin process environment", manifest_));
  auto arguments = plugin_argv(manifest_);
  auto executable = plugin_executable(manifest_, *working_directory);
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("plugin startup canceled", manifest_));

  auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(startup_deadline_ - Clock::now());
  if (remaining <= 0ms)
  {
    auto error = protocol_error("timed out preparing plugin startup", manifest_);
    error.with_context("timeout_ms", std::to_string(options_.startup_timeout.count()));
    return std::unexpected(std::move(error));
  }

  auto& supervisor = operation_scope_.supervisor();
  auto reservation = supervisor.reserve(
      operation_scope_.owner_prefix(), ava::process::ProcessRoleV1::Plugin,
      {.termination_grace = kPluginTerminationGrace, .startup_timeout = remaining, .execution_deadline = plugin_lifetime_deadline(options_)});
  if (!reservation)
    return std::unexpected(std::move(reservation.error()));

  auto spawned = supervisor.spawn(std::move(*reservation), {.executable = std::move(executable),
                                                            .argv = std::move(arguments),
                                                            .environment = std::move(*environment),
                                                            .cwd = working_directory->string(),
                                                            .stdin_mode = ava::process::StreamModeV1::Capture,
                                                            .stdout_mode = ava::process::StreamModeV1::Capture,
                                                            .stderr_mode = ava::process::StreamModeV1::Capture,
                                                            .cancel_requested = cancel_requested});
  if (!spawned)
  {
    auto error = std::move(spawned.error());
    bool const startup_canceled = std::ranges::any_of(error.context(), [](ava::core::ErrorContext const& context) {
      return context.key == "reason" && context.value == ava::process::to_string(ava::process::TerminationReasonV1::Canceled);
    });
    if (!startup_canceled)
      return std::unexpected(std::move(error));
    auto canceled = canceled_error("plugin startup canceled", manifest_);
    canceled.with_context("reason", std::string(ava::process::to_string(ava::process::TerminationReasonV1::Canceled)));
    return std::unexpected(std::move(canceled));
  }

  process_handle_ = std::move(spawned->handle);
  if (spawned->standard_input)
    standard_input_ = std::move(*spawned->standard_input);
  if (spawned->standard_output)
    standard_output_ = std::move(*spawned->standard_output);
  if (spawned->standard_error)
    standard_error_ = std::move(*spawned->standard_error);
  if (!standard_input_.valid() || !standard_output_.valid() || !standard_error_.valid())
  {
    auto error = protocol_error("plugin process did not provide its captured streams", manifest_);
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
  return {};
}

std::optional<ava::process::ExitStatusV1> PluginProcess::observe_settlement() const
{
  if (settlement_)
    return settlement_;
  if (!process_handle_.valid())
    return std::nullopt;
  auto observed = operation_scope_.supervisor().try_wait(process_handle_);
  if (!observed || !*observed)
    return std::nullopt;
  return **observed;
}

ava::core::VoidResult PluginProcess::settled_cleanup_result() const
{
  if (settlement_error_)
    return std::unexpected(*settlement_error_);
  if (!settlement_)
    return std::unexpected(cleanup_error("plugin process cleanup has not settled"));
  if (settlement_->cleanup != ava::process::CleanupStateV1::Complete)
    return std::unexpected(cleanup_error("plugin process cleanup is incomplete"));
  return {};
}

ava::core::VoidResult PluginProcess::settle_until(ava::process::TerminationReasonV1 reason, Clock::time_point cleanup_deadline,
                                                  Clock::time_point observation_deadline)
{
  if (settlement_ || settlement_error_)
    return settled_cleanup_result();
  if (!process_handle_.valid())
  {
    settlement_error_ = cleanup_error("plugin process cleanup handle is unavailable");
    return std::unexpected(*settlement_error_);
  }

  if (!cleanup_deadline_)
    cleanup_deadline_ = cleanup_deadline;
  if (!observation_deadline_)
    observation_deadline_ = observation_deadline;
  cleanup_deadline = *cleanup_deadline_;
  observation_deadline = *observation_deadline_;
  standard_input_.close();

  if (auto observed = observe_settlement())
    settlement_ = *observed;
  if (!settlement_ && !stop_requested_)
  {
    // The caller observation remains bounded by observation_deadline. Retain
    // Supervisor cleanup authority for one destructor retry beyond that bound
    // so a surfaced wait error cannot immediately strand a runnable child at
    // the Supervisor's hard cleanup horizon.
    auto const supervisor_cleanup_deadline = ava::process::saturating_deadline_add(observation_deadline, kPluginCleanupBudget);
    auto stopped = operation_scope_.supervisor().request_stop(process_handle_, reason, supervisor_cleanup_deadline);
    if (!stopped)
    {
      settlement_error_ = cleanup_error("failed to request plugin process cleanup");
      settlement_error_->with_context("cause", stopped.error().message());
      return std::unexpected(*settlement_error_);
    }
    stop_requested_ = true;
  }

  while (!settlement_ && Clock::now() < observation_deadline)
  {
    static_cast<void>(drain_for_cleanup(std::min(observation_deadline, ava::process::saturating_deadline_add(Clock::now(), 10ms))));
    if (auto observed = observe_settlement())
      settlement_ = *observed;
  }
  if (!settlement_)
  {
    auto observed = operation_scope_.supervisor().wait(process_handle_, observation_deadline);
    if (observed)
      settlement_ = *observed;
  }
  while (settlement_ && (standard_output_.valid() || standard_error_.valid()) && Clock::now() < observation_deadline)
    static_cast<void>(drain_for_cleanup(std::min(observation_deadline, ava::process::saturating_deadline_add(Clock::now(), 10ms))));
  static_cast<void>(drain_for_cleanup(Clock::now()));
  close_endpoints();

  if (!settlement_)
  {
    settlement_error_ = cleanup_error("plugin process cleanup did not settle before its bounded deadline");
    return std::unexpected(*settlement_error_);
  }
  return settled_cleanup_result();
}

ava::core::VoidResult PluginProcess::settle_failure(ava::process::TerminationReasonV1 reason)
{
  if (settlement_ || settlement_error_)
    return settled_cleanup_result();
  auto const cleanup_deadline = cleanup_deadline_.value_or(ava::process::saturating_deadline_add(Clock::now(), kPluginCleanupBudget));
  auto const observation_deadline = observation_deadline_.value_or(ava::process::saturating_deadline_add(cleanup_deadline, kPluginSettlementObservationBudget));
  return settle_until(reason, cleanup_deadline, observation_deadline);
}

ava::core::Error PluginProcess::fail_process(ava::process::TerminationReasonV1 reason, ava::core::Error error)
{
  auto settled = settle_failure(reason);
  if (!settled)
    return std::move(settled.error());
  return error;
}

ava::core::VoidResult PluginProcess::shutdown(std::chrono::milliseconds grace)
{
  if (grace < 0ms || grace > kPluginMaximumShutdownGrace)
    return std::unexpected(plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin shutdown grace is out of bounds", manifest_));
  if (settlement_ || settlement_error_)
    return settled_cleanup_result();

  standard_input_.close();
  auto const natural_deadline = ava::process::saturating_deadline_add(Clock::now(), grace);
  while (!settlement_ && Clock::now() < natural_deadline)
  {
    static_cast<void>(drain_for_cleanup(std::min(natural_deadline, ava::process::saturating_deadline_add(Clock::now(), 10ms))));
    if (auto observed = observe_settlement())
      settlement_ = *observed;
  }
  if (auto observed = observe_settlement())
    settlement_ = *observed;
  if (settlement_)
  {
    auto const drain_deadline = ava::process::saturating_deadline_add(Clock::now(), kPluginSettlementObservationBudget);
    while ((standard_output_.valid() || standard_error_.valid()) && Clock::now() < drain_deadline)
      static_cast<void>(drain_for_cleanup(std::min(drain_deadline, ava::process::saturating_deadline_add(Clock::now(), 10ms))));
    static_cast<void>(drain_for_cleanup(Clock::now()));
    close_endpoints();
    return settled_cleanup_result();
  }

  auto const cleanup_deadline = cleanup_deadline_.value_or(ava::process::saturating_deadline_add(Clock::now(), kPluginCleanupBudget));
  auto const observation_deadline = observation_deadline_.value_or(ava::process::saturating_deadline_add(cleanup_deadline, kPluginSettlementObservationBudget));
  return settle_until(ava::process::TerminationReasonV1::OwnerShutdown, cleanup_deadline, observation_deadline);
}

void PluginProcess::close_endpoints() noexcept
{
  standard_input_.close();
  standard_output_.close();
  standard_error_.close();
}

}  // namespace ava::plugin
