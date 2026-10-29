#pragma once

#include "ava/plugin/runner.h"
#include "ava/core/result.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ava::plugin {

[[nodiscard]] ava::core::Error plugin_error(ava::core::ErrorCategory category, std::string message, PluginManifest const& manifest);
[[nodiscard]] ava::core::Error protocol_error(std::string message, PluginManifest const& manifest);
[[nodiscard]] bool is_canceled(CancelCallback const& cancel_requested) noexcept;
[[nodiscard]] ava::core::Error canceled_error(std::string message, PluginManifest const& manifest);
[[nodiscard]] std::string json_string(std::string_view value);
[[nodiscard]] std::string exit_detail(ava::process::ExitStatusV1 const& status);
[[nodiscard]] std::vector<std::string> plugin_argv(PluginManifest const& manifest);
[[nodiscard]] std::filesystem::path child_working_dir(PluginManifest const& manifest, PluginRunnerOptions const& options);

}  // namespace ava::plugin
