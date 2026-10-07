#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_reload.h"
#include "ava/app/command_trust.h"
#include "ava/app/commands.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_prompt.h"
#include "ava/app/tui/display_reload.h"
#include "ava/config/model_config.h"
#include "ava/session/compaction.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

struct ReloadReportRow
{
  std::string name;
  std::string status;
  std::vector<std::pair<std::string, std::string>> details;
  bool blocks_following_prompt_reload = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

std::string reload_supported_targets()
{
  return "all, theme, models, prompts, trust, compaction, keybindings, auth, providers, permissions, lsp, mcp, plugins";
}

void append_reload_detail(ReloadReportRow& row, std::string key, std::string value)
{
  row.details.push_back({std::move(key), sanitize_inline_text(std::move(value))});
}

ReloadReportRow reload_error_row(std::string name, ava::core::Error const& error)
{
  ReloadReportRow row{.name = std::move(name), .status = "error", .details = {}};
  append_reload_detail(row, "error", error.format());
  return row;
}

std::string format_reload_report(std::vector<ReloadReportRow> const& rows)
{
  std::string output = "Reload report:";
  for (auto const& row : rows)
  {
    output += "\n  " + row.name + ": " + row.status;
    for (auto const& detail : row.details)
      output += "\n    " + detail.first + ": " + detail.second;
  }
  return output;
}

std::string normalize_reload_target(std::string_view target)
{
  if (target.empty() || target == "all")
    return "all";
  if (target == "theme" || target == "themes" || target == "display")
    return "display";
  if (target == "model" || target == "models")
    return "models";
  if (target == "prompt" || target == "prompts" || target == "context" || target == "contexts")
    return "prompts";
  if (target == "trust" || target == "project" || target == "projects")
    return "trust";
  if (target == "compact" || target == "compaction")
    return "compaction";
  if (target == "keybindings" || target == "keybinds" || target == "keys")
    return "keybindings";
  if (target == "auth" || target == "credentials")
    return "auth";
  if (target == "providers" || target == "provider")
    return "providers";
  if (target == "permission" || target == "permissions")
    return "permissions";
  if (target == "lsp" || target == "language-server" || target == "language-servers")
    return "lsp";
  if (target == "mcp")
    return "mcp";
  if (target == "plugin" || target == "plugins")
    return "plugins";
  return {};
}

ReloadReportRow reload_display_settings(runtime::session_ts& unlocked_session)
{
  auto const paths = runtime::session_ts::rat(unlocked_session)->paths();
  auto details = reload_tui_display_settings(paths);
  if (!details)
    return reload_error_row("display", details.error());
  ReloadReportRow row{.name = "display", .status = "loaded", .details = {}};
  for (auto& [key, value] : *details)
    append_reload_detail(row, std::move(key), std::move(value));
  return row;
}

ReloadReportRow reload_model_settings(runtime::session_ts& unlocked_session)
{
  auto const paths = runtime::session_ts::rat(unlocked_session)->paths();
  auto registry = ava::config::load_model_registry(paths);
  if (!registry)
    return reload_error_row("models", registry.error());
  std::string scoped_cycle;
  std::string active_model;
  {
    SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
    session_w->model_selection().scoped_model_cycle = registry->scoped_model_cycle;
    scoped_cycle = session_w->scoped_model_cycle() ? "configured" : "not configured";
    active_model = session_w->model().provider_id + "/" + session_w->model().model_id + " (unchanged)";
  }
  if (auto refreshed = runtime::Session::refresh_parent_configuration(unlocked_session); !refreshed)
    return reload_error_row("models", refreshed.error());
  ReloadReportRow row{.name = "models", .status = "loaded", .details = {}};
  append_reload_detail(row, "config", paths.models_file.string());
  append_reload_detail(row, "models", std::to_string(registry->models.size()));
  append_reload_detail(row, "scoped_cycle", scoped_cycle);
  append_reload_detail(row, "active_model", active_model);
  return row;
}

ReloadReportRow reload_prompt_settings(runtime::session_ts& unlocked_session)
{
  auto snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::tuple{session_r->paths(),
                      session_r->model(),
                      session_r->mode(),
                      session_r->workspace_dir(),
                      session_r->current_dir(),
                      project_resources_trusted(session_r->project_trust()),
                      session_r->prompt_overrides(),
                      session_r->selected_primary_agent()};
  }();
  auto& [paths, model, mode, workspace_dir, current_dir, project_trusted, prompt_overrides, selected_primary_agent] = snapshot;
  auto prompt_state =
      runtime::load_runtime_prompt_state(paths, model, mode, workspace_dir, current_dir, project_trusted, prompt_overrides, selected_primary_agent);
  if (!prompt_state)
    return reload_error_row("prompts", prompt_state.error());
  if (auto refreshed = runtime::Session::apply_prompt_state_and_refresh(unlocked_session, std::move(*prompt_state)); !refreshed)
    return reload_error_row("prompts", refreshed.error());
  std::tuple<bool, std::size_t, std::size_t, std::string> details;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    details = {project_resources_trusted(session_r->project_trust()), session_r->context_sources().size(), session_r->freshness_sources().size(),
               session_r->base_prompt().from_override ? std::string("override")
               : session_r->base_prompt().source_path ? session_r->base_prompt().source_path->string()
                                                      : std::string("built-in")};
  }
  auto& [resources_enabled, context_source_count, freshness_source_count, base_prompt] = details;
  ReloadReportRow row{.name = "prompts", .status = "loaded", .details = {}};
  append_reload_detail(row, "project_resources", resources_enabled ? "enabled" : "skipped");
  append_reload_detail(row, "context_sources", std::to_string(context_source_count));
  append_reload_detail(row, "freshness_sources", std::to_string(freshness_source_count));
  append_reload_detail(row, "base_prompt", base_prompt);
  return row;
}

ReloadReportRow reload_trust_settings(runtime::session_ts& unlocked_session)
{
  auto const trust_source = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->paths(), session_r->workspace_dir()};
  }();
  auto const disk_snapshot = load_project_trust_state(trust_source.first, trust_source.second);
  bool const disk_is_untrusted = !project_resources_trusted(disk_snapshot);

  auto applied = apply_project_trust_operation(unlocked_session, ProjectTrustOperation::Reload);
  if (!applied)
  {
    auto row = reload_error_row("trust", applied.error());
    row.blocks_following_prompt_reload = disk_is_untrusted;
    if (disk_is_untrusted)
      append_reload_detail(row, "authority_state", "reopen required; stale prompt reload skipped");
    return row;
  }
  if (applied->prompt_warning)
  {
    auto row = reload_error_row("trust", *applied->prompt_warning);
    row.blocks_following_prompt_reload = true;
    append_reload_detail(row, "trust_file", applied->state.trust_file.string());
    append_reload_detail(row, "authority_state", "project authority removed with fail-closed empty prompt");
    return row;
  }

  ReloadReportRow row{.name = "trust", .status = "loaded", .details = {}, .blocks_following_prompt_reload = !project_resources_trusted(applied->state)};
  append_reload_detail(row, "trust_file", applied->state.trust_file.string());
  append_reload_detail(row, "decision", std::string(to_string(applied->state.decision)));
  append_reload_detail(row, "project_resources", project_resources_trusted(applied->state) ? "enabled" : "skipped");
  if (!applied->state.diagnostic.empty())
    append_reload_detail(row, "diagnostic", applied->state.diagnostic);
  return row;
}

ReloadReportRow reload_compaction_settings(runtime::session_ts& unlocked_session)
{
  auto const paths = runtime::session_ts::rat(unlocked_session)->paths();
  auto loaded_config = ava::session::load_compaction_config(paths);
  if (!loaded_config)
    return reload_error_row("compaction", loaded_config.error());
  auto config = resolve_compaction_config(unlocked_session, std::move(*loaded_config));
  if (!config)
    return reload_error_row("compaction", config.error());
  ReloadReportRow row{.name = "compaction", .status = "validated", .details = {}};
  append_reload_detail(row, "config", paths.compaction_file.string());
  append_reload_detail(row, "provider", config->provider_id);
  append_reload_detail(row, "model", config->model_id);
  append_reload_detail(row, "auto_threshold_tokens", std::to_string(config->auto_threshold_tokens));
  append_reload_detail(row, "auto_threshold_percent", std::to_string(config->auto_threshold_percent));
  append_reload_detail(
      row, "effective_threshold_tokens",
      std::to_string(ava::session::effective_auto_threshold_tokens(*config, runtime::session_ts::rat(unlocked_session)->model().context_window_tokens)));
  append_reload_detail(row, "keep_recent_tokens", std::to_string(config->keep_recent_tokens));
  append_reload_detail(row, "keep_recent_turns", std::to_string(config->keep_recent_turns));
  append_reload_detail(row, "keep_recent_messages", std::to_string(config->keep_recent_messages));
  append_reload_detail(row, "max_summary_bytes", std::to_string(config->max_summary_bytes));
  return row;
}

ReloadReportRow keybindings_reload_row(runtime::session_ts const& unlocked_session)
{
  auto const paths = runtime::session_ts::crat(unlocked_session)->paths();
  ReloadReportRow row{.name = "keybindings", .status = "tui-runtime", .details = {}};
  append_reload_detail(row, "config", (paths.ava_config_dir / "keybinds.json").string());
  append_reload_detail(row, "note", "interactive TUI reloads keybindings live; restart non-TTY sessions after edits");
  return row;
}

ReloadReportRow restart_required_reload_row(std::string name, std::string reason, std::vector<std::pair<std::string, std::filesystem::path>> paths)
{
  ReloadReportRow row{.name = std::move(name), .status = "restart-required", .details = {}};
  append_reload_detail(row, "reason", std::move(reason));
  for (auto const& path : paths)
    append_reload_detail(row, path.first, path.second.string());
  return row;
}

std::vector<ReloadReportRow> reload_report_rows_for_target(runtime::session_ts& unlocked_session, std::string const& target)
{
  auto const path_snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->paths(), session_r->workspace_dir()};
  }();
  auto const& [paths, workspace_dir] = path_snapshot;
  auto one = [&](std::string const& normalized) -> ReloadReportRow {
    if (normalized == "display")
      return reload_display_settings(unlocked_session);
    if (normalized == "models")
      return reload_model_settings(unlocked_session);
    if (normalized == "trust")
      return reload_trust_settings(unlocked_session);
    if (normalized == "prompts")
      return reload_prompt_settings(unlocked_session);
    if (normalized == "compaction")
      return reload_compaction_settings(unlocked_session);
    if (normalized == "keybindings")
      return keybindings_reload_row(unlocked_session);
    if (normalized == "auth")
    {
      return restart_required_reload_row("auth", "active provider credentials are resolved when a run starts", {{"config", paths.auth_file}});
    }
    if (normalized == "providers")
    {
      return restart_required_reload_row("providers", "provider catalog composition is fixed at process startup and requires a restart",
                                         {{"config", paths.providers_file}});
    }
    if (normalized == "permissions")
    {
      return restart_required_reload_row(
          "permissions", "active permission policy and session grants are not hot-reloaded",
          {{"global", paths.ava_config_dir / "permission-rules.json"}, {"project", workspace_dir / ".ava" / "permission-rules.json"}});
    }
    if (normalized == "lsp")
    {
      return restart_required_reload_row("lsp", "language-server clients are created for tool calls and should restart with config changes",
                                         {{"global", paths.ava_config_dir / "lsp.json"}, {"project", workspace_dir / ".ava" / "lsp.json"}});
    }
    if (normalized == "mcp")
    {
      return restart_required_reload_row("mcp", "running MCP server processes are not restarted by /reload",
                                         {{"global", paths.ava_config_dir / "mcp.json"}, {"project", workspace_dir / ".ava" / "mcp.json"}});
    }
    return restart_required_reload_row("plugins", "plugin discovery and process state are not hot-reloaded",
                                       {{"global", paths.ava_config_dir / "plugins"},
                                        {"project", workspace_dir / ".ava" / "plugins"},
                                        {"state", paths.ava_state_dir / "plugin-enablement.json"}});
  };

  if (target != "all")
    return {one(target)};

  std::vector<ReloadReportRow> rows;
  rows.push_back(one("display"));
  rows.push_back(one("models"));
  auto trust = one("trust");
  bool const skip_prompt_reload = trust.blocks_following_prompt_reload;
  rows.push_back(std::move(trust));
  if (skip_prompt_reload)
  {
    ReloadReportRow prompt_row{.name = "prompts", .status = "included-with-trust", .details = {}};
    append_reload_detail(prompt_row, "authority_state", "no later prompt reload was attempted");
    rows.push_back(std::move(prompt_row));
  }
  else
  {
    rows.push_back(one("prompts"));
  }
  rows.push_back(one("compaction"));
  rows.push_back(one("keybindings"));
  rows.push_back(one("auth"));
  rows.push_back(one("providers"));
  rows.push_back(one("permissions"));
  rows.push_back(one("lsp"));
  rows.push_back(one("mcp"));
  rows.push_back(one("plugins"));
  return rows;
}

}  // namespace

ava::core::Result<CommandResult> run_reload_command(runtime::session_ts& unlocked_session, std::string_view argument)
{
  auto const args = split_command_arguments(argument);
  if (args.size() > 1)
    return handled_text("unsupported reload target: " + std::string(argument) + "\nsupported: " + reload_supported_targets());

  auto const raw_target = args.empty() ? std::string{} : args.front();
  auto const target = normalize_reload_target(raw_target);
  if (target.empty())
    return handled_text("unsupported reload target: " + raw_target + "\nsupported: " + reload_supported_targets());
  return handled_text(format_reload_report(reload_report_rows_for_target(unlocked_session, target)));
}

}  // namespace ava::app
