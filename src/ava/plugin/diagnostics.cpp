#include "sys.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/tool_broker.h"

#include <algorithm>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace ava::plugin {
namespace {

PluginFailure make_failure(PluginScope scope, std::filesystem::path const& path, ava::core::Error const& error,
                           PluginFailureKind kind = PluginFailureKind::Discovery)
{
  auto const failure = ava::diagnostics::safe_failure_from_error(ava::diagnostics::ComponentClass::Plugin, error);
  return PluginFailure{.scope = scope,
                       .path = path,
                       .kind = kind,
                       .failure = failure,
                       .message = ava::diagnostics::serialize_safe_failure_human(failure),
                       .details = ava::diagnostics::serialize_safe_failure_json(failure)};
}

PluginFailure make_failure(PluginScope scope, std::filesystem::path const& path, ava::core::ErrorCategory category,
                           PluginFailureKind kind = PluginFailureKind::Discovery)
{
  auto const error = ava::core::Error(category, "plugin diagnostic failure");
  return make_failure(scope, path, error, kind);
}

bool is_install_staging_directory(std::filesystem::path const& path)
{
  return path.filename().generic_string().find(".installing-") != std::string::npos;
}

void collect_from_dir(std::filesystem::path const& root, PluginScope scope, PluginDiagnostics& diagnostics)
{
  if (root.empty())
    return;
  std::error_code exists_error;
  if (!std::filesystem::exists(root, exists_error))
    return;
  if (exists_error)
  {
    diagnostics.failures.push_back(make_failure(scope, root, ava::core::ErrorCategory::Io));
    return;
  }

  std::error_code iter_error;
  for (std::filesystem::directory_iterator it(root, iter_error), end; !iter_error && it != end; it.increment(iter_error))
  {
    std::error_code entry_error;
    if (it->is_symlink(entry_error) || entry_error)
      continue;
    if (!it->is_directory(entry_error) || entry_error)
      continue;
    if (scope == PluginScope::Global && is_install_staging_directory(it->path()))
      continue;
    auto const manifest_path = it->path() / "plugin.json";
    std::error_code manifest_error;
    if (!std::filesystem::exists(manifest_path, manifest_error))
      continue;
    if (manifest_error)
    {
      diagnostics.failures.push_back(make_failure(scope, manifest_path, ava::core::ErrorCategory::Io));
      continue;
    }
    auto manifest = load_plugin_manifest(manifest_path);
    if (!manifest)
    {
      diagnostics.failures.push_back(make_failure(scope, manifest_path, manifest.error()));
      continue;
    }
    diagnostics.plugins.push_back(PluginStatus{.plugin = DiscoveredPlugin{.manifest = std::move(*manifest), .scope = scope}});
  }
  if (iter_error)
  {
    diagnostics.failures.push_back(make_failure(scope, root, ava::core::ErrorCategory::Io));
  }
}

void disable_duplicate_ids(PluginDiagnostics& diagnostics)
{
  std::ranges::sort(diagnostics.plugins, [](PluginStatus const& left, PluginStatus const& right) {
    if (left.plugin.manifest.id != right.plugin.manifest.id)
      return left.plugin.manifest.id < right.plugin.manifest.id;
    return static_cast<int>(left.plugin.scope) < static_cast<int>(right.plugin.scope);
  });

  std::vector<std::string> duplicate_ids;
  for (std::size_t index = 0; index < diagnostics.plugins.size();)
  {
    auto const start = index;
    while (index < diagnostics.plugins.size() && diagnostics.plugins[index].plugin.manifest.id == diagnostics.plugins[start].plugin.manifest.id)
    {
      ++index;
    }
    if (index - start > 1)
      duplicate_ids.push_back(diagnostics.plugins[start].plugin.manifest.id);
  }
  if (duplicate_ids.empty())
    return;

  for (auto const& status : diagnostics.plugins)
  {
    if (std::ranges::find(duplicate_ids, status.plugin.manifest.id) == duplicate_ids.end())
      continue;
    diagnostics.failures.push_back(
        make_failure(status.plugin.scope, status.plugin.manifest.path, ava::core::ErrorCategory::InvalidArgument, PluginFailureKind::DuplicateId));
  }
  std::erase_if(diagnostics.plugins,
                [&](PluginStatus const& status) { return std::ranges::find(duplicate_ids, status.plugin.manifest.id) != duplicate_ids.end(); });
}

void apply_enablement(std::filesystem::path const& enablement_file, std::filesystem::path const& workspace_root, PluginDiagnostics& diagnostics)
{
  auto records = load_plugin_enablement(enablement_file);
  if (!records)
  {
    diagnostics.failures.push_back(make_failure(PluginScope::Project, enablement_file, records.error(), PluginFailureKind::Enablement));
    return;
  }
  auto const workspace = canonical_workspace_key(workspace_root);
  for (auto& status : diagnostics.plugins)
  {
    for (auto const& record : *records)
    {
      if (record.workspace == workspace && record.plugin_id == status.plugin.manifest.id && record.scope == status.plugin.scope)
      {
        status.enabled = record.enabled;
        break;
      }
    }
  }
}

void collect_model_tool_name_collisions(PluginDiagnostics& diagnostics)
{
  // Keys and binding identifiers borrow the stable plugin/contribution storage
  // for this scan only; reserving each status keeps its model-name strings stable.
  std::unordered_map<std::string_view, std::pair<std::string_view, std::string_view>> bindings;
  for (auto& status : diagnostics.plugins)
  {
    if (!status.enabled)
      continue;
    auto const& manifest = status.plugin.manifest;
    status.model_tools.reserve(manifest.contributes.tools.size());
    for (auto const& contribution : manifest.contributes.tools)
    {
      auto& tool = status.model_tools.emplace_back(PluginModelToolStatus{.model_tool_name = plugin_model_tool_name(manifest.id, contribution.name)});
      auto const [existing, inserted] = bindings.try_emplace(tool.model_tool_name, manifest.id, contribution.name);
      if (inserted)
        continue;
      tool.name_collision = true;
      auto failure = make_failure(status.plugin.scope, manifest.path, ava::core::ErrorCategory::InvalidArgument, PluginFailureKind::ModelToolNameCollision);
      failure.message = "tool registry collision: later contribution disabled";
      // Manifest validation limits these ASCII identifiers to 128-byte plugin
      // ids and 96-byte contribution names; their normalized model name is bounded too.
      failure.details = "plugin=\"" + manifest.id + "\" contribution=\"" + contribution.name + "\" model_tool=\"" + tool.model_tool_name +
                        "\" existing_plugin=\"" + std::string(existing->second.first) + "\" existing_contribution=\"" + std::string(existing->second.second) +
                        "\"";
      diagnostics.failures.push_back(std::move(failure));
    }
  }
}

}  // namespace

PluginDiagnostics collect_plugin_diagnostics(PluginDiscoveryOptions const& options, std::filesystem::path const& enablement_file,
                                             std::filesystem::path const& workspace_root)
{
  PluginDiagnostics diagnostics{.discovery_options = options, .enablement_file = enablement_file, .plugins = {}, .failures = {}};
  collect_from_dir(options.global_plugins_dir, PluginScope::Global, diagnostics);
  collect_from_dir(options.project_plugins_dir, PluginScope::Project, diagnostics);
  disable_duplicate_ids(diagnostics);
  apply_enablement(enablement_file, workspace_root, diagnostics);
  collect_model_tool_name_collisions(diagnostics);
  return diagnostics;
}

}  // namespace ava::plugin
