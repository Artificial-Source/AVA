#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "ava/app/command_jobs.h"
#include "ava/app/command_registry.h"
#include "ava/app/commands.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/Session.h"
#include "ava/agent/job_control.h"
#include "ava/agent/subagent_coordinator.h"
#include "ava/session/session_store.h"
#include "ava/session/subagent_job_history.h"
#include "ava/permissions/permission.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace {

using namespace ava::tests;

ava::app::runtime::session_ts open_test_session(std::filesystem::path const& root, std::filesystem::path const& workspace)
{
  auto paths = app_test_paths(root);
  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "command registry test opens runtime session");
  return std::move(*unlocked_session_result);
}

ava::permissions::PermissionResolver allow_all_permissions(std::vector<ava::permissions::Operation>* operations = nullptr)
{
  return [operations](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    if (operations)
      operations->push_back(prompt.operation);
    return ava::permissions::PermissionResolution::Allow;
  };
}

ava::app::CommandRegistryEntry const* find_entry(ava::app::CommandRegistry const& registry, std::string_view command)
{
  return ava::app::find_command_registry_entry(registry, command);
}

bool has_diagnostic(ava::app::CommandRegistry const& registry, std::string_view command, std::string_view message)
{
  return std::ranges::any_of(registry.diagnostics, [&](ava::app::CommandRegistryDiagnostic const& diagnostic) {
    return diagnostic.command == command && diagnostic.message.find(message) != std::string::npos;
  });
}

void test_prompt_commands_load_project_global_and_expand_arguments()
{
  auto const root = create_empty_root("command-registry-prompts");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ScopedEnvVar home("HOME", (root / "home").string());
  ScopedEnvVar xdg_config("XDG_CONFIG_HOME", paths.config_home.string());
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(),
         trusted ? "command registry prompt test trusts project" : "command registry prompt test trusts project: " + trusted.error().format());

  write_app_test_file(paths.ava_config_dir / "commands" / "review.md", "---\ndescription: Global review\n---\nGlobal $1\n");
  write_app_test_file(workspace / ".ava" / "commands" / "review.md",
                      "---\ndescription: Project review\nargument-hint: <topic>\n---\nProject $1 $2 $@ $ARGUMENTS ${@:2}\n");
  write_app_test_file(workspace / ".ava" / "commands" / "ship.md", "Ship $$ $1 ${@:2:1}\n");
  write_app_test_file(workspace / ".ava" / "commands" / "defaults.md", "Default ${1:-release} ${2:-notes}\n");

  auto unlocked_session = open_test_session(root, workspace);
  auto registry = ava::app::load_command_registry(unlocked_session, ava::app::CommandRegistryOptions{.include_mcp_prompts = false});
  auto const* review = find_entry(registry, "/review");
  expect(review != nullptr && review->source == ava::app::UnifiedCommandSource::PromptProject && review->description == "Project review" &&
             review->hint == "<topic>",
         "command registry loads project prompt commands before global collisions");
  expect(has_diagnostic(registry, "/review", "command collision"), "command registry records deterministic prompt-command collision diagnostics");

  auto expanded = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/review \"one arg\" two"});
  expect(expanded && expanded->handled && expanded->prompt_message &&
             expanded->prompt_message->find("Project one arg two one arg two \"one arg\" two two") != std::string::npos,
         "prompt command invocation expands positional, all-argument, raw, and slice placeholders safely");
  auto literal = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/ship release notes extra"});
  expect(literal && literal->prompt_message && literal->prompt_message->find("Ship $ release notes") != std::string::npos,
         "prompt command invocation treats $$ as a literal dollar and supports bounded slices");
  auto defaulted = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/defaults"});
  expect(defaulted && defaulted->prompt_message && defaulted->prompt_message->find("Default release notes") != std::string::npos,
         "prompt command invocation applies positional defaults when arguments are missing");
  auto direct_defaults = ava::app::expand_prompt_command_template("Defaults ${1:-seven} ${2:-fallback} ${3:-tail} ${0:-bad} ${x:-bad}", "\"\" custom");
  expect(direct_defaults && *direct_defaults == "Defaults seven custom tail ${0:-bad} ${x:-bad}",
         "prompt command expansion applies defaults for missing or empty args and leaves malformed defaults literal");
}

void test_skill_commands_are_registry_entries_and_permissioned_prompts()
{
  auto const root = create_empty_root("command-registry-skills");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  ScopedEnvVar home("HOME", (root / "home").string());
  ScopedEnvVar xdg_config("XDG_CONFIG_HOME", paths.config_home.string());
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(),
         trusted ? "command registry skill test trusts project" : "command registry skill test trusts project: " + trusted.error().format());
  write_app_test_file(workspace / ".ava" / "skills" / "release" / "SKILL.md",
                      "---\nname: release\ndescription: Prepare release work\n---\nRelease skill body\n");

  auto unlocked_session = open_test_session(root, workspace);
  auto registry = ava::app::load_command_registry(unlocked_session);
  ava::app::CommandRegistryEntry const* entry_skill_release = find_entry(registry, "/skill:release");
  ava::app::CommandRegistryEntry const* entry_release = find_entry(registry, "/release");
  bool namespaced_and_unnamespaced_success = entry_skill_release != nullptr && entry_release != nullptr;
  expect(namespaced_and_unnamespaced_success, "command registry exposes skills as namespaced and unnamespaced command entries");
  if (!namespaced_and_unnamespaced_success)
    Dout(dc::warning,
         "entry_skill_release = " << static_cast<void const*>(entry_skill_release) << ", entry_release = " << static_cast<void const*>(entry_release));

  std::vector<ava::permissions::Operation> operations;
  auto result =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/skill:release", .permission_resolver = allow_all_permissions(&operations)});
  expect(result && result->handled && result->prompt_message && result->prompt_message->find("<skill_content name=\"release\">") != std::string::npos &&
             result->prompt_message->find("Release skill body") != std::string::npos,
         "skill command invocation returns normal prompt content");
  expect(std::ranges::find(operations, ava::permissions::Operation::SkillLoad) != operations.end(), "skill command invocation requests skill-load permission");
}

void test_plugin_commands_are_registry_entries()
{
  auto const root = create_empty_root("command-registry-plugins");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(),
         trusted ? "command registry plugin test trusts project" : "command registry plugin test trusts project: " + trusted.error().format());
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.cmd";
  write_app_test_file(plugin_dir / "plugin.json", app_test_plugin_manifest_json("com.example.cmd", "Command Plugin"));

  auto unlocked_session = open_test_session(root, workspace);
  auto enabled = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins enable com.example.cmd"});
  expect(enabled && enabled->handled, "plugin command registry test enables a project plugin without execution");

  auto registry = ava::app::load_command_registry(unlocked_session);
  auto const* entry = find_entry(registry, "/plugin:com.example.cmd:todo");
  expect(entry != nullptr && entry->source == ava::app::UnifiedCommandSource::PluginCommand && entry->enabled && entry->plugin_id == "com.example.cmd" &&
             entry->plugin_command_name == "todo",
         "command registry exposes enabled plugin command entries with source metadata");
}

void test_mcp_prompts_are_registry_entries_and_permissioned_prompts()
{
  if (std::string_view(AVA_FAKE_MCP_SERVER_PATH).empty())
    return;

  auto const root = create_empty_root("command-registry-mcp");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), trusted ? "command registry MCP test trusts project" : "command registry MCP test trusts project: " + trusted.error().format());
  write_app_test_file(workspace / ".ava" / "mcp.json", app_test_mcp_config_json("demo", "Demo MCP", AVA_FAKE_MCP_SERVER_PATH));

  auto unlocked_session = open_test_session(root, workspace);
  std::vector<ava::permissions::Operation> operations;
  auto registry = ava::app::load_command_registry(
      unlocked_session, ava::app::CommandRegistryOptions{.include_mcp_prompts = true, .permission_resolver = allow_all_permissions(&operations)});
  auto const* entry = find_entry(registry, "/mcp:demo:release-notes");
  expect(entry != nullptr && entry->source == ava::app::UnifiedCommandSource::McpPrompt && entry->mcp_server_id == "demo" &&
             entry->mcp_prompt_name == "release-notes" && !entry->mcp_arguments.empty() && entry->mcp_arguments[0].name == "topic",
         "command registry exposes MCP prompts as command entries with argument metadata");

  auto result = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/mcp:demo:release-notes AVA", .permission_resolver = allow_all_permissions(&operations)});
  expect(result && result->handled && result->prompt_message && result->prompt_message->find("MCP prompt for AVA") != std::string::npos,
         "MCP prompt command invocation returns prompt text from prompts/get");
  auto alias_result = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/release-notes AVA", .permission_resolver = allow_all_permissions(&operations)});
  expect(alias_result && alias_result->handled && alias_result->prompt_message && alias_result->prompt_message->find("MCP prompt for AVA") != std::string::npos,
         "unnamespaced MCP prompt command entries are invokable");
  expect(std::ranges::find(operations, ava::permissions::Operation::McpServerLaunch) != operations.end() &&
             std::ranges::find(operations, ava::permissions::Operation::McpServerConnect) != operations.end() &&
             std::ranges::find(operations, ava::permissions::Operation::McpToolCall) != operations.end(),
         "MCP prompt command invocation stays behind MCP launch, connect, and call permissions");
}

void test_builtin_session_alias_registers_as_current_stats_command()
{
  auto const root = create_empty_root("command-registry-builtin-session-alias");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  auto unlocked_session = open_test_session(root, workspace);
  auto registry = ava::app::load_command_registry(
      unlocked_session, ava::app::CommandRegistryOptions{
                            .include_prompt_commands = false, .include_skills = false, .include_plugin_commands = false, .include_mcp_prompts = false});
  auto const* entry = find_entry(registry, "/session");
  expect(entry != nullptr && entry->command == "/stats" && entry->source == ava::app::UnifiedCommandSource::Builtin &&
             entry->kind == ava::app::UnifiedCommandKind::Backend && std::ranges::find(entry->aliases, "/session") != entry->aliases.end(),
         "command registry exposes /session as the built-in current-session /stats alias");

  expect(ava::app::is_backend_command("/session"), "command catalog classifies /session as a backend slash command");
  auto const* sidebar_entry = find_entry(registry, "/sidebar");
  expect(sidebar_entry != nullptr && sidebar_entry->source == ava::app::UnifiedCommandSource::Builtin &&
             sidebar_entry->kind == ava::app::UnifiedCommandKind::Backend && sidebar_entry->description == "Open the current session overview",
         "command registry exposes the discoverable built-in /sidebar TUI view");
  expect(ava::app::command_help_text().find("/sidebar") != std::string::npos, "command help includes the discoverable /sidebar TUI view");

  auto seeded_stats_usage = ava::app::runtime::session_ts::wat(unlocked_session)
                                ->append_owned(ava::session::SessionEntry{.id = "entry_session_alias_usage",
                                                                          .parent_id = "",
                                                                          .type = ava::session::EntryType::AssistantMessage,
                                                                          .timestamp = "2026-05-02T00:00:00Z",
                                                                          .data_json = "{\"text\":\"usage\",\"usage\":{\"input_tokens\":12,"
                                                                                       "\"output_tokens\":7,\"total_tokens\":19}}"});
  expect(seeded_stats_usage.has_value(), "command registry /session runtime test seeds usage metadata");
  auto stats = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/stats"});
  auto session_alias = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/session"});
  expect(stats && session_alias && stats->handled && session_alias->handled && !stats->output.empty() && !session_alias->output.empty() &&
             session_alias->output[0] == stats->output[0] && session_alias->output[0].find("tokens: input=12 output=7 total=19") != std::string::npos,
         "command dispatcher runs /session with no arguments through the current-session /stats surface");

  auto jobs = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs"});
  auto missing = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs show job_missing"});
  auto invalid_wait = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs wait job_missing nope"});
  auto const job_snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->subagent_coordinator(), session_r->store.session_id()};
  }();
  auto active_wait = ava::app::run_jobs_command(job_snapshot.first, job_snapshot.second, "wait job_missing 10", true);
  auto active_result = ava::app::run_jobs_command(job_snapshot.first, job_snapshot.second, "result job_missing", true);
  auto active_list_arguments = ava::app::active_jobs_command_arguments(" /jobs ");
  auto active_promote_arguments = ava::app::active_jobs_command_arguments("/jobs promote job_1");
  auto unrelated_active_command = ava::app::active_jobs_command_arguments("/jobs-extra promote job_1");
  auto const help = ava::app::command_help_text();
  expect(ava::app::is_backend_command("/jobs") && jobs && jobs->handled && !jobs->output.empty() && jobs->output[0] == "Jobs: none" && missing &&
             !missing->output.empty() && missing->output[0] == "jobs: subagent job not found" && invalid_wait && !invalid_wait->output.empty() &&
             invalid_wait->output[0].find("positive integer") != std::string::npos && active_wait && !active_wait->output.empty() &&
             active_wait->output[0].find("may block") != std::string::npos && active_wait->output[0].find("/jobs show") != std::string::npos && active_result &&
             !active_result->output.empty() && active_result->output[0].find("may block") != std::string::npos && active_list_arguments &&
             active_list_arguments->empty() && active_promote_arguments && *active_promote_arguments == "promote job_1" && !unrelated_active_command &&
             help.find("/jobs") != std::string::npos,
         "slash job controls keep human empty list text, nonblocking active-run actions, and reject wait/result with actionable text");
}

void test_interactive_jobs_human_output_keeps_public_json_contract()
{
  auto const root = create_empty_root("command-registry-jobs-human");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto unlocked_session = open_test_session(root, workspace);
  auto const session_snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->subagent_coordinator(), session_r->store.session_id()};
  }();
  auto const& [coordinator, session_id] = session_snapshot;
  expect(coordinator != nullptr, "interactive jobs human-output test requires a coordinator");
  if (!coordinator)
    return;

  constexpr char kPromptLeak[] = "SECRET_PROMPT_SHOULD_NOT_LEAK_IN_JOBS_OUTPUT";
  // static: captureless BackgroundJobWorker lambda ODR-uses this when building final_text.
  static constexpr char kBoundedSummary[] = "BOUNDED_SUMMARY_UNIQUE_TOKEN";

  // Deterministic completed fixture: list/show must omit terminal summary; result includes it.
  auto completed_start =
      coordinator->start_background(session_id,
                                    ava::agent::BackgroundJobStartOptions{
                                        .title = "Review pull request",
                                        .description = kPromptLeak,
                                        .subagent_type = "general",
                                        .child_session_id = "child_jobs_human_completed",
                                    },
                                    [](ava::agent::BackgroundJobContext const&) {
                                      return ava::agent::BackgroundJobCompletion{
                                          .state = ava::agent::BackgroundJobState::Completed, .final_text = kBoundedSummary, .stop_reason = "completed"};
                                    });
  expect(completed_start.has_value(), "completed fixture starts");
  if (!completed_start)
    return;
  auto completed = coordinator->wait(session_id, completed_start->job.identity.job_id, std::chrono::seconds(2));
  expect(completed && !completed->timed_out && completed->job.execution == ava::agent::SubagentExecutionState::Completed &&
             completed->job.summary == kBoundedSummary,
         completed ? "completed fixture reaches terminal summary state" : "completed fixture reaches terminal summary state: " + completed.error().format());
  if (!completed)
    return;

  auto const completed_id = completed->job.identity.job_id;
  auto completed_list = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs"});
  auto completed_show = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs show " + completed_id});
  auto completed_result = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs result " + completed_id});
  auto completed_result_ordinal = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs result 1"});
  expect(
      completed_list && completed_list->handled && !completed_list->output.empty() &&
          completed_list->output[0].find("Completed · Background · Review pull request") != std::string::npos &&
          completed_list->output[0].find("id " + completed_id) != std::string::npos && completed_list->output[0].find(kBoundedSummary) == std::string::npos &&
          completed_list->output[0].find(kPromptLeak) == std::string::npos && completed_show && completed_show->handled && !completed_show->output.empty() &&
          completed_show->output[0].rfind("Completed · Background · Review pull request", 0) == 0 &&
          completed_show->output[0].find("id " + completed_id) != std::string::npos && completed_show->output[0].find(kBoundedSummary) == std::string::npos &&
          completed_show->output[0].find("result ") == std::string::npos && completed_result && completed_result->handled &&
          !completed_result->output.empty() && completed_result->output[0].find("Completed · Background · Review pull request") != std::string::npos &&
          completed_result->output[0].find("id " + completed_id) != std::string::npos &&
          completed_result->output[0].find(std::string("result ") + kBoundedSummary) != std::string::npos && completed_result_ordinal &&
          completed_result_ordinal->handled && !completed_result_ordinal->output.empty() &&
          completed_result_ordinal->output[0] == "jobs: subagent job not found",
      "interactive list/show omit terminal summary while /jobs result <exact id> includes the bounded summary");

  struct BlockingWorker
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool release = false;

    ava::agent::BackgroundJobCompletion run(ava::agent::BackgroundJobContext const& context)
    {
      std::stop_callback wake_on_stop(context.stop_token, [&] { changed.notify_all(); });
      std::unique_lock lock(mutex);
      started = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release || context.stop_token.stop_requested(); });
      if (context.stop_token.stop_requested())
        return {.state = ava::agent::BackgroundJobState::Canceled, .final_text = {}, .stop_reason = "canceled"};
      return {.state = ava::agent::BackgroundJobState::Completed, .final_text = "unexpected complete", .stop_reason = "completed"};
    }

    bool wait_started()
    {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, std::chrono::seconds(2), [&] { return started; });
    }

    void finish()
    {
      std::lock_guard lock(mutex);
      release = true;
      changed.notify_all();
    }
  };

  auto worker = std::make_shared<BlockingWorker>();
  auto started = coordinator->start_background(session_id,
                                               ava::agent::BackgroundJobStartOptions{
                                                   .title = std::string("Explore\x01repository\nnow"),
                                                   .description = kPromptLeak,
                                                   .subagent_type = "explore",
                                                   .child_session_id = "child_jobs_human_running",
                                               },
                                               [worker](ava::agent::BackgroundJobContext const& context) { return worker->run(context); });
  expect(started && worker->wait_started() && !started->job.display_title.empty() && started->job.display_title.find('\x01') == std::string::npos &&
             started->job.display_title.find('\n') == std::string::npos && started->job.display_title.find("Explore") != std::string::npos &&
             started->job.display_subagent_type == "explore",
         "coordinator stores only a bounded sanitized display title and type on the process-local snapshot");
  if (!started)
    return;

  auto const public_list = ava::agent::public_job_list_json(coordinator->list(session_id));
  auto const public_status = ava::agent::public_job_snapshot_json(*started);
  auto const public_completed = ava::agent::public_job_snapshot_json(*completed);
  auto const public_completed_result = ava::agent::public_job_snapshot_json(*completed, ava::agent::PublicJobContent::IncludeTerminalResult);
  expect(public_list.find("\"schema_version\":1") != std::string::npos && public_list.find("\"title\"") == std::string::npos &&
             public_list.find("display_title") == std::string::npos && public_list.find("Explore") == std::string::npos &&
             public_list.find(kPromptLeak) == std::string::npos && public_list.find(kBoundedSummary) == std::string::npos &&
             public_status.find("\"title\"") == std::string::npos && public_status.find("display_title") == std::string::npos &&
             public_status.find(kPromptLeak) == std::string::npos && public_status.find("\"schema_version\":1") != std::string::npos &&
             public_completed.find(kBoundedSummary) == std::string::npos && public_completed_result.find(kBoundedSummary) != std::string::npos &&
             public_completed_result.find("\"schema_version\":1") != std::string::npos,
         "public job JSON stays schema v1 without title/display fields or prompt leakage, and keeps list/status vs result content policy");

  auto list = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs"});
  expect(list && list->handled && !list->output.empty() && list->output[0].rfind("Jobs (2):", 0) == 0 &&
             list->output[0].find("Running · Background · Explore repository now") != std::string::npos &&
             list->output[0].find("id " + started->job.identity.job_id) != std::string::npos && list->output[0].find("type explore") != std::string::npos &&
             list->output[0].find(kPromptLeak) == std::string::npos && list->output[0].find('\x01') == std::string::npos &&
             list->output[0].find("\"jobs\"") == std::string::npos && list->output[0].find(kBoundedSummary) == std::string::npos,
         "interactive /jobs list is human text with display-only ordinals, sanitized title, and no prompt/summary leak");

  auto show = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs show " + started->job.identity.job_id});
  expect(show && show->handled && !show->output.empty() && show->output[0].rfind("Running · Background · Explore repository now", 0) == 0 &&
             show->output[0].find("id " + started->job.identity.job_id) != std::string::npos && show->output[0].find(kPromptLeak) == std::string::npos,
         "interactive /jobs show keeps a human primary line and exact job id on the secondary details line");

  auto ordinal = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs show 1"});
  expect(ordinal && ordinal->handled && !ordinal->output.empty() && ordinal->output[0] == "jobs: subagent job not found",
         "display ordinals are never accepted as job-control authority");

  auto canceled = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/jobs cancel " + started->job.identity.job_id});
  bool const has_cancel_state =
      canceled && canceled->handled && !canceled->output.empty() &&
      (canceled->output[0].find("Canceled") != std::string::npos || canceled->output[0].find("cancel requested") != std::string::npos);
  // Legitimate race: cancel may still be Running only when cancel_requested is already visible, or already terminal Canceled.
  bool const bare_running_without_cancel = canceled && !canceled->output.empty() && canceled->output[0].find("Running") != std::string::npos &&
                                           canceled->output[0].find("cancel requested") == std::string::npos &&
                                           canceled->output[0].find("Canceled") == std::string::npos;
  expect(has_cancel_state && !bare_running_without_cancel && canceled->output[0].find("Background · Explore repository now") != std::string::npos &&
             canceled->output[0].find("id " + started->job.identity.job_id) != std::string::npos && canceled->output[0].find(kPromptLeak) == std::string::npos,
         "interactive cancel receipt requires cancel requested or terminal Canceled and retains the exact job id");

  auto waited = coordinator->wait(session_id, started->job.identity.job_id, std::chrono::seconds(2));
  bool const terminal =
      waited && !waited->timed_out &&
      (waited->job.execution == ava::agent::SubagentExecutionState::Completed || waited->job.execution == ava::agent::SubagentExecutionState::Failed ||
       waited->job.execution == ava::agent::SubagentExecutionState::Canceled || waited->job.execution == ava::agent::SubagentExecutionState::Interrupted);
  expect(terminal,
         waited ? "cancel fixture reaches a terminal coordinator state" : "cancel fixture reaches a terminal coordinator state: " + waited.error().format());
  worker->finish();
}

void test_project_trust_gates_project_resource_commands()
{
  auto const root = create_empty_root("command-registry-project-trust");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ScopedEnvVar home("HOME", (root / "home").string());
  ScopedEnvVar xdg_config("XDG_CONFIG_HOME", paths.config_home.string());
  write_app_test_file(paths.ava_config_dir / "commands" / "global.md", "Global command $1\n");
  write_app_test_file(paths.ava_config_dir / "APPEND_SYSTEM.md", "Global append instruction.\n");
  write_app_test_file(workspace / "AGENTS.md", "Project AGENTS context still loads while untrusted.\n");
  write_app_test_file(workspace / ".ava" / "SYSTEM.md", "Project system replacement.\n");
  write_app_test_file(workspace / ".ava" / "APPEND_SYSTEM.md", "Project append instruction.\n");
  write_app_test_file(workspace / ".ava" / "commands" / "local.md", "Local project command $1\n");
  write_app_test_file(workspace / ".ava" / "skills" / "local-skill" / "SKILL.md",
                      "---\nname: local-skill\ndescription: Local project skill\n---\nLocal skill body\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.local" / "plugin.json",
                      "{\n"
                      "  \"schema_version\": 1,\n"
                      "  \"id\": \"com.example.local\",\n"
                      "  \"name\": \"Local Plugin\",\n"
                      "  \"version\": \"0.1.0\",\n"
                      "  \"api_version\": \"ava.plugin.v1\",\n"
                      "  \"description\": \"local plugin\",\n"
                      "  \"entrypoint\": {\"command\": \"node\", \"args\": [\"plugin.js\"]},\n"
                      "  \"capabilities\": [\"commands\"],\n"
                      "  \"contributes\": {\n"
                      "    \"commands\": [{\"name\": \"todo\", \"description\": \"Local todo\"}],\n"
                      "    \"prompts\": [{\"name\": \"review\", \"description\": \"Local review\", \"path\": \"prompts/review.md\"}],\n"
                      "    \"skills\": [{\"name\": \"triage\", \"description\": \"Local triage\", \"path\": \"skills/triage.md\"}]\n"
                      "  }\n"
                      "}");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.local" / "prompts" / "review.md", "Local plugin prompt\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.local" / "skills" / "triage.md", "Local plugin skill\n");

  auto unlocked_session = open_test_session(root, workspace);
  auto system_prompt = [&] { return ava::app::runtime::session_ts::rat(unlocked_session)->system_prompt(); };
  auto project_trust = [&] { return ava::app::runtime::session_ts::rat(unlocked_session)->project_trust(); };

  expect(project_trust().decision == ava::app::ProjectTrustDecision::Unknown && !ava::app::project_resources_trusted(project_trust()),
         "runtime session defaults project resources to skipped without a trust decision");
  expect(system_prompt().find("Project AGENTS context still loads") != std::string::npos &&
             system_prompt().find("Global append instruction") != std::string::npos &&
             system_prompt().find("Project system replacement") == std::string::npos &&
             system_prompt().find("Project append instruction") == std::string::npos && system_prompt().find("local-skill") == std::string::npos,
         "project AGENTS context loads while project system prompt files and skills remain gated");

  auto registry = ava::app::load_command_registry(unlocked_session, ava::app::CommandRegistryOptions{.include_mcp_prompts = false});
  expect(find_entry(registry, "/global") != nullptr && find_entry(registry, "/local") == nullptr && find_entry(registry, "/skill:local-skill") == nullptr,
         "untrusted sessions load global prompt commands but skip project prompt and skill commands");
  auto context_before = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context"});
  expect(context_before && context_before->handled && !context_before->output.empty() &&
             context_before->output[0].find("project_trust=unknown project_resources=skipped") != std::string::npos &&
             context_before->output[0].find("system_prompt_sources=1") != std::string::npos &&
             context_before->output[0].find("append_system_prompt  global  APPEND_SYSTEM.md") != std::string::npos &&
             context_before->output[0].find("system_prompt  project  SYSTEM.md") == std::string::npos &&
             context_before->output[0].find("prompt_commands=1") != std::string::npos && context_before->output[0].find("skills=0") != std::string::npos &&
             context_before->output[0].find("plugin_sources=0") != std::string::npos &&
             context_before->output[0].find("prompt_command  project  local") == std::string::npos,
         "untrusted /context reports skipped project resources without listing project freshness sources");
  auto plugins_before = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins list"});
  expect(
      plugins_before && plugins_before->handled && !plugins_before->output.empty() && plugins_before->output[0].find("com.example.local") == std::string::npos,
      "untrusted plugin commands do not discover project plugin manifests");

  auto trust = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/trust project"});
  expect(trust && trust->handled && !trust->output.empty() && trust->output[0].find("trusted project resources") != std::string::npos &&
             trust->output[0].find("project_resources=enabled") != std::string::npos && project_trust().decision == ava::app::ProjectTrustDecision::Trusted,
         "/trust project persists trust outside the workspace and reloads the runtime prompt state");
  expect(system_prompt().find("Project system replacement") != std::string::npos && system_prompt().find("Project append instruction") != std::string::npos &&
             system_prompt().find("Global append instruction") == std::string::npos &&
             system_prompt().find("Implement changes directly") == std::string::npos && system_prompt().find("local-skill") != std::string::npos,
         "trusted project resources replace/append the active system prompt after reload");

  registry = ava::app::load_command_registry(unlocked_session, ava::app::CommandRegistryOptions{.include_mcp_prompts = false});
  expect(find_entry(registry, "/local") != nullptr && find_entry(registry, "/skill:local-skill") != nullptr,
         "trusted sessions expose project prompt and skill commands");
  auto local = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/local topic"});
  expect(local && local->handled && local->prompt_message && local->prompt_message->find("Local project command topic") != std::string::npos,
         "trusted project prompt commands can be invoked");
  auto skill =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/skill:local-skill", .permission_resolver = allow_all_permissions()});
  expect(skill && skill->handled && skill->prompt_message && skill->prompt_message->find("Local skill body") != std::string::npos,
         "trusted project skill commands can be invoked");
  auto context_after = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context"});
  expect(context_after && context_after->handled && !context_after->output.empty() &&
             context_after->output[0].find("project_trust=trusted project_resources=enabled") != std::string::npos &&
             context_after->output[0].find("system_prompt  project  SYSTEM.md") != std::string::npos &&
             context_after->output[0].find("append_system_prompt  project  APPEND_SYSTEM.md") != std::string::npos &&
             context_after->output[0].find("prompt_command  project  local") != std::string::npos &&
             context_after->output[0].find("skill  project  local-skill") != std::string::npos &&
             context_after->output[0].find("plugin_manifest  project  com.example.local/manifest") != std::string::npos,
         "trusted /context reports project prompt, skill, and plugin freshness sources");

  auto const global_append = paths.ava_config_dir / "APPEND_SYSTEM.md";
  auto const symlink_target = root / "outside-append.md";
  write_app_test_file(symlink_target, "outside append\n");
  std::error_code symlink_setup_error;
  std::filesystem::remove(global_append, symlink_setup_error);
  symlink_setup_error.clear();
  std::filesystem::create_symlink(symlink_target, global_append, symlink_setup_error);
  if (!symlink_setup_error)
  {
    auto denied_with_bad_prompt = ava::app::set_project_trust_decision(paths, workspace, false);
    expect(denied_with_bad_prompt.has_value(), denied_with_bad_prompt
                                                   ? "test denies project trust for failed reload"
                                                   : "test denies project trust for failed reload: " + denied_with_bad_prompt.error().format());
    auto failed_reload = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload trust"});
    expect(failed_reload && failed_reload->handled && !failed_reload->output.empty() && failed_reload->output[0].find("trust: error") != std::string::npos &&
               failed_reload->output[0].find("freshness source is not a regular file") != std::string::npos &&
               failed_reload->output[0].find("authority_state: project authority removed with fail-closed empty prompt") != std::string::npos &&
               project_trust().decision == ava::app::ProjectTrustDecision::Denied,
           "/reload trust applies an untrusted authority state even when dependent prompt reconstruction fails");
    expect(system_prompt().empty(), "/reload trust removes all prior project prompt authority when untrusted reconstruction needs a fail-closed fallback");
    std::error_code restore_error;
    std::filesystem::remove(global_append, restore_error);
    write_app_test_file(global_append, "Global append instruction.\n");

    auto trusted_again = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/trust project"});
    std::error_code second_symlink_error;
    std::filesystem::remove(global_append, second_symlink_error);
    second_symlink_error.clear();
    std::filesystem::create_symlink(symlink_target, global_append, second_symlink_error);
    if (!second_symlink_error)
    {
      auto denied_for_all = ava::app::set_project_trust_decision(paths, workspace, false);
      auto reload_all = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload all"});
      expect(trusted_again && denied_for_all && reload_all && reload_all->handled && !reload_all->output.empty() &&
                 reload_all->output[0].find("trust: error") != std::string::npos &&
                 reload_all->output[0].find("prompts: included-with-trust") != std::string::npos &&
                 reload_all->output[0].find("prompts: loaded") == std::string::npos && project_trust().decision == ava::app::ProjectTrustDecision::Denied &&
                 system_prompt().empty(),
             "/reload all uses the untrusted transaction, reports fail-closed prompt reconstruction, and never reports a later stale prompt reload");
      std::filesystem::remove(global_append, restore_error);
      write_app_test_file(global_append, "Global append instruction.\n");
    }
    else
    {
      write_app_test_file(global_append, "Global append instruction.\n");
    }
  }
  else
  {
    write_app_test_file(global_append, "Global append instruction.\n");
  }

  auto denied = ava::app::set_project_trust_decision(paths, workspace, false);
  expect(denied.has_value(),
         denied ? "test denies project trust outside the active session" : "test denies project trust outside the active session: " + denied.error().format());
  auto reload_trust = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload trust"});
  expect(reload_trust && reload_trust->handled && !reload_trust->output.empty() && reload_trust->output[0].find("Reload report:") != std::string::npos &&
             reload_trust->output[0].find("trust: loaded") != std::string::npos && reload_trust->output[0].find("decision: denied") != std::string::npos &&
             reload_trust->output[0].find("project_resources: skipped") != std::string::npos &&
             project_trust().decision == ava::app::ProjectTrustDecision::Denied,
         "/reload trust applies external deny decisions and disables project resources");
  expect(system_prompt().find("Project system replacement") == std::string::npos && system_prompt().find("Project append instruction") == std::string::npos &&
             system_prompt().find("local-skill") == std::string::npos,
         "/reload trust removes trusted project prompt content after denial");
  registry = ava::app::load_command_registry(unlocked_session, ava::app::CommandRegistryOptions{.include_mcp_prompts = false});
  expect(find_entry(registry, "/local") == nullptr && find_entry(registry, "/skill:local-skill") == nullptr,
         "/reload trust removes project prompt and skill commands after denial");
}

void test_jobs_command_reads_owned_session_history_after_reopen_path()
{
  auto const root = create_empty_root("command-registry-jobs-history");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const paths = app_test_paths(root);
  std::string session_id;
  {
    auto unlocked_session = open_test_session(root, workspace);
    ava::agent::SessionAppendSink owner;
    {
      SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
      session_id = session_r->store.session_id();
      owner = session_r->owner_append_route_1();
    }
    ava::session::SubagentJobHistoryRecord completed;
    completed.phase = ava::session::SubagentJobHistoryPhase::Terminal;
    completed.job_id = "job_session_hist";
    completed.task_id = "session_child";
    completed.parent_session_id = session_id;
    completed.child_session_id = "session_child";
    completed.delivery_id = "delivery_session_hist";
    completed.mode = ava::session::SubagentJobHistoryMode::Background;
    completed.execution = ava::session::SubagentJobHistoryExecution::Completed;
    completed.started_at = "2026-05-01T00:00:00Z";
    completed.updated_at = "2026-05-01T00:00:01Z";
    completed.terminal_at = "2026-05-01T00:00:01Z";
    completed.summary = "session recorded summary";
    ava::session::SubagentJobHistoryRecord unmatched = completed;
    unmatched.phase = ava::session::SubagentJobHistoryPhase::Start;
    unmatched.job_id = "job_session_open";
    unmatched.delivery_id = "delivery_session_open";
    unmatched.execution = ava::session::SubagentJobHistoryExecution::Starting;
    unmatched.terminal_at = std::nullopt;
    unmatched.summary = std::nullopt;
    auto completed_entry = ava::session::make_subagent_job_history_entry(completed);
    auto unmatched_entry = ava::session::make_subagent_job_history_entry(unmatched);
    expect(completed_entry && unmatched_entry && owner, "owned session history fixture writes through owner_append_route");
    if (!completed_entry || !unmatched_entry || !owner)
      return;
    auto appended_completed = owner(std::move(*completed_entry));
    auto appended_unmatched = owner(std::move(*unmatched_entry));
    expect(appended_completed && appended_unmatched, appended_completed && appended_unmatched
                                                         ? "owner_append_route accepts job history"
                                                         : (appended_completed ? appended_unmatched.error().format() : appended_completed.error().format()));
    owner = {};
  }

  ava::app::runtime::OpenContext reopen_context;
  reopen_context.workspace_dir = workspace;
  reopen_context.current_dir = workspace;
  reopen_context.paths = paths;
  reopen_context.exact_session_id = true;
  auto reopened = ava::app::runtime::Session::open(reopen_context, {.requested_session_id = session_id});
  expect(reopened.has_value(), reopened ? "fresh runtime reopens the exact session" : reopened.error().format());
  if (!reopened)
    return;
  std::shared_ptr<ava::agent::SubagentCoordinator> coordinator;
  {
    SCOPED_CRITICAL_AREA_R(session_r, *reopened);
    coordinator = session_r->subagent_coordinator();
    expect(session_r->store.session_id() == session_id, "reopen keeps the exact session identity");
  }
  expect(coordinator && coordinator->list(session_id).empty(), "reopen uses a fresh empty coordinator and does not rehydrate jobs");
  auto listed = ava::app::run_command(*reopened, ava::app::CommandRequest{.command = "/jobs list"});
  auto result = ava::app::run_command(*reopened, ava::app::CommandRequest{.command = "/jobs result job_session_hist"});
  auto unknown = ava::app::run_command(*reopened, ava::app::CommandRequest{.command = "/jobs result job_session_open"});
  auto canceled = ava::app::run_command(*reopened, ava::app::CommandRequest{.command = "/jobs cancel job_session_hist"});
  expect(listed && !listed->output.empty() && listed->output[0].find("job_session_hist") != std::string::npos &&
             listed->output[0].find("historical") != std::string::npos && result && !result->output.empty() &&
             result->output[0].find("session recorded summary") != std::string::npos && unknown && !unknown->output.empty() &&
             unknown->output[0].find("Interrupted") != std::string::npos && unknown->output[0].find("outcome unknown") != std::string::npos && canceled &&
             !canceled->output.empty() && canceled->output[0].find("display-only") != std::string::npos,
         "/jobs after actual reopen shows recorded history, unmatched starts as interrupted/unknown, and rejects live controls");
}

void test_jobs_command_merges_recorded_history_without_live_controls()
{
  ava::session::SubagentJobHistoryView completed;
  completed.record.phase = ava::session::SubagentJobHistoryPhase::Terminal;
  completed.record.job_id = "job_hist_done";
  completed.record.task_id = "session_child";
  completed.record.parent_session_id = "session_parent";
  completed.record.child_session_id = "session_child";
  completed.record.delivery_id = "delivery_hist";
  completed.record.mode = ava::session::SubagentJobHistoryMode::Background;
  completed.record.execution = ava::session::SubagentJobHistoryExecution::Completed;
  completed.record.started_at = "2026-05-01T00:00:00Z";
  completed.record.updated_at = "2026-05-01T00:00:01Z";
  completed.record.terminal_at = "2026-05-01T00:00:01Z";
  completed.record.summary = "recorded summary";

  ava::session::SubagentJobHistoryView unmatched;
  unmatched.record = completed.record;
  unmatched.record.job_id = "job_hist_open";
  unmatched.record.phase = ava::session::SubagentJobHistoryPhase::Start;
  unmatched.record.execution = ava::session::SubagentJobHistoryExecution::Interrupted;
  unmatched.record.terminal_at = std::nullopt;
  unmatched.record.summary = std::nullopt;
  unmatched.unmatched_start = true;

  auto listed = ava::app::run_jobs_command(nullptr, "session_parent", "list", false, {completed, unmatched});
  auto history = ava::app::run_jobs_command(nullptr, "session_parent", "history", false, {completed, unmatched});
  auto show = ava::app::run_jobs_command(nullptr, "session_parent", "show job_hist_done", false, {completed, unmatched});
  auto result = ava::app::run_jobs_command(nullptr, "session_parent", "result job_hist_done", false, {completed, unmatched});
  auto unknown = ava::app::run_jobs_command(nullptr, "session_parent", "result job_hist_open", false, {completed, unmatched});
  auto cancel = ava::app::run_jobs_command(nullptr, "session_parent", "cancel job_hist_done", false, {completed, unmatched});
  expect(listed && !listed->output.empty() && listed->output[0].find("job_hist_done") != std::string::npos &&
             listed->output[0].find("historical") != std::string::npos && history && !history->output.empty() &&
             history->output[0].rfind("Job history", 0) == 0 && show && !show->output.empty() && show->output[0].find("Completed") != std::string::npos &&
             result && !result->output.empty() && result->output[0].find("recorded summary") != std::string::npos && unknown && !unknown->output.empty() &&
             unknown->output[0].find("outcome unknown") != std::string::npos && cancel && !cancel->output.empty() &&
             cancel->output[0].find("display-only") != std::string::npos,
         "/jobs list/history/show/result expose recorded history while controls reject historical-only jobs");

  auto coordinator = ava::agent::SubagentCoordinator::create();
  expect(coordinator.has_value(), "history merge live-wins fixture creates a coordinator");
  if (!coordinator)
    return;
  auto started = (*coordinator)->start_background("session_parent", {.child_session_id = "session_live_child"}, [](auto const&) {
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed, .final_text = "live", .stop_reason = "completed"};
  });
  expect(started.has_value(), "history merge live-wins fixture starts");
  if (!started)
    return;
  ava::session::SubagentJobHistoryView stale = completed;
  stale.record.job_id = started->job.identity.job_id;
  stale.record.summary = "stale recorded summary";
  auto merged = ava::app::run_jobs_command(*coordinator, "session_parent", "list", false, {stale});
  expect(merged && !merged->output.empty() && merged->output[0].find(started->job.identity.job_id) != std::string::npos &&
             merged->output[0].find("stale recorded summary") == std::string::npos && merged->output[0].find("historical") == std::string::npos,
         "live jobs win over recorded history for the same id");
}

void test_jobs_history_overlays_live_running_unmatched_start()
{
  struct BlockingWorker
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool release = false;

    ava::agent::BackgroundJobCompletion run(ava::agent::BackgroundJobContext const& context)
    {
      std::stop_callback wake_on_stop(context.stop_token, [&] { changed.notify_all(); });
      std::unique_lock lock(mutex);
      started = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release || context.stop_token.stop_requested(); });
      return {.state = ava::agent::BackgroundJobState::Completed, .final_text = "later", .stop_reason = "completed"};
    }

    bool wait_started()
    {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, std::chrono::seconds(2), [&] { return started; });
    }

    void finish()
    {
      std::lock_guard lock(mutex);
      release = true;
      changed.notify_all();
    }
  };

  auto coordinator = ava::agent::SubagentCoordinator::create();
  expect(coordinator.has_value(), "history live-overlay fixture creates a coordinator");
  if (!coordinator)
    return;
  auto worker = std::make_shared<BlockingWorker>();
  std::vector<ava::session::SessionEntry> recorded;
  auto started = (*coordinator)
                     ->start(ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "session_parent",
                                                                         .mode = ava::agent::SubagentJobMode::Background,
                                                                         .job = {.child_session_id = "session_child_live"},
                                                                         .history_append =
                                                                             [&recorded](ava::session::SessionEntry entry) {
                                                                               recorded.push_back(std::move(entry));
                                                                               return ava::core::VoidResult{};
                                                                             }},
                             [worker](auto const& context) { return worker->run(context); });
  expect(started && worker->wait_started() && started->job.execution == ava::agent::SubagentExecutionState::Running,
         "history live-overlay fixture starts a blocking worker");
  if (!started)
    return;
  auto projected = ava::session::project_subagent_job_history("session_parent", recorded);
  expect(projected.size() == 1 && projected[0].unmatched_start, "live running job has an unmatched start history row");
  auto history = ava::app::run_jobs_command(*coordinator, "session_parent", "history", false, projected);
  auto listed = ava::app::run_jobs_command(*coordinator, "session_parent", "list", false, projected);
  expect(history && !history->output.empty() && history->output[0].find("Running") != std::string::npos &&
             history->output[0].find("Interrupted") == std::string::npos && history->output[0].find("outcome unknown") == std::string::npos &&
             history->output[0].find("live") != std::string::npos && listed && !listed->output.empty() &&
             listed->output[0].find("Running") != std::string::npos && listed->output[0].find("historical") == std::string::npos,
         "/jobs history overlays live Running over unmatched start and does not imply interrupted/unknown");
  worker->finish();
  auto terminal = (*coordinator)->wait("session_parent", started->job.identity.job_id, std::chrono::seconds(2));
  expect(terminal && !terminal->timed_out, "history live-overlay fixture joins the blocked worker");
}

}  // namespace

void run_app_command_registry_tests()
{
  test_prompt_commands_load_project_global_and_expand_arguments();
  test_skill_commands_are_registry_entries_and_permissioned_prompts();
  test_plugin_commands_are_registry_entries();
  test_mcp_prompts_are_registry_entries_and_permissioned_prompts();
  test_builtin_session_alias_registers_as_current_stats_command();
  test_interactive_jobs_human_output_keeps_public_json_contract();
  test_jobs_command_reads_owned_session_history_after_reopen_path();
  test_jobs_command_merges_recorded_history_without_live_controls();
  test_jobs_history_overlays_live_running_unmatched_start();
  test_project_trust_gates_project_resource_commands();
}
