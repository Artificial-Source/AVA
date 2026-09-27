#include "sys.h"
#include "ava/app/plugin_event_hooks.h"
#include "ava/tools/tool_permission.h"
#include "ava/event/events.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/discovery.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/runner.h"
#include "ava/plugin/tool_broker.h"
#include "ava/core/ids.h"

#include <algorithm>
#include <cctype>
#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ava::app {
namespace {

struct PluginEventHookBinding
{
  ava::plugin::PluginManifest manifest;
  ava::plugin::PluginEventHookContribution hook;
};

std::string normalized_event_name(std::string_view event)
{
  std::string normalized;
  normalized.reserve(event.size());
  for (char const ch : event)
  {
    if (ch == '.')
    {
      normalized.push_back('_');
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return normalized;
}

bool event_matches(std::string_view hook_event, std::string_view runtime_event)
{
  return normalized_event_name(hook_event) == normalized_event_name(runtime_event);
}

bool contains(std::vector<std::string> const& values, std::string_view value)
{
  return std::ranges::find(values, value) != values.end();
}

std::string observation_key(ava::plugin::PluginManifest const& manifest, std::string_view event)
{
  return manifest.id + "\n" + std::string(event);
}

std::string_view event_call_id(ava::event::RuntimeEvent const& event)
{
  return std::visit(
      [](auto const& concrete) -> std::string_view {
        using Event = std::remove_cvref_t<decltype(concrete)>;
        if constexpr (std::same_as<Event, ava::event::ProviderEvent> || std::same_as<Event, ava::event::ToolStartEvent> ||
                      std::same_as<Event, ava::event::ToolProgressEvent> || std::same_as<Event, ava::event::ToolResultEvent>)
          return concrete.payload.call_id;
        return {};
      },
      event.payload());
}

class PluginEventObserverState final
{
 public:
  explicit PluginEventObserverState(PluginEventObserverOptions options) : options_(std::move(options)) { }

  void observe(ava::event::RuntimeEvent const& event)
  {
    ensure_loaded();
    if (hooks_.empty())
      return;

    auto const event_name = ava::event::to_string(event.type());
    auto const payload = ava::event::to_event_envelope(event).payload_json;
    auto const call_id = std::string(event_call_id(event));
    for (auto const& binding : hooks_)
    {
      if (!event_matches(binding.hook.event, event_name))
        continue;
      if (!ensure_launch_permission(binding.manifest))
        continue;
      if (!ensure_observe_permission(binding.manifest, binding.hook.event))
        continue;

      ava::plugin::PluginRunnerOptions runner_options;
      runner_options.workspace_dir = options_.workspace_dir;
      runner_options.process_scope = options_.process_scope;
      auto process = ava::plugin::PluginProcess::start(binding.manifest, runner_options, options_.cancel_requested);
      if (!process)
      {
        notify_failure(binding, process.error());
        continue;
      }
      auto const hook_label = binding.manifest.id + ":" + binding.hook.event;
      auto proxy_handler = ava::plugin::make_core_service_proxy_handler(permission_context("plugin_event_observe"), binding.manifest, "event_hook",
                                                                        binding.hook.event, hook_label, call_id);
      auto observed = (*process)->observe_event(event_name, payload, call_id, options_.cancel_requested, std::move(proxy_handler));
      if (!observed)
        notify_failure(binding, observed.error());
      auto shutdown = (*process)->shutdown();
      if (!shutdown)
        notify_failure(binding, shutdown.error());
    }
  }

 private:
  void notify_failure(PluginEventHookBinding const& binding, ava::core::Error const& error) const
  {
    if (!options_.hook_failure_sink)
      return;
    try
    {
      options_.hook_failure_sink(binding.manifest.id, binding.hook.event, error);
    }
    catch (...)
    {
    }
  }

  void ensure_loaded()
  {
    if (loaded_)
      return;
    loaded_ = true;
    if (options_.workspace_dir.empty())
      return;

    auto discovery_options = ava::plugin::default_plugin_discovery_options(options_.workspace_dir);
    if (!options_.plugin_global_plugins_dir.empty())
    {
      discovery_options.global_plugins_dir = options_.plugin_global_plugins_dir;
    }
    if (!options_.include_project_plugins)
    {
      discovery_options.project_plugins_dir.clear();
    }
    else if (!options_.plugin_project_plugins_dir.empty())
    {
      discovery_options.project_plugins_dir = options_.plugin_project_plugins_dir;
    }
    auto enablement_file = options_.plugin_enablement_file;
    if (enablement_file.empty())
      enablement_file = ava::plugin::default_plugin_enablement_file();

    auto diagnostics = ava::plugin::collect_plugin_diagnostics(discovery_options, enablement_file, options_.workspace_dir);
    for (auto const& status : diagnostics.plugins)
    {
      if (!status.enabled)
        continue;
      for (auto const& hook : status.plugin.manifest.contributes.event_hooks)
      {
        hooks_.push_back(PluginEventHookBinding{.manifest = status.plugin.manifest, .hook = hook});
      }
    }
  }

  ava::tools::ToolContext permission_context(std::string_view tool_name) const
  {
    return ava::tools::ToolContext{.workspace_dir = options_.workspace_dir,
                                   .mode = options_.mode,
                                   .permission_resolver = options_.permission_resolver,
                                   .permission_audit_sink = options_.permission_audit_sink,
                                   .cancel_requested = options_.cancel_requested,
                                   .permission_tool_name = std::string(tool_name),
                                   .current_tool_name = std::string(tool_name),
                                   .process_scope = options_.process_scope,
                                   .session_id = options_.session_id,
                                   .provider_id = options_.provider_id,
                                   .model_id = options_.model_id,
                                   .current_dir = options_.current_dir};
  }

  bool ensure_launch_permission(ava::plugin::PluginManifest const& manifest)
  {
    if (contains(launch_allowed_plugins_, manifest.id))
      return true;
    if (contains(launch_denied_plugins_, manifest.id))
      return false;

    auto const context = permission_context("plugin_event_observe");
    auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::PluginExecute, manifest.path, manifest.id, "plugin_event_observe",
                                                    "plugin event hook launch requires permission");
    if (permission)
    {
      launch_allowed_plugins_.push_back(manifest.id);
      return true;
    }
    launch_denied_plugins_.push_back(manifest.id);
    return false;
  }

  bool ensure_observe_permission(ava::plugin::PluginManifest const& manifest, std::string_view event)
  {
    auto const key = observation_key(manifest, event);
    if (contains(observe_allowed_, key))
      return true;
    if (contains(observe_denied_, key))
      return false;

    auto const context = permission_context("plugin_event_observe");
    auto const command = manifest.id + ":" + std::string(event);
    auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::PluginEventObserve, manifest.path, command, "plugin_event_observe",
                                                    "plugin event observation requires permission");
    if (permission)
    {
      observe_allowed_.push_back(key);
      return true;
    }
    observe_denied_.push_back(key);
    return false;
  }

  PluginEventObserverOptions options_;
  bool loaded_ = false;
  std::vector<PluginEventHookBinding> hooks_;
  std::vector<std::string> launch_allowed_plugins_;
  std::vector<std::string> launch_denied_plugins_;
  std::vector<std::string> observe_allowed_;
  std::vector<std::string> observe_denied_;
};

}  // namespace

PluginEventObserverOptions plugin_event_observer_options(runtime::session_ts& unlocked_session, ava::permissions::PermissionResolver permission_resolver)
{
  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  return PluginEventObserverOptions{
      .workspace_dir = session_r->workspace_dir(),
      .plugin_global_plugins_dir = resource_policy.plugin_discovery.global_plugins_dir,
      .plugin_project_plugins_dir = resource_policy.plugin_discovery.project_plugins_dir,
      .plugin_enablement_file = resource_policy.plugin_enablement_file,
      .include_project_plugins = resource_policy.include_project_resources,
      .mode = session_r->mode(),
      .permission_resolver = std::move(permission_resolver),
      // Permission audits are owner-routed outside active runs and replaced by
      // run_prompt's immutable generation route during one.
      .permission_audit_sink = [sink = session_r->owner_append_route_1()](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        return ava::agent::append_permission_decision(sink, event);
      },
      .cancel_requested = nullptr,
      .process_scope = session_r->session_process_scope(),
      .session_id = session_r->store.session_id(),
      .provider_id = session_r->model().provider_id,
      .model_id = session_r->model().model_id,
      .current_dir = session_r->current_dir()};
}

ava::event::RuntimeEventSink make_plugin_event_observer_sink(PluginEventObserverOptions options, ava::event::RuntimeEventSink next)
{
  auto state = std::make_shared<PluginEventObserverState>(std::move(options));
  return [state = std::move(state), next = std::move(next)](ava::event::RuntimeEvent const& event) -> ava::core::VoidResult {
    state->observe(event);
    return ava::event::emit_event(next, event);
  };
}

}  // namespace ava::app
