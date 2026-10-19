#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_session_support_internal.h"
#include "ava/app/command_sessions.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/session/Stats.h"
#include "ava/context/context_loader.h"
#include "ava/lsp/configured_provider.h"
#include "ava/core/fingerprint.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ava::app {
using session_command_support::contains_ascii_case_insensitive;
using session_command_support::load_runtime_entries;
using session_command_support::shorten_middle;
using session_command_support::trim_ascii;

namespace {

std::string format_cost_usd(long double value)
{
  std::ostringstream output;
  output << '$' << std::fixed << std::setprecision(6) << value;
  return output.str();
}

bool context_source_matches_query(runtime::ContextSourceMetadata const& source, std::string_view query)
{
  return contains_ascii_case_insensitive(source.path.generic_string(), query) ||
         contains_ascii_case_insensitive(ava::context::to_string(source.source_type), query);
}

std::string context_file_status(std::filesystem::path const& path, std::size_t loaded_bytes, std::uint64_t loaded_fingerprint)
{
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(path, status_error);
  if (status_error == std::errc::no_such_file_or_directory || status_error == std::errc::not_a_directory)
    return "status=missing";
  if (status_error)
    return "status=unreadable cause=" + sanitize_inline_text(status_error.message());
  if (!std::filesystem::exists(status))
    return "status=missing";
  if (std::filesystem::is_symlink(status))
    return "status=changed cause=symlink";
  if (!std::filesystem::is_regular_file(status))
    return "status=changed cause=not_regular";

  std::error_code size_error;
  auto const current_bytes = std::filesystem::file_size(path, size_error);
  if (size_error)
    return "status=unreadable cause=" + sanitize_inline_text(size_error.message());
  if (current_bytes != loaded_bytes)
    return "status=changed current_bytes=" + std::to_string(current_bytes);

  std::ifstream file(path, std::ios::binary);
  if (!file)
    return "status=unreadable cause=open_failed";

  std::string content;
  content.reserve(loaded_bytes);
  std::array<char, 4096> buffer{};
  while (file)
  {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (file.gcount() > 0)
      content.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
  }
  if (!file.eof() && file.fail())
    return "status=unreadable cause=read_failed";
  if (ava::core::content_fingerprint(content) == loaded_fingerprint)
    return "status=current current_bytes=" + std::to_string(current_bytes);
  return "status=changed current_bytes=" + std::to_string(current_bytes);
}

bool prompt_matches_query(runtime::session_ts const& unlocked_session, std::string_view query)
{
  if (query.empty())
    return true;
  if (contains_ascii_case_insensitive("prompt", query) || contains_ascii_case_insensitive("base_prompt", query) ||
      contains_ascii_case_insensitive("builtin", query) || contains_ascii_case_insensitive("override", query))
    return true;
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
  return session_r->base_prompt().source_path && contains_ascii_case_insensitive(session_r->base_prompt().source_path->generic_string(), query);
}

std::string_view freshness_source_kind_text(runtime::FreshnessSourceKind kind)
{
  switch (kind)
  {
    case runtime::FreshnessSourceKind::SystemPrompt:
      return "system_prompt";
    case runtime::FreshnessSourceKind::AppendSystemPrompt:
      return "append_system_prompt";
    case runtime::FreshnessSourceKind::PromptCommand:
      return "prompt_command";
    case runtime::FreshnessSourceKind::Skill:
      return "skill";
    case runtime::FreshnessSourceKind::PluginManifest:
      return "plugin_manifest";
    case runtime::FreshnessSourceKind::PluginPrompt:
      return "plugin_prompt";
    case runtime::FreshnessSourceKind::PluginSkill:
      return "plugin_skill";
  }
  return "unknown";
}

bool freshness_source_matches_query(runtime::FreshnessSourceMetadata const& source, std::string_view query)
{
  if (query.empty())
    return true;
  return contains_ascii_case_insensitive(freshness_source_kind_text(source.kind), query) || contains_ascii_case_insensitive(source.scope, query) ||
         contains_ascii_case_insensitive(source.source_id, query) || contains_ascii_case_insensitive(source.name, query) ||
         contains_ascii_case_insensitive(source.path.generic_string(), query);
}

std::size_t freshness_source_count(std::vector<runtime::FreshnessSourceMetadata> const& sources, runtime::FreshnessSourceKind kind)
{
  return static_cast<std::size_t>(std::ranges::count_if(sources, [&](auto const& source) { return source.kind == kind; }));
}

std::string freshness_source_status_text(runtime::FreshnessSourceMetadata const& source)
{
  if (source.path.empty())
    return "<inline>  loaded_bytes=" + std::to_string(source.byte_count) + "  status=inline";
  if (source.kind == runtime::FreshnessSourceKind::PluginPrompt || source.kind == runtime::FreshnessSourceKind::PluginSkill)
  {
    bool const loaded_snapshot = source.byte_count != 0 || source.content_fingerprint != 0;
    return source.path.string() + "  loaded_bytes=" + std::to_string(source.byte_count) +
           (loaded_snapshot ? "  status=loaded_snapshot" : "  status=unavailable");
  }
  return source.path.string() + "  loaded_bytes=" + std::to_string(source.byte_count) + "  " +
         context_file_status(source.path, source.byte_count, source.content_fingerprint);
}

std::string lsp_error_text(ava::core::Error const& error)
{
  std::string text = ava::core::to_string(error.category()) + ":" + error.message();
  for (auto const& item : error.context())
  {
    text += " ";
    text += item.key;
    text += "=";
    text += item.value;
  }
  return sanitize_inline_text(std::move(text));
}

bool lsp_config_matches_query(ava::lsp::ConfiguredLspConfigDiagnostic const& diagnostic, std::string_view query)
{
  if (query.empty())
    return true;
  if (contains_ascii_case_insensitive("lsp", query) || contains_ascii_case_insensitive("lsp_config", query))
    return true;
  if (contains_ascii_case_insensitive(diagnostic.scope, query) || contains_ascii_case_insensitive(diagnostic.path.generic_string(), query))
    return true;
  if (diagnostic.error && contains_ascii_case_insensitive(diagnostic.error->message(), query))
    return true;
  if (diagnostic.error)
  {
    return std::ranges::any_of(diagnostic.error->context(), [&](auto const& item) {
      return contains_ascii_case_insensitive(item.key, query) || contains_ascii_case_insensitive(item.value, query);
    });
  }
  return false;
}

std::string lsp_config_status_text(ava::lsp::ConfiguredLspConfigDiagnostic const& diagnostic)
{
  std::string output = diagnostic.path.string();
  if (diagnostic.error)
  {
    output += "  status=error  error=" + lsp_error_text(*diagnostic.error);
    return output;
  }
  if (!diagnostic.exists)
  {
    output += "  status=missing";
    return output;
  }
  output += "  loaded_bytes=" + std::to_string(diagnostic.byte_count) + "  status=";
  output += diagnostic.loaded ? "loaded" : "skipped";
  output += " servers=" + std::to_string(diagnostic.server_count);
  return output;
}

std::size_t configured_lsp_config_count(std::vector<ava::lsp::ConfiguredLspConfigDiagnostic> const& diagnostics)
{
  return static_cast<std::size_t>(std::ranges::count_if(diagnostics, [](auto const& diagnostic) { return diagnostic.exists || diagnostic.error.has_value(); }));
}

std::string lsp_provider_status_text(ava::lsp::ConfiguredLspProviderInspection const& inspection)
{
  if (inspection.error_count > 0)
    return "error";
  if (inspection.server_count > 0)
    return "configured";
  if (std::ranges::any_of(inspection.builtin_servers, [](auto const& server) { return server.status != ava::lsp::BuiltinServerStatus::Disabled; }))
    return "enabled";
  return "disabled";
}

std::size_t enabled_builtin_lsp_count(ava::lsp::ConfiguredLspProviderInspection const& inspection)
{
  return static_cast<std::size_t>(
      std::ranges::count_if(inspection.builtin_servers, [](auto const& server) { return server.status != ava::lsp::BuiltinServerStatus::Disabled; }));
}

std::size_t available_builtin_lsp_count(ava::lsp::ConfiguredLspProviderInspection const& inspection)
{
  return static_cast<std::size_t>(
      std::ranges::count_if(inspection.builtin_servers, [](auto const& server) { return server.status == ava::lsp::BuiltinServerStatus::Available; }));
}

bool path_exists_for_status(std::filesystem::path const& path)
{
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(path, status_error);
  return !status_error && std::filesystem::exists(status);
}

template <typename Value>
void append_known_value(std::ostringstream& output, bool& wrote_any, std::string_view label, std::optional<Value> const& value)
{
  if (!value)
    return;
  if (wrote_any)
    output << ' ';
  output << label << '=' << *value;
  wrote_any = true;
}

std::string compact_workspace_label(std::filesystem::path const& workspace)
{
  auto const filename = workspace.filename().generic_string();
  if (!filename.empty())
    return shorten_middle(filename, 32);
  return shorten_middle(workspace.generic_string(), 48);
}

std::string compact_cwd_label(std::filesystem::path const& cwd, std::filesystem::path const& workspace)
{
  auto text = display_path(cwd, workspace);
  if (text.empty())
    text = ".";
  return shorten_middle(std::move(text), 48);
}

std::string known_values_text(ava::session::Stats const& stats)
{
  std::ostringstream output;
  bool wrote_any = false;
  append_known_value(output, wrote_any, "input", stats.input_tokens);
  append_known_value(output, wrote_any, "output", stats.output_tokens);
  append_known_value(output, wrote_any, "reasoning", stats.reasoning_tokens);
  append_known_value(output, wrote_any, "cache_read", stats.cache_read_tokens);
  append_known_value(output, wrote_any, "cache_write", stats.cache_write_tokens);
  append_known_value(output, wrote_any, "total", stats.total_tokens);
  return wrote_any ? output.str() : std::string("unavailable");
}

std::string estimated_bytes_text(ava::session::Stats const& stats)
{
  std::ostringstream output;
  bool wrote_any = false;
  append_known_value(output, wrote_any, "input", stats.estimated_input_bytes);
  append_known_value(output, wrote_any, "output", stats.estimated_output_bytes);
  append_known_value(output, wrote_any, "total", stats.estimated_total_bytes);
  return wrote_any ? output.str() : std::string("unavailable");
}

std::string cost_text(ava::session::Stats const& stats)
{
  if (stats.cost_complete)
    return stats.total_cost_usd ? format_cost_usd(*stats.total_cost_usd) : "unavailable";
  if (stats.known_cost_usd)
  {
    return "at least " + format_cost_usd(*stats.known_cost_usd) + " (" + std::to_string(stats.unknown_cost_entries) + " unknown)";
  }
  return "incomplete (" + std::to_string(stats.unknown_cost_entries) + " unknown)";
}

std::string format_session_stats_text(runtime::session_ts const& unlocked_session, ava::session::Stats const& stats)
{
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
  std::ostringstream output;
  output << "Session stats\n";
  output << "  session: " << shorten_middle(session_r->store.session_id(), 32) << "   entries: " << stats.entry_count << '\n';
  runtime::ModelSelection_ts::crat model_selection_r = session_r->model_selection_r();
  output << "  model: " << model_selection_r->model.provider_id << '/' << model_selection_r->model.model_id
         << "   mode: " << ava::agent::to_string(session_r->mode())
         << '\n';
  output << "  workspace: " << compact_workspace_label(session_r->workspace_dir()) << "   cwd: "
         << compact_cwd_label(session_r->current_dir(), session_r->workspace_dir()) << '\n';
  if (!stats.first_timestamp.empty() || !stats.last_timestamp.empty())
  {
    output << "  time: " << (stats.first_timestamp.empty() ? "unknown" : stats.first_timestamp) << " -> "
           << (stats.last_timestamp.empty() ? "unknown" : stats.last_timestamp) << '\n';
  }

  output << "\nMessages:\n";
  output << "  user " << stats.counts.user_message << "   assistant " << stats.counts.assistant_message << "   tools " << stats.counts.tool_call << '/'
         << stats.counts.tool_result << "   permissions " << stats.counts.permission_decision << '\n';
  output << "  compactions " << stats.counts.compaction << "   mode/model " << stats.counts.mode_change << '/' << stats.counts.model_change
         << "   errors/cancels " << stats.counts.error << '/' << stats.counts.cancel << '\n';

  output << "\nUsage:\n";
  output << "  tokens: " << known_values_text(stats) << '\n';
  output << "  est bytes: " << estimated_bytes_text(stats) << '\n';
  output << "  cost: " << cost_text(stats) << "   usage entries exact/estimated " << stats.exact_usage_entries << '/' << stats.estimated_usage_entries << '\n';

  output << "\nHints:\n";
  output << "  export: /export   resume: ava --session " << session_r->store.session_id();
  return output.str();
}

}  // namespace

ava::core::Result<CommandResult> run_context_command(runtime::session_ts& unlocked_session, std::string_view query)
{
  CommandResult result;
  result.handled = true;
  auto const trimmed_query = trim_ascii(query);
  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  auto snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::tuple{session_r->workspace_dir(), session_r->anchor_set(), session_r->mode(), session_r->model_selection_r()->model,
                      session_r->project_trust().decision,
                      session_r->base_prompt(), session_r->context_sources(), session_r->freshness_sources()};
  }();
  auto const& [workspace_dir, anchor_set, mode, model, project_trust, base_prompt, context_sources, freshness_sources] = snapshot;
  auto const prompt_matches = prompt_matches_query(unlocked_session, trimmed_query);
  auto const project_lsp_config = workspace_dir / ".ava" / "lsp.json";
  auto const lsp_inspection = ava::lsp::inspect_configured_lsp_provider(ava::lsp::ConfiguredLspProviderFiles{
      .global_config_file = resource_policy.global_lsp_config_file,
      .project_config_file = resource_policy.project_lsp_config_file,
      .workspace_root = workspace_dir,
      .anchor_set = anchor_set,
      .mode = mode,
  });
  std::string output = "Context freshness:\n";
  output += "  mode=" + ava::agent::to_string(mode) + "\n";
  output += "  model=" + model.provider_id + "/" + model.model_id + "\n";
  output += "  project_trust=" + std::string(to_string(project_trust)) +
             " project_resources=" + (resource_policy.include_project_resources ? std::string("enabled") : std::string("skipped")) + "\n";
  if (prompt_matches)
  {
    output += "  base_prompt=";
    if (base_prompt.from_override)
    {
      output += "override";
      if (base_prompt.source_path)
        output += " path=" + base_prompt.source_path->string() + " " +
                  context_file_status(*base_prompt.source_path, base_prompt.byte_count, base_prompt.content_fingerprint);
    }
    else
    {
      output += "builtin";
    }
    output += " bytes=" + std::to_string(base_prompt.byte_count) + "\n";
  }
  output += "  context_sources=" + std::to_string(context_sources.size()) + "\n";
  auto const system_prompt_sources = freshness_source_count(freshness_sources, runtime::FreshnessSourceKind::SystemPrompt) +
                                     freshness_source_count(freshness_sources, runtime::FreshnessSourceKind::AppendSystemPrompt);
  output += "  system_prompt_sources=" + std::to_string(system_prompt_sources) + "\n";
  auto const prompt_command_sources = freshness_source_count(freshness_sources, runtime::FreshnessSourceKind::PromptCommand);
  output += "  prompt_commands=" + std::to_string(prompt_command_sources) + "\n";
  auto const skill_sources = freshness_source_count(freshness_sources, runtime::FreshnessSourceKind::Skill);
  output += "  skills=" + std::to_string(skill_sources) + "\n";
  auto const plugin_sources = freshness_source_count(freshness_sources, runtime::FreshnessSourceKind::PluginManifest) +
                              freshness_source_count(freshness_sources, runtime::FreshnessSourceKind::PluginPrompt) +
                              freshness_source_count(freshness_sources, runtime::FreshnessSourceKind::PluginSkill);
  output += "  plugin_sources=" + std::to_string(plugin_sources) + "\n";
  output += "  lsp_status=" + lsp_provider_status_text(lsp_inspection) + " lsp_configs=" + std::to_string(configured_lsp_config_count(lsp_inspection.configs)) +
            " lsp_servers=" + std::to_string(lsp_inspection.server_count) + " lsp_errors=" + std::to_string(lsp_inspection.error_count) +
            " lsp_builtins_enabled=" + std::to_string(enabled_builtin_lsp_count(lsp_inspection)) +
            " lsp_builtins_available=" + std::to_string(available_builtin_lsp_count(lsp_inspection)) + "\n";

  bool matched_source = false;
  for (auto const& source : context_sources)
  {
    if (!context_source_matches_query(source, trimmed_query))
      continue;
    matched_source = true;
    output += "  " + ava::context::to_string(source.source_type) + "  " + source.path.string() + "  loaded_bytes=" + std::to_string(source.byte_count) + "  " +
              context_file_status(source.path, source.byte_count, source.content_fingerprint) + '\n';
  }
  bool matched_freshness_source = false;
  for (auto const& source : freshness_sources)
  {
    if (!freshness_source_matches_query(source, trimmed_query))
      continue;
    matched_freshness_source = true;
    output += "  " + std::string(freshness_source_kind_text(source.kind)) + "  " + source.scope + "  ";
    if (!source.source_id.empty())
    {
      output += sanitize_inline_text(source.source_id);
      if (!source.name.empty() && source.name != source.source_id)
        output += "/" + sanitize_inline_text(source.name);
    }
    else
    {
      output += sanitize_inline_text(source.name);
    }
    output += "  " + freshness_source_status_text(source) + '\n';
  }
  bool matched_lsp_config = false;
  for (auto const& diagnostic : lsp_inspection.configs)
  {
    bool const visible = diagnostic.error.has_value() || (!trimmed_query.empty() && lsp_config_matches_query(diagnostic, trimmed_query));
    if (!visible || !lsp_config_matches_query(diagnostic, trimmed_query))
      continue;
    matched_lsp_config = true;
    output += "  lsp_config  " + sanitize_inline_text(diagnostic.scope) + "  " + lsp_config_status_text(diagnostic) + '\n';
  }
  bool matched_lsp_builtin = false;
  if (trimmed_query.empty() || contains_ascii_case_insensitive("lsp", trimmed_query) || contains_ascii_case_insensitive("builtin", trimmed_query))
  {
    for (auto const& builtin : lsp_inspection.builtin_servers)
    {
      if (!trimmed_query.empty() && !contains_ascii_case_insensitive(builtin.id, trimmed_query) && !contains_ascii_case_insensitive("lsp", trimmed_query) &&
          !contains_ascii_case_insensitive("builtin", trimmed_query))
      {
        continue;
      }
      matched_lsp_builtin = true;
      output += "  lsp_builtin  id=" + builtin.id + " status=" + std::string(ava::lsp::to_string(builtin.status)) + " reason=" + builtin.reason + '\n';
    }
  }
  if (!resource_policy.include_project_resources)
  {
    ava::lsp::ConfiguredLspConfigDiagnostic skipped_project{
        .scope = "project",
        .path = project_lsp_config,
        .exists = path_exists_for_status(project_lsp_config),
    };
    bool const visible = !trimmed_query.empty() && lsp_config_matches_query(skipped_project, trimmed_query);
    if (visible)
    {
      matched_lsp_config = true;
      output += "  lsp_config  project  " + project_lsp_config.string() + "  status=skipped reason=project_resources_skipped\n";
    }
  }
  if (!matched_source && !matched_freshness_source && !matched_lsp_config && !matched_lsp_builtin && !prompt_matches && !trimmed_query.empty())
  {
    add_output(result, "No context sources matching: " + sanitize_inline_text(trimmed_query));
    return result;
  }
  add_output(result, std::move(output));
  return result;
}

ava::core::Result<CommandResult> run_stats_command(runtime::session_ts& unlocked_session)
{
  CommandResult result;
  result.handled = true;
  auto entries = load_runtime_entries(unlocked_session);
  if (!entries)
  {
    add_output(result, entries.error().format());
    return result;
  }
  auto stats = ava::session::compute_session_stats(*entries);
  if (!stats)
  {
    add_output(result, stats.error().format());
    return result;
  }
  add_output(result, format_session_stats_text(unlocked_session, *stats));
  return result;
}

}  // namespace ava::app
