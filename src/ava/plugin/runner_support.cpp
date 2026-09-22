#include "sys.h"
#include "ava/plugin/runner_support.h"
#include "ava/core/json.h"

#include <chrono>
#include <utility>

namespace ava::plugin {

ava::core::Error plugin_error(ava::core::ErrorCategory category, std::string message, PluginManifest const& manifest)
{
  static_cast<void>(manifest);
  return ava::core::Error(category, std::move(message));
}

ava::core::Error protocol_error(std::string message, PluginManifest const& manifest)
{
  return plugin_error(ava::core::ErrorCategory::Tool, std::move(message), manifest);
}

bool is_canceled(CancelCallback const& cancel_requested) noexcept
{
  if (!cancel_requested)
    return false;
  try
  {
    return cancel_requested();
  }
  catch (...)
  {
    return true;
  }
}

ava::core::Error canceled_error(std::string message, PluginManifest const& manifest)
{
  static_cast<void>(manifest);
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, std::move(message), ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  return error;
}

std::string json_string(std::string_view value)
{
  return "\"" + ava::core::json::escape(value) + "\"";
}

std::string exit_detail(ava::process::ExitStatusV1 const& status)
{
  if (status.kind == ava::process::ExitKindV1::Exited && status.has_exit_code)
    return "exit " + std::to_string(status.exit_code);
  if (status.kind == ava::process::ExitKindV1::Signaled && status.has_signal_number)
    return "signal " + std::to_string(status.signal_number);
  return std::string(ava::process::to_string(status.reason));
}

std::vector<std::string> plugin_argv(PluginManifest const& manifest)
{
  std::vector<std::string> argv;
  argv.reserve(manifest.entrypoint.args.size() + 1);
  argv.push_back(manifest.entrypoint.command);
  argv.insert(argv.end(), manifest.entrypoint.args.begin(), manifest.entrypoint.args.end());
  return argv;
}

std::filesystem::path child_working_dir(PluginManifest const& manifest, PluginRunnerOptions const& options)
{
  if (!manifest.directory.empty())
    return manifest.directory;
  if (!options.workspace_dir.empty())
    return options.workspace_dir;
  return std::filesystem::current_path();
}

std::chrono::steady_clock::time_point saturating_add(std::chrono::steady_clock::time_point value, std::chrono::steady_clock::duration duration) noexcept
{
  if (duration <= std::chrono::steady_clock::duration::zero())
    return value;
  auto const maximum = std::chrono::steady_clock::time_point::max().time_since_epoch();
  if (value.time_since_epoch() > maximum - duration)
    return std::chrono::steady_clock::time_point::max();
  return value + duration;
}

}  // namespace ava::plugin
