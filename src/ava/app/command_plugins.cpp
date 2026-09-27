#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_plugins.h"
#include "ava/app/command_tools.h"
#include "ava/app/plugin_ui_capability.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/tools/tool_permission.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/install.h"
#include "ava/plugin/manifest.h"
#include "ava/plugin/runner.h"
#include "ava/plugin/static_resources.h"
#include "ava/plugin/tool_broker.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

std::string plugin_display_path(std::filesystem::path const& path, runtime::session_ts const& unlocked_session)
{
  auto const current_dir = runtime::session_ts::crat(unlocked_session)->current_dir();
  return sanitize_inline_text(display_path(path, current_dir));
}

ava::plugin::PluginDiagnostics plugin_diagnostics(runtime::ExtensionResourcePolicy const& policy, runtime::session_ts const& unlocked_session)
{
  auto const workspace_dir = runtime::session_ts::crat(unlocked_session)->workspace_dir();
  return ava::plugin::collect_plugin_diagnostics(policy.plugin_discovery, policy.plugin_enablement_file, workspace_dir);
}

std::string plugin_scope_text(ava::plugin::PluginScope scope)
{
  return std::string(ava::plugin::to_string(scope));
}

std::string plugin_status_text(bool enabled)
{
  return enabled ? "enabled" : "disabled";
}

std::string plugin_capabilities_text(ava::plugin::PluginManifest const& manifest)
{
  if (manifest.capabilities.empty())
    return "none";
  std::vector<std::string> capabilities;
  capabilities.reserve(manifest.capabilities.size());
  for (auto const& capability : manifest.capabilities) capabilities.push_back(sanitize_inline_text(capability));
  return joined_strings(capabilities, ", ");
}

bool plugin_declares_ui_capability(ava::plugin::PluginManifest const& manifest)
{
  return ava::plugin::plugin_has_capability(manifest, ava::plugin::kPluginUiStatusCapability) ||
         ava::plugin::plugin_has_capability(manifest, ava::plugin::kPluginUiWidgetCapability) ||
         ava::plugin::plugin_has_capability(manifest, ava::plugin::kPluginUiSelectCapability) ||
         ava::plugin::plugin_has_capability(manifest, ava::plugin::kPluginUiConfirmCapability);
}

bool plugin_callback_canceled(ava::plugin::CancelCallback const& cancel_requested) noexcept
{
  try
  {
    return cancel_requested && cancel_requested();
  }
  catch (...)
  {
    return true;
  }
}

class ExactPluginEnablementRevocation final
{
 public:
  ExactPluginEnablementRevocation(std::filesystem::path state_file, std::filesystem::path workspace, std::string plugin_id, ava::plugin::PluginScope scope)
      : state_file_(std::move(state_file)), workspace_(std::move(workspace)), plugin_id_(std::move(plugin_id)), scope_(scope)
  {
  }

  [[nodiscard]] bool revoked(bool force = false) noexcept
  {
    std::lock_guard lock(mutex_);
    if (revoked_)
      return true;
    auto const now = std::chrono::steady_clock::now();
    if (!force && now < next_check_)
      return false;
    next_check_ = now + std::chrono::milliseconds(25);
    try
    {
      auto const enabled = ava::plugin::plugin_enabled(state_file_, workspace_, plugin_id_, scope_);
      revoked_ = !enabled || !*enabled;
    }
    catch (...)
    {
      revoked_ = true;
    }
    return revoked_;
  }

 private:
  std::mutex mutex_;
  std::filesystem::path state_file_;
  std::filesystem::path workspace_;
  std::string plugin_id_;
  ava::plugin::PluginScope scope_ = ava::plugin::PluginScope::Project;
  std::chrono::steady_clock::time_point next_check_{};
  bool revoked_ = false;
};

std::string plugin_entrypoint_text(ava::plugin::PluginEntrypoint const& entrypoint)
{
  std::string text = sanitize_inline_text(entrypoint.command);
  for (auto const& arg : entrypoint.args) text += " " + sanitize_inline_text(arg);
  return text;
}

ava::plugin::PluginCommandContribution const* find_plugin_command(ava::plugin::PluginManifest const& manifest, std::string_view command_name)
{
  for (auto const& command : manifest.contributes.commands)
  {
    if (command.name == command_name)
      return &command;
  }
  return nullptr;
}

ava::plugin::PluginResourceContribution const* find_plugin_resource(std::vector<ava::plugin::PluginResourceContribution> const& resources,
                                                                    std::string_view name)
{
  for (auto const& resource : resources)
  {
    if (resource.name == name)
      return &resource;
  }
  return nullptr;
}

ava::core::Result<std::string> read_plugin_resource(ava::plugin::PluginManifest const& manifest, ava::plugin::PluginResourceContribution const& resource)
{
  auto loaded = ava::plugin::load_plugin_static_resource(manifest, resource, ava::plugin::kPluginResourceContentMaxBytes);
  if (!loaded)
    return std::unexpected(std::move(loaded.error()));
  return std::move(loaded->content);
}

std::string format_plugin_resource_list_text(ava::plugin::PluginStatus const& status, runtime::session_ts const& unlocked_session, std::string_view label,
                                             std::vector<ava::plugin::PluginResourceContribution> const& resources)
{
  std::ostringstream output;
  output << "Plugin " << label << " for " << sanitize_inline_text(status.plugin.manifest.id) << "\n";
  if (!status.enabled)
    output << "  status: disabled\n";
  if (resources.empty())
  {
    output << "  none";
    return output.str();
  }
  for (auto const& resource : resources)
  {
    output << "  " << sanitize_inline_text(resource.name);
    if (!resource.description.empty())
      output << " - " << sanitize_inline_text(resource.description);
    output << "  " << plugin_display_path(status.plugin.manifest.directory / resource.path, unlocked_session) << "\n";
  }
  auto text = output.str();
  if (!text.empty() && text.back() == '\n')
    text.pop_back();
  return text;
}

std::string format_plugin_resource_text(ava::plugin::PluginManifest const& manifest, ava::plugin::PluginResourceContribution const& resource,
                                        std::string_view label, runtime::session_ts const& unlocked_session, std::string content)
{
  std::ostringstream output;
  output << "Plugin " << label << " " << sanitize_inline_text(manifest.id) << "/" << sanitize_inline_text(resource.name) << "\n";
  output << "  path: " << plugin_display_path(manifest.directory / resource.path, unlocked_session) << "\n\n";
  output << content;
  return output.str();
}

std::string dynamic_resource_plural(ava::plugin::PluginDynamicResourceKind kind)
{
  switch (kind)
  {
    case ava::plugin::PluginDynamicResourceKind::Prompt:
      return "prompts";
    case ava::plugin::PluginDynamicResourceKind::Skill:
      return "skills";
  }
  return "resources";
}

std::string_view dynamic_resource_capability(ava::plugin::PluginDynamicResourceKind kind)
{
  return ava::plugin::plugin_dynamic_resource_capability(kind);
}

std::string_view dynamic_resource_contribution_kind(ava::plugin::PluginDynamicResourceKind kind)
{
  switch (kind)
  {
    case ava::plugin::PluginDynamicResourceKind::Prompt:
      return "dynamic_prompt";
    case ava::plugin::PluginDynamicResourceKind::Skill:
      return "dynamic_skill";
  }
  return "dynamic_resource";
}

std::string invalid_dynamic_resource_name_text(ava::plugin::PluginDynamicResourceKind kind, std::string_view name)
{
  return "invalid dynamic " + std::string(ava::plugin::plugin_dynamic_resource_kind_name(kind)) + " name: " + sanitize_inline_text(std::string(name));
}

std::string dynamic_resource_unsupported_text(ava::plugin::PluginManifest const& manifest, ava::plugin::PluginDynamicResourceKind kind)
{
  return "plugin does not declare " + std::string(dynamic_resource_capability(kind)) + ": " + sanitize_inline_text(manifest.id);
}

struct DynamicResourceListEntry
{
  std::string plugin_id;
  ava::plugin::PluginDynamicResource resource;
};

struct DynamicResourceFailure
{
  std::string plugin_id;
  std::string message;
};

std::string format_dynamic_resource_list_text(ava::plugin::PluginDynamicResourceKind kind, std::vector<DynamicResourceListEntry> const& entries,
                                              std::vector<DynamicResourceFailure> const& failures)
{
  std::ostringstream output;
  output << "Dynamic plugin " << dynamic_resource_plural(kind) << ":\n";
  if (entries.empty() && failures.empty())
  {
    output << "  none";
    return output.str();
  }
  for (auto const& entry : entries)
  {
    output << "  " << sanitize_inline_text(entry.plugin_id) << "/" << sanitize_inline_text(entry.resource.name);
    if (!entry.resource.description.empty())
      output << " - " << sanitize_inline_text(entry.resource.description);
    output << "\n";
  }
  for (auto const& failure : failures)
  {
    output << "  " << sanitize_inline_text(failure.plugin_id) << "  error: " << sanitize_inline_text(failure.message) << "\n";
  }
  auto text = output.str();
  if (!text.empty() && text.back() == '\n')
    text.pop_back();
  return text;
}

std::string format_dynamic_resource_text(ava::plugin::PluginManifest const& manifest, ava::plugin::PluginDynamicResourceKind kind, std::string_view name,
                                         std::string content)
{
  std::ostringstream output;
  output << "Plugin dynamic " << ava::plugin::plugin_dynamic_resource_kind_name(kind) << " " << sanitize_inline_text(manifest.id) << "/"
         << sanitize_inline_text(std::string(name)) << "\n\n";
  output << content;
  return output.str();
}

ava::core::VoidResult record_dynamic_resource_result(runtime::session_ts const& unlocked_session, ava::event::RuntimeEventSink const& sink,
                                                     CommandResult& result, std::string const& call_id, ava::agent::ToolTimelineStatus status,
                                                     std::string result_summary, std::string result_content, ava::tools::ToolContext const& context)
{
  auto const permission_ids = context.permission_request_ids ? *context.permission_request_ids : std::vector<std::string>{};
  return record_tool_result(unlocked_session, sink, result, call_id, "plugin_resource", status, std::move(result_summary), std::move(result_content),
                            permission_ids);
}

ava::plugin::PluginStatus const* find_plugin_status(ava::plugin::PluginDiagnostics const& diagnostics, std::string_view plugin_id)
{
  for (auto const& status : diagnostics.plugins)
  {
    if (status.plugin.manifest.id == plugin_id)
      return &status;
  }
  return nullptr;
}

bool has_duplicate_plugin_failure(ava::plugin::PluginDiagnostics const& diagnostics, std::string_view plugin_id)
{
  auto const details = "plugin=" + std::string(plugin_id);
  return std::ranges::any_of(diagnostics.failures, [&](ava::plugin::PluginFailure const& failure) {
    return failure.message == "duplicate plugin id discovered" && failure.details == details;
  });
}

std::string plugin_not_found_text(ava::plugin::PluginDiagnostics const& diagnostics, std::string_view plugin_id)
{
  if (has_duplicate_plugin_failure(diagnostics, plugin_id))
  {
    return "plugin id is ambiguous: " + sanitize_inline_text(std::string(plugin_id)) + " (see /plugins failures)";
  }
  return "plugin not found: " + sanitize_inline_text(std::string(plugin_id));
}

bool plugin_command_error_is_canceled(ava::core::Error const& error)
{
  for (auto const& context : error.context())
  {
    if (context.key == "canceled" && context.value == "true")
      return true;
  }
  return error.message().find("canceled") != std::string::npos;
}

std::string format_plugin_list_text(ava::plugin::PluginDiagnostics const& diagnostics, runtime::session_ts const& unlocked_session)
{
  std::ostringstream output;
  output << "Plugins:\n";
  if (diagnostics.plugins.empty())
  {
    output << "  none discovered\n";
  }
  else
  {
    for (auto const& status : diagnostics.plugins)
    {
      auto const& manifest = status.plugin.manifest;
      output << "  " << sanitize_inline_text(manifest.id) << "  " << plugin_status_text(status.enabled) << "  " << plugin_scope_text(status.plugin.scope)
             << "  " << sanitize_inline_text(manifest.name) << "  " << sanitize_inline_text(manifest.version) << "\n";
    }
  }
  output << "\nDiscovery paths:\n";
  output << "  global: " << plugin_display_path(diagnostics.discovery_options.global_plugins_dir, unlocked_session) << "\n";
  output << "  project: " << plugin_display_path(diagnostics.discovery_options.project_plugins_dir, unlocked_session) << "\n";
  output << "Enablement: " << plugin_display_path(diagnostics.enablement_file, unlocked_session);
  if (!diagnostics.failures.empty())
  {
    output << "\nFailures: " << diagnostics.failures.size() << " (use /plugins failures)";
  }
  return output.str();
}

std::string format_plugin_failure_text(ava::plugin::PluginFailure const& failure, runtime::session_ts const& unlocked_session)
{
  std::string text =
      "  " + plugin_scope_text(failure.scope) + "  " + plugin_display_path(failure.path, unlocked_session) + "\n    " + sanitize_inline_text(failure.message);
  if (!failure.details.empty())
    text += "\n    " + sanitize_inline_text(failure.details);
  return text;
}

std::string format_plugin_failures_text(ava::plugin::PluginDiagnostics const& diagnostics, runtime::session_ts const& unlocked_session)
{
  if (diagnostics.failures.empty())
    return "No plugin discovery or enablement failures.";
  std::string output = "Plugin failures:\n";
  for (auto const& failure : diagnostics.failures) output += format_plugin_failure_text(failure, unlocked_session) + "\n";
  if (!output.empty() && output.back() == '\n')
    output.pop_back();
  return output;
}

std::string format_plugin_inspect_text(ava::plugin::PluginStatus const& status, runtime::session_ts const& unlocked_session)
{
  auto const& manifest = status.plugin.manifest;
  std::ostringstream output;
  output << "Plugin " << sanitize_inline_text(manifest.id) << "\n";
  output << "  name: " << sanitize_inline_text(manifest.name) << "\n";
  output << "  version: " << sanitize_inline_text(manifest.version) << "\n";
  output << "  scope: " << plugin_scope_text(status.plugin.scope) << "\n";
  output << "  status: " << plugin_status_text(status.enabled) << "\n";
  output << "  manifest: " << plugin_display_path(manifest.path, unlocked_session) << "\n";
  output << "  entrypoint: " << plugin_entrypoint_text(manifest.entrypoint) << " (not executed)\n";
  output << "  capabilities: " << plugin_capabilities_text(manifest) << "\n";
  output << "  tools: " << (manifest.contributes.tools.empty() ? "none" : std::to_string(manifest.contributes.tools.size())) << "\n";
  for (auto const& tool : manifest.contributes.tools)
  {
    output << "    " << sanitize_inline_text(tool.name);
    if (!tool.description.empty())
      output << " - " << sanitize_inline_text(tool.description);
    output << "\n";
  }
  output << "  commands: " << (manifest.contributes.commands.empty() ? "none" : std::to_string(manifest.contributes.commands.size())) << "\n";
  for (auto const& command : manifest.contributes.commands)
  {
    output << "    " << sanitize_inline_text(command.name);
    if (!command.description.empty())
      output << " - " << sanitize_inline_text(command.description);
    output << "\n";
  }
  output << "  prompts: " << (manifest.contributes.prompts.empty() ? "none" : std::to_string(manifest.contributes.prompts.size())) << "\n";
  for (auto const& prompt : manifest.contributes.prompts)
  {
    output << "    " << sanitize_inline_text(prompt.name);
    if (!prompt.description.empty())
      output << " - " << sanitize_inline_text(prompt.description);
    output << "\n";
  }
  output << "  skills: " << (manifest.contributes.skills.empty() ? "none" : std::to_string(manifest.contributes.skills.size())) << "\n";
  for (auto const& skill : manifest.contributes.skills)
  {
    output << "    " << sanitize_inline_text(skill.name);
    if (!skill.description.empty())
      output << " - " << sanitize_inline_text(skill.description);
    output << "\n";
  }
  output << "  event_hooks: " << (manifest.contributes.event_hooks.empty() ? "none" : std::to_string(manifest.contributes.event_hooks.size())) << "\n";
  output << "  note: inspection lists metadata only; no plugin process is started yet.";
  return output.str();
}

std::string format_valid_plugin_manifest_text(ava::plugin::PluginManifest const& manifest, runtime::session_ts const& unlocked_session)
{
  std::ostringstream output;
  output << "Valid plugin manifest\n";
  output << "  id: " << sanitize_inline_text(manifest.id) << "\n";
  output << "  name: " << sanitize_inline_text(manifest.name) << "\n";
  output << "  version: " << sanitize_inline_text(manifest.version) << "\n";
  output << "  api_version: " << sanitize_inline_text(manifest.api_version) << "\n";
  output << "  manifest: " << plugin_display_path(manifest.path, unlocked_session) << "\n";
  output << "  capabilities: " << plugin_capabilities_text(manifest) << "\n";
  output << "  tools: " << manifest.contributes.tools.size() << "\n";
  output << "  commands: " << manifest.contributes.commands.size() << "\n";
  output << "  prompts: " << manifest.contributes.prompts.size() << "\n";
  output << "  skills: " << manifest.contributes.skills.size() << "\n";
  output << "  event_hooks: " << manifest.contributes.event_hooks.size() << "\n";
  output << "  note: validation parsed the manifest only; no entrypoint was executed.";
  return output.str();
}

std::string format_plugin_installed_text(ava::plugin::PluginManifest const& manifest, std::filesystem::path const& source_dir,
                                         std::filesystem::path const& destination_dir, runtime::session_ts const& unlocked_session)
{
  std::ostringstream output;
  output << "Installed global plugin " << sanitize_inline_text(manifest.id) << "\n";
  output << "  source: " << plugin_display_path(source_dir, unlocked_session) << "\n";
  output << "  target: " << plugin_display_path(destination_dir, unlocked_session) << "\n";
  output << "  status: disabled\n";
  output << "  note: no plugin process was started; use /plugins enable " << sanitize_inline_text(manifest.id) << " to enable it.";
  return output.str();
}

std::string format_plugin_removed_text(ava::plugin::PluginManifest const& manifest, std::filesystem::path const& removed_dir,
                                       runtime::session_ts const& unlocked_session)
{
  std::ostringstream output;
  output << "Removed global plugin " << sanitize_inline_text(manifest.id) << "\n";
  output << "  path: " << plugin_display_path(removed_dir, unlocked_session) << "\n";
  output << "  note: no plugin process was stopped.";
  return output.str();
}

std::filesystem::path plugin_validate_path(runtime::session_ts const& unlocked_session, std::string_view path_text)
{
  auto path = std::filesystem::path(std::string(path_text));
  if (path.is_relative())
    path = runtime::session_ts::crat(unlocked_session)->current_dir() / path;
  return path.lexically_normal();
}

std::string trim_ascii_whitespace(std::string_view text)
{
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) ++start;
  auto end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
  return std::string(text.substr(start, end - start));
}

std::string plugin_validate_argument(std::string_view plugins_argument)
{
  std::size_t index = 0;
  while (index < plugins_argument.size() && std::isspace(static_cast<unsigned char>(plugins_argument[index])) != 0)
  {
    ++index;
  }
  while (index < plugins_argument.size() && std::isspace(static_cast<unsigned char>(plugins_argument[index])) == 0)
  {
    ++index;
  }
  return trim_ascii_whitespace(plugins_argument.substr(index));
}

ava::core::Result<std::string> install_plugin_from_path(runtime::session_ts const& unlocked_session, runtime::ExtensionResourcePolicy const& policy,
                                                        std::string_view path_text)
{
  auto source = ava::plugin::inspect_plugin_install_source(plugin_validate_path(unlocked_session, path_text));
  if (!source)
    return std::unexpected(std::move(source.error()));

  auto const diagnostics = plugin_diagnostics(policy, unlocked_session);
  if (find_plugin_status(diagnostics, source->manifest().id) || has_duplicate_plugin_failure(diagnostics, source->manifest().id))
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin id is already discovered").with_context("plugin", source->manifest().id));
  }

  auto installed = ava::plugin::install_global_plugin(std::move(*source), policy.plugin_discovery.global_plugins_dir);
  if (!installed)
    return std::unexpected(std::move(installed.error()));
  return format_plugin_installed_text(installed->manifest, installed->source_directory, installed->destination_directory, unlocked_session);
}

ava::core::Result<std::string> remove_plugin_by_id(runtime::session_ts const& unlocked_session, runtime::ExtensionResourcePolicy const& policy,
                                                   ava::plugin::PluginStatus const& status)
{
  auto removed = ava::plugin::remove_global_plugin(status, policy.plugin_discovery.global_plugins_dir);
  if (!removed)
    return std::unexpected(std::move(removed.error()));
  return format_plugin_removed_text(removed->manifest, removed->removed_directory, unlocked_session);
}

struct PluginRunArguments
{
  std::string plugin_id;
  std::string command_name;
  std::string arguments_json = "{}";
};

std::optional<std::string_view> consume_token(std::string_view& text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
  if (text.empty())
    return std::nullopt;
  auto const end = text.find_first_of(" \t\r\n");
  auto const token = text.substr(0, end == std::string_view::npos ? text.size() : end);
  text.remove_prefix(token.size());
  return token;
}

ava::core::Result<PluginRunArguments> parse_plugin_run_arguments(std::string_view argument)
{
  auto const subcommand = consume_token(argument);
  auto const plugin_id = consume_token(argument);
  auto const command_name = consume_token(argument);
  while (!argument.empty() && std::isspace(static_cast<unsigned char>(argument.front())) != 0) argument.remove_prefix(1);
  if (!subcommand || *subcommand != "run" || !plugin_id || !command_name)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "usage: /plugin run <plugin_id> <command> [arguments_json]"));
  }
  auto arguments_json = argument.empty() ? std::string("{}") : std::string(argument);
  if (!ava::core::json::is_valid_object(arguments_json))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin command arguments must be a JSON object"));
  }
  return PluginRunArguments{.plugin_id = std::string(*plugin_id), .command_name = std::string(*command_name), .arguments_json = std::move(arguments_json)};
}

}  // namespace

ava::core::Result<CommandResult> run_plugins_command(runtime::session_ts& unlocked_session, CommandRequest const& request)
{
  CommandResult result;
  result.handled = true;
  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  auto const [workspace_dir, session_process_scope] = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->workspace_dir(), session_r->session_process_scope()};
  }();
  auto const usage = [&]() {
    add_output(result, missing_argument("/plugins "
                                        "<list|inspect|install|remove|enable|disable|validate|failures|prompts|prompt|skills|skill|dynamic-prompts|dynamic-"
                                        "prompt|dynamic-skills|dynamic-skill> "
                                        "..."));
    return result;
  };

  auto const argument = command_argument(request.command, "/plugins");
  auto const args = split_command_arguments(argument);
  if (args.empty())
    return usage();

  auto const& subcommand = args[0];
  if (subcommand == "list")
  {
    if (args.size() != 1)
      return usage();
    add_output(result, format_plugin_list_text(plugin_diagnostics(resource_policy, unlocked_session), unlocked_session));
    return result;
  }

  if (subcommand == "failures")
  {
    if (args.size() != 1)
      return usage();
    add_output(result, format_plugin_failures_text(plugin_diagnostics(resource_policy, unlocked_session), unlocked_session));
    return result;
  }

  if (subcommand == "inspect")
  {
    if (args.size() != 2)
      return usage();
    auto const diagnostics = plugin_diagnostics(resource_policy, unlocked_session);
    auto const* status = find_plugin_status(diagnostics, args[1]);
    if (!status)
    {
      add_output(result, plugin_not_found_text(diagnostics, args[1]));
      return result;
    }
    add_output(result, format_plugin_inspect_text(*status, unlocked_session));
    return result;
  }

  if (subcommand == "prompts" || subcommand == "skills")
  {
    if (args.size() != 2)
      return usage();
    auto const diagnostics = plugin_diagnostics(resource_policy, unlocked_session);
    auto const* status = find_plugin_status(diagnostics, args[1]);
    if (!status)
    {
      add_output(result, plugin_not_found_text(diagnostics, args[1]));
      return result;
    }
    auto const& resources = subcommand == "prompts" ? status->plugin.manifest.contributes.prompts : status->plugin.manifest.contributes.skills;
    add_output(result, format_plugin_resource_list_text(*status, unlocked_session, subcommand, resources));
    return result;
  }

  if (subcommand == "prompt" || subcommand == "skill")
  {
    if (args.size() != 3)
      return usage();
    auto const diagnostics = plugin_diagnostics(resource_policy, unlocked_session);
    auto const* status = find_plugin_status(diagnostics, args[1]);
    if (!status)
    {
      add_output(result, plugin_not_found_text(diagnostics, args[1]));
      return result;
    }
    auto const& resources = subcommand == "prompt" ? status->plugin.manifest.contributes.prompts : status->plugin.manifest.contributes.skills;
    auto const* resource = find_plugin_resource(resources, args[2]);
    if (!resource)
    {
      add_output(result, std::string(subcommand) + " not found: " + sanitize_inline_text(args[2]));
      return result;
    }
    auto content = read_plugin_resource(status->plugin.manifest, *resource);
    if (!content)
    {
      add_output(result, content.error().format());
      return result;
    }
    add_output(result, format_plugin_resource_text(status->plugin.manifest, *resource, subcommand, unlocked_session, std::move(*content)));
    return result;
  }

  if (subcommand == "dynamic-prompts" || subcommand == "dynamic-skills")
  {
    if (args.size() != 1)
      return usage();
    auto const kind = subcommand == "dynamic-prompts" ? ava::plugin::PluginDynamicResourceKind::Prompt : ava::plugin::PluginDynamicResourceKind::Skill;
    auto const kind_name = std::string(ava::plugin::plugin_dynamic_resource_kind_name(kind));
    auto const diagnostics = plugin_diagnostics(resource_policy, unlocked_session);
    std::vector<DynamicResourceListEntry> entries;
    std::vector<DynamicResourceFailure> failures;

    for (auto const& status : diagnostics.plugins)
    {
      if (!status.enabled)
        continue;
      if (!ava::plugin::plugin_has_capability(status.plugin.manifest, dynamic_resource_capability(kind)))
        continue;
      auto const call_id = ava::core::make_id("dynres");
      auto const call_label = status.plugin.manifest.id + ":" + kind_name + ":list";
      if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "plugin_resource", call_label); !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }

      auto context = make_tool_context(unlocked_session, request.permission_resolver);
      context.permission_tool_name = "plugin_resource";
      context.cancel_requested = request.cancel_requested;
      context.permission_request_ids = std::make_shared<std::vector<std::string>>();

      auto fail = [&](std::string text, ava::agent::ToolTimelineStatus timeline_status = ava::agent::ToolTimelineStatus::Error) -> ava::core::VoidResult {
        failures.push_back(DynamicResourceFailure{.plugin_id = status.plugin.manifest.id, .message = text});
        return record_dynamic_resource_result(unlocked_session, request.event_sink, result, call_id, timeline_status, std::move(text), {}, context);
      };

      if (request.cancel_requested && request.cancel_requested())
      {
        if (auto recorded = fail("plugin resource list canceled", ava::agent::ToolTimelineStatus::Canceled); !recorded)
        {
          return std::unexpected(std::move(recorded.error()));
        }
        continue;
      }

      if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::PluginExecute, status.plugin.manifest.path, call_label,
                                                          "plugin_resource", "plugin dynamic resource lookup requires permission");
          !permission)
      {
        if (auto recorded = fail(permission.error().format()); !recorded)
        {
          return std::unexpected(std::move(recorded.error()));
        }
        continue;
      }

      ava::plugin::PluginRunnerOptions options;
      options.workspace_dir = workspace_dir;
      options.process_scope = session_process_scope;
      auto process = ava::plugin::PluginProcess::start(status.plugin.manifest, options, request.cancel_requested);
      if (!process)
      {
        auto const status =
            plugin_command_error_is_canceled(process.error()) ? ava::agent::ToolTimelineStatus::Canceled : ava::agent::ToolTimelineStatus::Error;
        if (auto recorded = fail(process.error().format(), status); !recorded)
        {
          return std::unexpected(std::move(recorded.error()));
        }
        continue;
      }

      auto proxy_handler = ava::plugin::make_core_service_proxy_handler(context, status.plugin.manifest, std::string(dynamic_resource_contribution_kind(kind)),
                                                                        "list", call_label, call_id);
      auto listed = (*process)->list_resources(kind, request.cancel_requested, std::move(proxy_handler));
      auto shutdown = (*process)->shutdown();
      if (!listed)
      {
        auto const status = plugin_command_error_is_canceled(listed.error()) ? ava::agent::ToolTimelineStatus::Canceled : ava::agent::ToolTimelineStatus::Error;
        if (auto recorded = fail(listed.error().format(), status); !recorded)
        {
          return std::unexpected(std::move(recorded.error()));
        }
        continue;
      }
      if (!shutdown)
      {
        if (auto recorded = fail(shutdown.error().format()); !recorded)
        {
          return std::unexpected(std::move(recorded.error()));
        }
        continue;
      }
      if (!listed->ok)
      {
        auto text = "plugin dynamic " + kind_name + " list failed: " + listed->content;
        if (auto recorded = fail(std::move(text)); !recorded)
        {
          return std::unexpected(std::move(recorded.error()));
        }
        continue;
      }

      for (auto const& resource : listed->resources)
      {
        entries.push_back(DynamicResourceListEntry{.plugin_id = status.plugin.manifest.id, .resource = resource});
      }
      if (auto recorded = record_dynamic_resource_result(unlocked_session, request.event_sink, result, call_id, ava::agent::ToolTimelineStatus::Success,
                                                         std::to_string(listed->resources.size()) + " dynamic " + dynamic_resource_plural(kind), {}, context);
          !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
    }

    add_output(result, format_dynamic_resource_list_text(kind, entries, failures));
    return result;
  }

  if (subcommand == "dynamic-prompt" || subcommand == "dynamic-skill")
  {
    if (args.size() != 3)
      return usage();
    auto const kind = subcommand == "dynamic-prompt" ? ava::plugin::PluginDynamicResourceKind::Prompt : ava::plugin::PluginDynamicResourceKind::Skill;
    auto const kind_name = std::string(ava::plugin::plugin_dynamic_resource_kind_name(kind));
    auto const diagnostics = plugin_diagnostics(resource_policy, unlocked_session);
    auto const* status = find_plugin_status(diagnostics, args[1]);
    if (!status)
    {
      add_output(result, plugin_not_found_text(diagnostics, args[1]));
      return result;
    }
    if (!status->enabled)
    {
      add_output(result, "plugin is disabled: " + sanitize_inline_text(args[1]));
      return result;
    }
    if (!ava::plugin::is_valid_dynamic_resource_name(args[2]))
    {
      add_output(result, invalid_dynamic_resource_name_text(kind, args[2]));
      return result;
    }
    if (!ava::plugin::plugin_has_capability(status->plugin.manifest, dynamic_resource_capability(kind)))
    {
      add_output(result, dynamic_resource_unsupported_text(status->plugin.manifest, kind));
      return result;
    }

    auto const call_id = ava::core::make_id("dynres");
    auto const call_label = status->plugin.manifest.id + ":" + kind_name + ":" + args[2];
    if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "plugin_resource", call_label); !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }

    auto context = make_tool_context(unlocked_session, request.permission_resolver);
    context.permission_tool_name = "plugin_resource";
    context.cancel_requested = request.cancel_requested;
    context.permission_request_ids = std::make_shared<std::vector<std::string>>();

    auto fail = [&](std::string text, ava::agent::ToolTimelineStatus status = ava::agent::ToolTimelineStatus::Error) -> ava::core::Result<CommandResult> {
      if (auto recorded = record_dynamic_resource_result(unlocked_session, request.event_sink, result, call_id, status, text, {}, context); !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
      add_output(result, std::move(text));
      return result;
    };

    if (request.cancel_requested && request.cancel_requested())
    {
      return fail("plugin resource read canceled", ava::agent::ToolTimelineStatus::Canceled);
    }

    if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::PluginExecute, status->plugin.manifest.path, call_label,
                                                        "plugin_resource", "plugin dynamic resource read requires permission");
        !permission)
    {
      return fail(permission.error().format());
    }

    ava::plugin::PluginRunnerOptions options;
    options.workspace_dir = workspace_dir;
    options.process_scope = session_process_scope;
    auto process = ava::plugin::PluginProcess::start(status->plugin.manifest, options, request.cancel_requested);
    if (!process)
    {
      auto const status = plugin_command_error_is_canceled(process.error()) ? ava::agent::ToolTimelineStatus::Canceled : ava::agent::ToolTimelineStatus::Error;
      return fail(process.error().format(), status);
    }
    auto proxy_handler = ava::plugin::make_core_service_proxy_handler(context, status->plugin.manifest, std::string(dynamic_resource_contribution_kind(kind)),
                                                                      args[2], call_label, call_id);
    auto content = (*process)->read_resource(kind, args[2], request.cancel_requested, std::move(proxy_handler));
    auto shutdown = (*process)->shutdown();
    if (!content)
    {
      auto const status = plugin_command_error_is_canceled(content.error()) ? ava::agent::ToolTimelineStatus::Canceled : ava::agent::ToolTimelineStatus::Error;
      return fail(content.error().format(), status);
    }
    if (!shutdown)
      return fail(shutdown.error().format());
    if (!content->ok)
    {
      return fail("plugin dynamic " + kind_name + " " + status->plugin.manifest.id + "/" + args[2] + " failed: " + content->content);
    }

    if (auto recorded = record_dynamic_resource_result(unlocked_session, request.event_sink, result, call_id, ava::agent::ToolTimelineStatus::Success, "ok",
                                                       content->content, context);
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, format_dynamic_resource_text(status->plugin.manifest, kind, args[2], std::move(content->content)));
    return result;
  }

  if (subcommand == "install")
  {
    auto const path_text = plugin_validate_argument(argument);
    if (path_text.empty())
      return usage();
    auto installed = install_plugin_from_path(unlocked_session, resource_policy, path_text);
    if (!installed)
    {
      add_output(result, installed.error().format());
      return result;
    }
    add_output(result, std::move(*installed));
    return result;
  }

  if (subcommand == "remove")
  {
    if (args.size() != 2)
      return usage();
    auto const diagnostics = plugin_diagnostics(resource_policy, unlocked_session);
    auto const* status = find_plugin_status(diagnostics, args[1]);
    if (!status)
    {
      add_output(result, plugin_not_found_text(diagnostics, args[1]));
      return result;
    }
    auto removed = remove_plugin_by_id(unlocked_session, resource_policy, *status);
    if (!removed)
    {
      add_output(result, removed.error().format());
      return result;
    }
    add_output(result, std::move(*removed));
    return result;
  }

  if (subcommand == "enable" || subcommand == "disable")
  {
    if (args.size() != 2)
      return usage();
    bool const enabled = subcommand == "enable";
    auto const diagnostics = plugin_diagnostics(resource_policy, unlocked_session);
    auto const* status = find_plugin_status(diagnostics, args[1]);
    if (!status)
    {
      add_output(result, plugin_not_found_text(diagnostics, args[1]));
      return result;
    }
    auto stored = ava::plugin::set_plugin_enabled(resource_policy.plugin_enablement_file, workspace_dir, args[1], enabled, status->plugin.scope);
    if (!stored)
    {
      add_output(result, stored.error().format());
      return result;
    }
    add_output(result, std::string(enabled ? "Enabled " : "Disabled ") + plugin_scope_text(status->plugin.scope) + " plugin " + sanitize_inline_text(args[1]) +
                           (enabled ? ". No plugin process was started." : ". No plugin process was stopped."));
    return result;
  }

  if (subcommand == "validate")
  {
    auto const path_text = plugin_validate_argument(argument);
    if (path_text.empty())
      return usage();
    auto manifest = ava::plugin::load_plugin_manifest(plugin_validate_path(unlocked_session, path_text));
    if (!manifest)
    {
      add_output(result, manifest.error().format());
      return result;
    }
    add_output(result, format_valid_plugin_manifest_text(*manifest, unlocked_session));
    return result;
  }

  return usage();
}

ava::core::Result<CommandResult> run_plugin_command(runtime::session_ts& unlocked_session, CommandRequest const& request)
{
  PluginUiInvocationGuard const ui_capability_guard(request.plugin_ui_capability);
  CommandResult result;
  result.handled = true;
  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  auto const [workspace_dir, session_process_scope] = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->workspace_dir(), session_r->session_process_scope()};
  }();
  auto run_args = parse_plugin_run_arguments(command_argument(request.command, "/plugin"));
  if (!run_args)
  {
    add_output(result, run_args.error().message());
    return result;
  }

  auto const diagnostics = plugin_diagnostics(resource_policy, unlocked_session);
  auto const* status = find_plugin_status(diagnostics, run_args->plugin_id);
  if (!status)
  {
    add_output(result, plugin_not_found_text(diagnostics, run_args->plugin_id));
    return result;
  }
  if (!status->enabled)
  {
    add_output(result, "plugin is disabled: " + sanitize_inline_text(run_args->plugin_id));
    return result;
  }
  auto const* command = find_plugin_command(status->plugin.manifest, run_args->command_name);
  if (!command)
  {
    add_output(result, "plugin command not found: " + sanitize_inline_text(run_args->command_name));
    return result;
  }

  auto context = make_tool_context(unlocked_session, request.permission_resolver);
  context.permission_tool_name = "plugin_command";
  context.cancel_requested = request.cancel_requested;
  auto const call_id = ava::core::make_id("cmd");
  auto const command_label = status->plugin.manifest.id + ":" + command->name;
  if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "plugin_command", command_label); !recorded)
  {
    return std::unexpected(std::move(recorded.error()));
  }

  auto fail = [&](ava::core::Error const& error) -> ava::core::Result<CommandResult> {
    auto const text = error.format();
    if (auto recorded =
            record_tool_result(unlocked_session, request.event_sink, result, call_id, "plugin_command", ava::agent::ToolTimelineStatus::Error, text);
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, text);
    return result;
  };

  auto const command_canceled = [&] { return plugin_callback_canceled(request.cancel_requested); };
  auto const canceled_error = [] { return ava::core::Error(ava::core::ErrorCategory::Unknown, "plugin command canceled"); };
  auto const unavailable_error = [] { return ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "plugin UI capability is unavailable"); };
  if (command_canceled())
    return fail(canceled_error());
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::PluginExecute, status->plugin.manifest.path,
                                                      status->plugin.manifest.id, "plugin_command", "plugin command requires permission");
      !permission)
  {
    return fail(permission.error());
  }
  if (command_canceled())
    return fail(canceled_error());
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::PluginCommandRun, status->plugin.manifest.path, command_label,
                                                      "plugin_command", "plugin command requires permission");
      !permission)
  {
    return fail(permission.error());
  }
  if (command_canceled())
    return fail(canceled_error());

  ava::plugin::PluginRunnerOptions options;
  options.workspace_dir = workspace_dir;
  options.process_scope = session_process_scope;

  std::optional<PluginUiInvocationClaim> ui_claim;
  std::shared_ptr<ExactPluginEnablementRevocation> ui_enablement;
  ava::plugin::CancelCallback active_cancel = request.cancel_requested;
  if (request.plugin_ui_capability && plugin_declares_ui_capability(status->plugin.manifest))
  {
    auto claimed = claim_plugin_ui_invocation_capability(request.plugin_ui_capability, request.command, status->plugin.manifest.id, command->name);
    if (!claimed)
      return fail(claimed.error());
    ui_claim.emplace(std::move(*claimed));

    auto const capability_deadline = ui_claim->deadline();
    auto permission_cancel = [cancel_requested = request.cancel_requested, capability_deadline] {
      return plugin_callback_canceled(cancel_requested) || std::chrono::steady_clock::now() >= capability_deadline;
    };
    context.cancel_requested = permission_cancel;
    if (permission_cancel())
      return fail(unavailable_error());
    if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::PluginUiPresent, status->plugin.manifest.path, command_label,
                                                        "plugin_ui", "plugin UI invocation authority requires approval");
        !permission)
    {
      return fail(unavailable_error());
    }
    if (permission_cancel())
      return fail(unavailable_error());

    ui_enablement =
        std::make_shared<ExactPluginEnablementRevocation>(diagnostics.enablement_file, workspace_dir, status->plugin.manifest.id, status->plugin.scope);
    if (ui_enablement->revoked(true))
      return fail(unavailable_error());

    active_cancel = [cancel_requested = request.cancel_requested, capability_deadline, ui_enablement] {
      return plugin_callback_canceled(cancel_requested) || std::chrono::steady_clock::now() >= capability_deadline || ui_enablement->revoked();
    };
    context.cancel_requested = active_cancel;
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(capability_deadline - std::chrono::steady_clock::now());
    if (remaining < std::chrono::milliseconds(50) || plugin_callback_canceled(active_cancel))
      return fail(unavailable_error());
    options.startup_timeout = std::min(options.startup_timeout, remaining);
  }

  if (ui_enablement && ui_enablement->revoked(true))
    return fail(unavailable_error());
  auto process = ava::plugin::PluginProcess::start(status->plugin.manifest, options, active_cancel);
  if (!process)
    return ui_claim ? fail(unavailable_error()) : fail(process.error());
  auto proxy_handler = ava::plugin::make_core_service_proxy_handler(context, status->plugin.manifest, "command", command->name, command_label, call_id);
  auto ui_handler = ui_claim ? ui_claim->handler() : ava::plugin::PluginUiHandler{};
  if (ui_handler)
  {
    auto presenter = std::move(ui_handler.callback);
    ui_handler.callback = [presenter = std::move(presenter), active_cancel](
                              ava::plugin::PluginUiRequest const& ui_request, std::chrono::steady_clock::time_point deadline,
                              ava::plugin::CancelCallback cancel_requested) mutable -> ava::core::Result<ava::plugin::PluginUiAction> {
      auto presentation_cancel = [cancel_requested = std::move(cancel_requested), active_cancel, deadline] {
        return plugin_callback_canceled(cancel_requested) || plugin_callback_canceled(active_cancel) || std::chrono::steady_clock::now() >= deadline;
      };
      if (presentation_cancel())
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "plugin UI presentation canceled"));
      return presenter(ui_request, deadline, std::move(presentation_cancel));
    };
  }
  auto command_result =
      (*process)->call_command(command->name, run_args->arguments_json, call_id, active_cancel, std::move(proxy_handler), std::move(ui_handler));
  auto shutdown = (*process)->shutdown();
  if (!command_result)
  {
    if (ui_claim || command_result.error().message().starts_with("plugin UI"))
      return fail(unavailable_error());
    return fail(command_result.error());
  }
  if (!shutdown)
    return ui_claim ? fail(unavailable_error()) : fail(shutdown.error());

  if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "plugin_command",
                                         command_result->ok ? ava::agent::ToolTimelineStatus::Success : ava::agent::ToolTimelineStatus::Error,
                                         command_result->ok ? "ok" : "plugin command returned error", command_result->content);
      !recorded)
  {
    return std::unexpected(std::move(recorded.error()));
  }
  add_output(result, command_result->content);
  return result;
}

}  // namespace ava::app
