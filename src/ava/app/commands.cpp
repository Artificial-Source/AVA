#include "sys.h"
#include "ava/app/command_connect.h"
#include "ava/app/command_format.h"
#include "ava/app/command_help.h"
#include "ava/app/command_jobs.h"
#include "ava/app/command_mcp.h"
#include "ava/app/command_models.h"
#include "ava/app/command_permissions.h"
#include "ava/app/command_plugins.h"
#include "ava/app/command_registry.h"
#include "ava/app/command_reload.h"
#include "ava/app/command_sessions.h"
#include "ava/app/command_tools.h"
#include "ava/app/command_trust.h"
#include "ava/app/commands.h"
#include "ava/app/plugin_event_hooks.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/tui/configuration_commands.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/tool_permission.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/static_resources.h"
#include "ava/permissions/permission.h"
#include "ava/context/skill_loader.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace ava::app {
namespace {

bool starts_with_command(std::string_view line, std::string_view command) noexcept
{
  return line == command || (line.starts_with(command) && line.size() > command.size() && line[command.size()] == ' ');
}

std::string_view command_token(std::string_view line) noexcept
{
  auto const end = line.find_first_of(" \t\r\n");
  return line.substr(0, end == std::string_view::npos ? line.size() : end);
}

bool is_shell_helper_command(std::string_view line) noexcept
{
  return line.starts_with('!');
}

std::string shell_helper_argument(std::string_view line)
{
  if (!is_shell_helper_command(line))
    return {};
  line.remove_prefix(line.starts_with("!!") ? 2 : 1);
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
    line.remove_prefix(1);
  return std::string(line);
}

CommandResult handled_prompt(std::string command, std::string source, std::string message)
{
  CommandResult result;
  result.handled = true;
  result.prompt_command = std::move(command);
  result.prompt_source = std::move(source);
  result.prompt_message = std::move(message);
  return result;
}

std::string dynamic_command_argument(std::string_view line)
{
  auto const token = command_token(line);
  if (line.size() <= token.size())
    return {};
  auto rest = line.substr(token.size());
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
    rest.remove_prefix(1);
  return std::string(rest);
}

ava::core::Result<std::string> skill_prompt_message(runtime::session_ts& unlocked_session, CommandRequest const& request, CommandRegistryEntry const& entry)
{
  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  CRITICAL_AREA_BEGIN_R(session);
  auto plugin_diagnostics =
      ava::plugin::collect_plugin_diagnostics(resource_policy.plugin_discovery, resource_policy.plugin_enablement_file, session_r->workspace_dir());
  auto loaded = ava::context::load_skills(ava::context::SkillLoadOptions{
      .workspace_root = session_r->workspace_dir(),
      .global_skill_dirs = resource_policy.global_skill_dirs,
      .project_skill_dirs = resource_policy.project_skill_dirs,
      .declared_skill_files = ava::plugin::declared_plugin_skill_files(plugin_diagnostics),
      .include_project_skills = resource_policy.include_project_resources,
  });
  CRITICAL_AREA_END_R(session);

  auto const match = std::ranges::find_if(loaded.skills, [&](ava::context::LoadedSkill const& skill) { return skill.name == entry.skill_name; });
  if (match == loaded.skills.end())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "skill not found");
    error.with_context("skill", entry.skill_name);
    return std::unexpected(std::move(error));
  }

  auto context = make_tool_context(unlocked_session, request.permission_resolver);
  context.permission_tool_name = "skill";
  context.current_tool_name = "skill";
  if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::SkillLoad, match->path, match->name, "skill",
                                                      "skill loading requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  auto sampled_files = ava::context::sample_skill_files(match->directory);
  return ava::context::format_loaded_skill_for_tool(*match, sampled_files);
}

ava::core::Result<CommandResult> run_registry_command(runtime::session_ts& unlocked_session, CommandRequest request, CommandRegistryEntry const& entry)
{
  if (!entry.enabled)
    return handled_text(entry.command + " is disabled: " + entry.disabled_reason);
  auto const argument = dynamic_command_argument(request.command);
  switch (entry.kind)
  {
    case UnifiedCommandKind::Backend:
      return CommandResult{};
    case UnifiedCommandKind::PromptTemplate: {
      auto prompt = expand_prompt_command_template(entry.template_text, argument);
      if (!prompt)
        return std::unexpected(std::move(prompt.error()));
      return handled_prompt(entry.command, to_string(entry.source), std::move(*prompt));
    }
    case UnifiedCommandKind::SkillPrompt: {
      auto prompt = skill_prompt_message(unlocked_session, request, entry);
      if (!prompt)
        return std::unexpected(std::move(prompt.error()));
      return handled_prompt(entry.command, to_string(entry.source), std::move(*prompt));
    }
    case UnifiedCommandKind::McpPrompt: {
      auto prompt = mcp_prompt_message(unlocked_session, request, entry, argument);
      if (!prompt)
        return std::unexpected(std::move(prompt.error()));
      return handled_prompt(entry.command, to_string(entry.source), std::move(*prompt));
    }
    case UnifiedCommandKind::PluginCommand: {
      auto delegated = request;
      auto args = argument.empty() ? std::string("{}") : argument;
      delegated.command = "/plugin run " + entry.plugin_id + " " + entry.plugin_command_name + " " + args;
      return run_plugin_command(unlocked_session, delegated);
    }
  }
  return CommandResult{};
}

}  // namespace

bool is_backend_command(std::string_view line) noexcept
{
  if (is_shell_helper_command(line))
    return true;
  return find_command_catalog_entry(line) != nullptr;
}

bool is_backend_command_1(std::string_view line, runtime::session_ts& unlocked_session)
{
  if (is_backend_command(line))
    return true;
  return command_registry_contains(unlocked_session, line);
}

// Dispatch a normalized backend command against unlocked_session and return its frontend-facing result.
//
// Session state remains write-locked while invoking legacy command handlers. The lock is released before
// invoking a session-aware handler that acquires its own access guard.
ava::core::Result<CommandResult> run_command(runtime::session_ts& unlocked_session, CommandRequest request)
{
  CommandResult result;
  if (request.command.empty())
    return result;

  if (is_shell_helper_command(request.command))
  {
    auto const shell_command = shell_helper_argument(request.command);
    if (shell_command.empty())
      return handled_text(missing_argument("!<command> or !!<command>"));
    request.command = "/bash " + shell_command;
  }

  auto const* entry = find_command_catalog_entry(request.command);
  if (!entry)
  {
    auto const token = command_token(request.command);
    auto registry = load_command_registry(unlocked_session, CommandRegistryOptions{.include_builtins = false,
                                                                                   .include_prompt_commands = true,
                                                                                   .include_skills = true,
                                                                                   .include_plugin_commands = true,
                                                                                   .include_mcp_prompts = token.starts_with("/mcp:"),
                                                                                   .permission_resolver = request.permission_resolver,
                                                                                   .cancel_requested = request.cancel_requested});
    if (auto const* registry_entry = find_command_registry_entry(registry, request.command))
      return run_registry_command(unlocked_session, std::move(request), *registry_entry);
    if (!token.starts_with("/skill:") && !token.starts_with("/mcp:") && !token.starts_with("/plugin:"))
    {
      registry = load_command_registry(unlocked_session, CommandRegistryOptions{.include_builtins = false,
                                                                                .include_prompt_commands = false,
                                                                                .include_skills = false,
                                                                                .include_plugin_commands = false,
                                                                                .include_mcp_prompts = true,
                                                                                .permission_resolver = request.permission_resolver,
                                                                                .cancel_requested = request.cancel_requested});
      if (auto const* registry_entry = find_command_registry_entry(registry, request.command))
        return run_registry_command(unlocked_session, std::move(request), *registry_entry);
    }

    if (token.starts_with("/skill:") || token.starts_with("/mcp:") || token.starts_with("/plugin:"))
    {
      if (!registry.diagnostics.empty())
        return handled_text(registry.diagnostics.front().message);
      return handled_text("command not found: " + std::string(token));
    }
    return result;
  }
  request.command = normalize_command_line(request.command, *entry);

  if (!entry->enabled)
  {
    return handled_text(entry->command + " is disabled: " + entry->disabled_reason);
  }

  auto plugin_observer_options = plugin_event_observer_options(unlocked_session, request.permission_resolver);
  plugin_observer_options.cancel_requested = request.cancel_requested;
  request.event_sink = make_plugin_event_observer_sink(std::move(plugin_observer_options), std::move(request.event_sink));

  if (request.command == "/quit" || request.command == "/exit")
  {
    result.handled = true;
    result.quit = true;
    return result;
  }
  if (request.command == "/help")
  {
    return handled_text(command_help_text(request.hotkeys));
  }
  if (auto configuration_result = run_tui_configuration_command(unlocked_session, request.command, request.hotkeys))
    return std::move(*configuration_result);
  if (request.command == "/settings")
  {
    return handled_text("Settings are shown as a TUI view. Use /theme, /images, /image-width, and /cursor to persist display settings.");
  }
  if (starts_with_command(request.command, "/details"))
  {
    return handled_text(
        "Tool cards default to Rich. In the TUI, exact /details or Ctrl+O toggles Rich and Expanded; use /details compact, /details rich, or "
        "/details expanded to select a view explicitly.");
  }
  if (request.command == "/sidebar")
  {
    return handled_text("The current session overview is an interactive TUI view. Use /sidebar inside the TUI to open it.");
  }
  if (request.command == "/overview")
  {
    return handled_text("The startup resources overview is an interactive TUI view. Use /overview inside the TUI to toggle it.");
  }
  if (starts_with_command(request.command, "/search"))
  {
    return handled_text(
        "Transcript search is available only inside the interactive TUI. Use /search [query] there to find currently rendered transcript items.");
  }
  if (starts_with_command(request.command, "/tool"))
  {
    return handled_text(
        "Tool detail inspection is available inside the interactive TUI. Use /tool [query] to toggle the latest or matching card between its inherited "
        "non-expanded view and Expanded, /details to toggle Rich and Expanded globally, or /copy tool [query] to copy safe tool details.");
  }
  if (starts_with_command(request.command, "/diff"))
  {
    return handled_text(
        "Tool diff inspection is available inside the interactive TUI. Use /diff [query] to show the latest or matching unified diff, or /copy diff [query] to "
        "copy it.");
  }
  if (starts_with_command(request.command, "/copy"))
  {
    auto const argument = command_argument(request.command, "/copy");
    auto const copy_args = split_command_arguments(argument);
    auto const target = copy_args.empty() ? std::string{} : copy_args.front();
    // Exact first token parsing: "user" opens the user-turn picker in the TUI.
    // Aliases tools/diffs/permissions remain accepted for those targets only.
    if (!target.empty() && target != "user" && target != "tool" && target != "tools" && target != "diff" && target != "diffs" && target != "permission" &&
        target != "permissions")
    {
      return handled_text("unsupported copy target: " + target + "\nsupported: user [query], tool [query], diff [query], permission [query]");
    }
    return handled_text(
        "Clipboard copy is available inside the interactive TUI. Use /copy for the latest AVA message, /copy user [query] to pick a public user turn, /copy "
        "tool "
        "[query] for tool details, /copy diff [query] for unified diffs, or /copy permission [query] for permission audit details.");
  }
  if (request.command == "/thinking" || request.command == "/thinking details")
  {
    return handled_text(
        "Thinking visibility is a TUI display toggle. It does not change provider reasoning mode. Bare /thinking shows or hides all inline thinking. In the "
        "TUI, "
        "/thinking details toggles the latest completed long thinking block between its bounded preview and full text; mouse-click the Thinking: header for "
        "the "
        "same per-item expand/collapse. Expansion is presentation-only and is not persisted across reload.");
  }
  if (starts_with_command(request.command, "/attach"))
  {
    return handled_text(
        "Image attachment import is available inside the interactive TUI with /attach <path>. In headless RPC, send prompt attachments with the attachments "
        "array.");
  }
  if (starts_with_command(request.command, "/reload"))
  {
    return run_reload_command(unlocked_session, command_argument(request.command, "/reload"));
  }
  if (starts_with_command(request.command, "/models"))
  {
    return run_models_command(unlocked_session, command_argument(request.command, "/models"));
  }
  if (starts_with_command(request.command, "/providers"))
  {
    return run_providers_command(unlocked_session, command_argument(request.command, "/providers"));
  }
  if (request.command == "/scoped-models")
  {
    return handled_text("Scoped model cycling is a TUI selector. In the TUI, /scoped-models opens the model cycle list and Ctrl+S persists it to models.json.");
  }
  if (starts_with_command(request.command, "/connect"))
  {
    return run_connect_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/mcp"))
  {
    return run_mcp_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/plugins"))
  {
    return run_plugins_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/plugin"))
  {
    return run_plugin_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/trust"))
  {
    return run_trust_command(unlocked_session, command_argument(request.command, "/trust"));
  }
  if (starts_with_command(request.command, "/permissions"))
  {
    return run_permissions_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/sessions"))
  {
    return run_sessions_command(unlocked_session, command_argument(request.command, "/sessions"));
  }
  if (starts_with_command(request.command, "/jobs"))
  {
    return run_jobs_command_1(unlocked_session, command_argument(request.command, "/jobs"));
  }
  if (request.command == "/recover-persistence")
  {
    return run_recover_persistence_command(unlocked_session);
  }
  if (request.command == "/fork-from" || starts_with_command(request.command, "/fork-from"))
  {
    return handled_text(
        "Fork-from is available inside the interactive TUI. Use /fork-from to open the public user-turn picker, or /fork [name] to fork at the latest entry.");
  }
  if (starts_with_command(request.command, "/fork"))
  {
    return run_fork_command(unlocked_session, command_argument(request.command, "/fork"));
  }
  if (starts_with_command(request.command, "/clone"))
  {
    return run_clone_command(unlocked_session, command_argument(request.command, "/clone"));
  }
  if (starts_with_command(request.command, "/new"))
  {
    return run_new_session_command(unlocked_session, command_argument(request.command, "/new"));
  }
  if (starts_with_command(request.command, "/resume"))
  {
    return run_resume_command(unlocked_session, command_argument(request.command, "/resume"));
  }
  if (starts_with_command(request.command, "/name"))
  {
    return run_name_command(unlocked_session, command_argument(request.command, "/name"));
  }
  if (starts_with_command(request.command, "/labels"))
  {
    return run_labels_command(unlocked_session, command_argument(request.command, "/labels"));
  }
  if (request.command == "/mode")
  {
    return run_mode_command(unlocked_session);
  }
  if (starts_with_command(request.command, "/context"))
  {
    return run_context_command(unlocked_session, command_argument(request.command, "/context"));
  }
  if (request.command == "/stats" || request.command == "/status")
  {
    return run_stats_command(unlocked_session);
  }
  if (starts_with_command(request.command, "/compact"))
  {
    return run_compact_command(unlocked_session, request);
  }
  if (starts_with_command(request.command, "/import"))
  {
    return run_import_command(unlocked_session, command_argument(request.command, "/import"));
  }
  if (starts_with_command(request.command, "/export"))
  {
    return run_export_command(unlocked_session, request);
  }

  if (entry->hint.empty() && starts_with_command(request.command, entry->command))
  {
    return handled_text(missing_argument(entry->command));
  }

  if (request.command == "/glob")
  {
    return handled_text(missing_argument("/glob <pattern>"));
  }
  if (request.command == "/find")
  {
    return handled_text(missing_argument("/find <pattern>"));
  }
  if (request.command == "/grep")
  {
    return handled_text(missing_argument("/grep <text> [glob]"));
  }
  if (request.command == "/read")
  {
    return handled_text(missing_argument("/read <path>"));
  }
  if (request.command == "/write")
  {
    return handled_text(missing_argument("/write <path> <text>"));
  }
  if (request.command == "/bash")
  {
    return handled_text(missing_argument("/bash <command>"));
  }

  return run_tool_command(unlocked_session, request);
}

}  // namespace ava::app
