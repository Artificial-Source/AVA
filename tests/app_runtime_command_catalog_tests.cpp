#include "sys.h"
#include "tests/app_runtime_test_declarations.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/app/command_catalog.h"
#include "ava/app/command_format.h"
#include "ava/app/command_palette.h"
#include "ava/app/command_permissions.h"
#include "ava/app/command_sessions.h"
#include "ava/app/commands.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_model.h"
#include "ava/app/tui/command_palette_views.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/mode.h"
#include "ava/agent/subagent_coordinator.h"
#include "ava/tools/file_tools.h"
#include "ava/tui/composer.h"
#include "ava/config/model_config.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/session_branch.h"
#include "ava/session/session_metadata.h"
#include "ava/permissions/permission.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

// Exercise command catalog and dispatcher behavior while releasing session access around background-job waits.
//
// unlocked_session supplies the runtime session, while paths, workspace, and custom_hotkeys provide the fixture configuration and expected catalog
// customization. The function updates session-backed command state as part of the test.
void app_command_dispatcher_catalog_part(ava::app::runtime::session_ts& unlocked_session, ava::config::XdgPaths const& paths,
                                         std::filesystem::path const& workspace, std::vector<ava::app::CommandHotkey> const& custom_hotkeys)
{
  auto session_id = [&] { return ava::app::runtime::session_ts::rat(unlocked_session)->store.session_id(); };
  auto session_workspace_dir = [&] { return ava::app::runtime::session_ts::rat(unlocked_session)->workspace_dir(); };
  auto const slash_items = ava::app::command_catalog_slash_items_1(unlocked_session, custom_hotkeys);
  auto const file_reference_items = ava::app::file_reference_items(unlocked_session);
  auto has_alias = [](ava::tui::SlashCommandItem const& item, std::string_view value) { return std::ranges::find(item.aliases, value) != item.aliases.end(); };
  auto has_file_reference = [&file_reference_items](std::string_view value) {
    return std::ranges::any_of(file_reference_items, [&](auto const& item) { return item.value == value; });
  };
  auto const* connect_item = tui_test_support::find_slash_command_item(slash_items, "/connect");
  auto const* hotkeys_item = tui_test_support::find_slash_command_item(slash_items, "/hotkeys");
  auto const* models_item = tui_test_support::find_slash_command_item(slash_items, "/models");
  auto const* providers_item = tui_test_support::find_slash_command_item(slash_items, "/providers");
  auto const* scoped_models_item = tui_test_support::find_slash_command_item(slash_items, "/scoped-models");
  auto const* details_item = tui_test_support::find_slash_command_item(slash_items, "/details");
  auto const* cursor_item = tui_test_support::find_slash_command_item(slash_items, "/cursor");
  auto const* stash_item = tui_test_support::find_slash_command_item(slash_items, "/stash");
  auto const* search_item = tui_test_support::find_slash_command_item(slash_items, "/search");
  auto const* tool_item = tui_test_support::find_slash_command_item(slash_items, "/tool");
  auto const* diff_item = tui_test_support::find_slash_command_item(slash_items, "/diff");
  auto const* copy_item = tui_test_support::find_slash_command_item(slash_items, "/copy");
  auto const* thinking_item = tui_test_support::find_slash_command_item(slash_items, "/thinking");
  auto const* export_item = tui_test_support::find_slash_command_item(slash_items, "/export");
  auto const* import_item = tui_test_support::find_slash_command_item(slash_items, "/import");
  auto const* setup_item = tui_test_support::find_slash_command_item(slash_items, "/setup");
  auto const setup_matches = ava::tui::filter_slash_commands("/setup", slash_items);
  expect(setup_item == nullptr && setup_matches.empty(), "removed /setup command is absent from the slash catalog and completion matches");
  expect(details_item != nullptr && details_item->hint == "[compact|rich|expanded]" && details_item->description.find("Rich") != std::string::npos,
         "slash catalog exposes explicit Compact, Rich, and Expanded tool-card modes");
  expect(cursor_item != nullptr && cursor_item->hint == "default|block|underline|bar [blink|steady]" &&
             tui_test_support::has_slash_argument_completion(cursor_item, 0, "default") &&
             tui_test_support::has_slash_argument_completion(cursor_item, 0, "block") &&
             tui_test_support::has_slash_argument_completion(cursor_item, 0, "underline") &&
             tui_test_support::has_slash_argument_completion(cursor_item, 0, "bar") &&
             tui_test_support::has_slash_argument_completion(cursor_item, 1, "blink", {"bar"}) &&
             tui_test_support::has_slash_argument_completion(cursor_item, 1, "steady", {"underline"}),
         "slash catalog exposes cursor style and blink argument completions");
  expect(stash_item != nullptr && stash_item->hint == "[pop|clear]" && stash_item->description.find("process-memory") != std::string::npos,
         "slash catalog exposes process-memory prompt stash controls");
  expect(search_item != nullptr && search_item->hint == "[query]" && search_item->description.find("currently rendered") != std::string::npos,
         "slash catalog exposes TUI transcript literal search with a truthful rendered-scope description");
  expect(tool_item != nullptr && tool_item->hint == "[query]" && has_alias(*tool_item, "/tools") &&
             tool_item->description.find("latest or matching tool details") != std::string::npos,
         "slash catalog exposes /tool for visible TUI tool-card inspection");
  expect(diff_item != nullptr && diff_item->hint == "[query]" && diff_item->description.find("latest or matching tool diff") != std::string::npos,
         "slash catalog exposes /diff for visible TUI tool-diff inspection");
  expect(copy_item != nullptr && copy_item->hint.empty() && copy_item->description.find("latest AVA message") != std::string::npos &&
             copy_item->description.find("user turn") != std::string::npos && copy_item->description.find("tool") != std::string::npos &&
             copy_item->description.find("permission details") != std::string::npos,
         "slash catalog exposes /copy including the user-turn picker while preserving bare-command exact-submit behavior");
  expect(thinking_item != nullptr && thinking_item->hint.empty() && thinking_item->description.find("/thinking details") != std::string::npos,
         "slash catalog documents /thinking details while preserving bare-command exact-submit behavior");
  auto const* fork_from_item = tui_test_support::find_slash_command_item(slash_items, "/fork-from");
  expect(fork_from_item != nullptr && fork_from_item->description.find("public user turn") != std::string::npos,
         "slash catalog exposes /fork-from user-turn fork picker");
  expect(scoped_models_item != nullptr && scoped_models_item->description.find("Ctrl+P cycling") != std::string::npos,
         "slash catalog exposes Pi-style /scoped-models model cycle selector entry point");
  expect(models_item != nullptr && models_item->hint == "[query]" && models_item->description.find("model selector") != std::string::npos,
         "slash catalog explains that exact /models opens the selector while an argument filters configured models");
  expect(providers_item != nullptr && providers_item->hint == "[query]" && providers_item->description.find("credential status") != std::string::npos,
         "slash catalog exposes provider capability and credential status discovery");
  auto const* sessions_item = tui_test_support::find_slash_command_item(slash_items, "/sessions");
  auto const* context_item = tui_test_support::find_slash_command_item(slash_items, "/context");
  auto const* read_item = tui_test_support::find_slash_command_item(slash_items, "/read");
  auto const* write_item = tui_test_support::find_slash_command_item(slash_items, "/write");
  auto const* glob_item = tui_test_support::find_slash_command_item(slash_items, "/glob");
  auto const* find_item = tui_test_support::find_slash_command_item(slash_items, "/find");
  auto const* ls_item = tui_test_support::find_slash_command_item(slash_items, "/ls");
  auto const* grep_item = tui_test_support::find_slash_command_item(slash_items, "/grep");
  auto const* mcp_item = tui_test_support::find_slash_command_item(slash_items, "/mcp");
  auto const* plugin_item = tui_test_support::find_slash_command_item(slash_items, "/plugin");
  auto const* permissions_item = tui_test_support::find_slash_command_item(slash_items, "/permissions");
  expect(
      !tui_test_support::has_slash_argument_completion(connect_item, 0, "openai") &&
          !tui_test_support::has_slash_argument_completion(connect_item, 1, "api-key") &&
          !tui_test_support::has_slash_argument_completion(connect_item, 1, "browser-oauth", {"openai"}) &&
          !tui_test_support::has_slash_argument_completion(connect_item, 1, "headless-oauth", {"openai"}) &&
          tui_test_support::has_slash_argument_completion(models_item, 0, "openai/gpt-5.5") &&
          !tui_test_support::has_slash_argument_completion(models_item, 0, "gpt-5.5") &&
          tui_test_support::has_slash_argument_completion(sessions_item, 0, session_id()) &&
          tui_test_support::has_slash_argument_completion(context_item, 0, (workspace / "AGENTS.md").generic_string()) &&
          tui_test_support::has_slash_argument_completion(read_item, 0, "src/main.cpp") &&
          tui_test_support::has_slash_argument_completion(write_item, 0, "src/main.cpp") &&
          tui_test_support::has_slash_argument_completion(glob_item, 0, "src/**") && tui_test_support::has_slash_argument_completion(find_item, 0, "src/**") &&
          tui_test_support::has_slash_argument_completion(ls_item, 0, "src/main.cpp") &&
          tui_test_support::has_slash_argument_completion(grep_item, 1, "src/**") &&
          tui_test_support::has_slash_argument_completion(export_item, 0, "markdown") &&
          tui_test_support::has_slash_argument_completion(export_item, 0, "html") && tui_test_support::has_slash_argument_completion(export_item, 0, "jsonl") &&
          tui_test_support::has_slash_argument_completion(import_item, 1, "--confirm") &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 0, "init") &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 0, "import") &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 0, "set") &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 0, "reset") &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 1, "submit", {"set"}) &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 1, "variant_cycle", {"set"}) &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 1, "submit", {"reset"}) &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 0, "validate") &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 1, "--force", {"init"}) &&
          tui_test_support::has_slash_argument_completion(hotkeys_item, 2, "--force", {"import"}) &&
          !tui_test_support::has_slash_argument_completion(read_item, 0, "my folder/space file.txt") &&
          !tui_test_support::has_slash_argument_completion(read_item, 0, "docs/reference-code/pi/reference-only.md") && has_file_reference("src/main.cpp") &&
          has_file_reference("my folder/space file.txt") && !has_file_reference("docs/reference-code/pi/reference-only.md") &&
          tui_test_support::has_slash_argument_completion(mcp_item, 1, "fs", {"inspect"}) &&
          tui_test_support::has_slash_argument_completion(plugin_item, 1, "com.example.project", {"run"}) &&
          tui_test_support::has_slash_argument_completion(plugin_item, 2, "todo", {"run", "com.example.project"}) &&
          tui_test_support::has_slash_argument_completion(permissions_item, 0, "list") &&
          tui_test_support::has_slash_argument_completion(permissions_item, 0, "audit") &&
          tui_test_support::has_slash_argument_completion(permissions_item, 0, "add") &&
          tui_test_support::has_slash_argument_completion(permissions_item, 1, "export", {"audit"}) &&
          tui_test_support::has_slash_argument_completion(permissions_item, 1, "summary", {"audit"}) &&
          tui_test_support::has_slash_argument_completion(permissions_item, 1, "show", {"audit"}) &&
          tui_test_support::has_slash_argument_completion(permissions_item, 1, "action=allow", {"add"}) &&
          tui_test_support::has_slash_argument_completion(permissions_item, 1, "operation=read", {"add"}) &&
          tui_test_support::has_slash_argument_completion(permissions_item, 1, "reason=", {"add"}),
      "command catalog argument completions keep /connect provider and method choices in the modal while "
      "populating model, session, context, export format, file path, file reference, MCP, plugin, and permission-rule metadata");
  auto disable_plugin = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins disable com.example.project"});
  expect(disable_plugin && disable_plugin->handled && !disable_plugin->output.empty() &&
             disable_plugin->output[0].find("Disabled project plugin com.example.project") != std::string::npos &&
             disable_plugin->output[0].find("No plugin process was stopped") != std::string::npos,
         "command dispatcher /plugins disable records state without stopping plugin processes");
  auto validate_plugin =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins validate .ava/plugins/com.example.project/plugin.json"});
  expect(validate_plugin && validate_plugin->handled && !validate_plugin->output.empty() &&
             validate_plugin->output[0].find("Valid plugin manifest") != std::string::npos &&
             validate_plugin->output[0].find("no entrypoint was executed") != std::string::npos,
         "command dispatcher /plugins validate parses manifests without executing entrypoints");
  auto plugin_failures = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins failures"});
  expect(plugin_failures && plugin_failures->handled && !plugin_failures->output.empty() &&
             plugin_failures->output[0].find("com.example.bad") != std::string::npos,
         "command dispatcher /plugins failures reports invalid discovered manifests");
  auto models = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/model"});
  expect(models && models->handled && !models->output.empty() && models->output[0].find("Models:") != std::string::npos &&
             models->output[0].find("current ") != std::string::npos && models->output[0].find("reasoning current") != std::string::npos &&
             models->output[0].find("reasoning levels: low, medium, high, xhigh") != std::string::npos &&
             models->output[0].find("reasoning params") != std::string::npos && models->output[0].find("reasoning.effort=<level>") != std::string::npos &&
             models->output[0].find("reasoning.summary=auto") != std::string::npos && models->output[0].find("reasoning format") != std::string::npos &&
             models->output[0].find("Shift+Tab cycles") != std::string::npos &&
             models->output[0].find("Ctrl+T opens the policy-resolved thinking-mode selector") != std::string::npos,
         "command dispatcher lists provider/model reasoning metadata and documents TUI reasoning controls");
  auto filtered_models = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/models gpt-5.5"});
  expect(filtered_models && filtered_models->handled && !filtered_models->output.empty() &&
             filtered_models->output[0].find("filter gpt-5.5") != std::string::npos && filtered_models->output[0].find("gpt-5.5") != std::string::npos,
         "command dispatcher /models accepts backend-backed autocomplete query text without switching models");
  auto diagnostic_models = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/models diagnostic-local"});
  expect(diagnostic_models && diagnostic_models->handled && !diagnostic_models->output.empty() &&
             diagnostic_models->output[0].find("Diagnostic Local") != std::string::npos &&
             diagnostic_models->output[0].find("diagnostics:") != std::string::npos &&
             diagnostic_models->output[0].find("custom model missing context_window_tokens") != std::string::npos &&
             diagnostic_models->output[0].find("custom model api_family does not match provider profile") != std::string::npos &&
             diagnostic_models->output[0].find("custom model has unknown tool support") != std::string::npos &&
             diagnostic_models->output[0].find("reasoning model has no reasoning_levels") != std::string::npos,
         "command dispatcher /models reports actionable custom-model diagnostics");
  auto unavailable_models = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/models ghost"});
  expect(unavailable_models && unavailable_models->handled && !unavailable_models->output.empty() &&
             unavailable_models->output[0].find("Remote Missing") != std::string::npos &&
             unavailable_models->output[0].find("provider is not registered") != std::string::npos &&
             unavailable_models->output[0].find("custom model api_family is not recognized") != std::string::npos &&
             unavailable_models->output[0].find("selector is disabled") != std::string::npos,
         "command dispatcher /models reports unregistered provider diagnostics");
  {
    ScopedEnvVar openai_key("OPENAI_API_KEY", "providers-secret-openai-key");
    auto providers = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/providers"});
    expect(providers && providers->handled && !providers->output.empty() && providers->output[0].find("Providers:") != std::string::npos &&
               providers->output[0].find("openai  OpenAI") != std::string::npos && providers->output[0].find("anthropic  Anthropic") != std::string::npos &&
               providers->output[0].find("auth=api_key source=env:OPENAI_API_KEY") != std::string::npos &&
               providers->output[0].find("providers-secret-openai-key") == std::string::npos &&
               providers->output[0].find("oauth=interactive supported") != std::string::npos &&
               providers->output[0].find("interactive deferred pending official third-party flow") != std::string::npos &&
               providers->output[0].find("vercel  Vercel AI Gateway  unavailable: no runtime provider factory") != std::string::npos,
           "command dispatcher /providers lists provider runtime, auth, and OAuth status without printing secrets");
  }
  {
    ScopedEnvVar anthropic_oauth("ANTHROPIC_OAUTH_TOKEN", "providers-secret-anthropic-token");
    auto filtered_providers = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/providers anthropic"});
    expect(filtered_providers && filtered_providers->handled && !filtered_providers->output.empty() &&
               filtered_providers->output[0].find("filter anthropic") != std::string::npos &&
               filtered_providers->output[0].find("anthropic  Anthropic") != std::string::npos &&
               filtered_providers->output[0].find("auth=oauth source=env:ANTHROPIC_OAUTH_TOKEN") != std::string::npos &&
               filtered_providers->output[0].find("providers-secret-anthropic-token") == std::string::npos &&
               filtered_providers->output[0].find("openai  OpenAI") == std::string::npos,
           "command dispatcher /providers accepts query text without exposing unrelated provider rows or env token values");
  }

  auto context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context"});
  expect(context && context->handled && !context->output.empty() && context->output[0].find("Context freshness:") != std::string::npos &&
             context->output[0].find("mode=plan") != std::string::npos && context->output[0].find("model=openai/gpt-5.5") != std::string::npos &&
             context->output[0].find("prompt=override") != std::string::npos && context->output[0].find("plan.txt") != std::string::npos &&
             context->output[0].find("context_sources=1") != std::string::npos && context->output[0].find("prompt_commands=1") != std::string::npos &&
             context->output[0].find("skills=") != std::string::npos && context->output[0].find("plugin_sources=2") != std::string::npos &&
             context->output[0].find("workspace") != std::string::npos && context->output[0].find("AGENTS.md") != std::string::npos &&
             context->output[0].find("prompt_command  project  prompt-check") != std::string::npos &&
             context->output[0].find("skill  project  dispatcher-skill") != std::string::npos &&
             context->output[0].find("plugin_manifest  project  com.example.project/manifest") != std::string::npos &&
             context->output[0].find("plugin_prompt  project  com.example.project/review") == std::string::npos &&
             context->output[0].find("plugin_skill  project  com.example.project/triage") == std::string::npos &&
             context->output[0].find("loaded_bytes=") != std::string::npos && context->output[0].find("status=changed") != std::string::npos &&
             context->output[0].find("current_bytes=") != std::string::npos,
         "command dispatcher /context reports context freshness metadata without loading disabled plugin resources");
  auto prompt_context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context prompt"});
  expect(prompt_context && prompt_context->handled && !prompt_context->output.empty() &&
             prompt_context->output[0].find("prompt=override") != std::string::npos && prompt_context->output[0].find("plan.txt") != std::string::npos &&
             prompt_context->output[0].find("prompt_command  project  prompt-check") != std::string::npos &&
             prompt_context->output[0].find("plugin_prompt  project  com.example.project/review") == std::string::npos &&
             prompt_context->output[0].find("AGENTS.md") == std::string::npos,
         "command dispatcher /context can filter to prompt freshness metadata");
  auto prompt_command_context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context prompt-check"});
  expect(prompt_command_context && prompt_command_context->handled && !prompt_command_context->output.empty() &&
             prompt_command_context->output[0].find("prompt_command  project  prompt-check") != std::string::npos &&
             prompt_command_context->output[0].find("status=changed") != std::string::npos &&
             prompt_command_context->output[0].find("AGENTS.md") == std::string::npos,
         "command dispatcher /context can filter to prompt command freshness metadata");
  auto skill_context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context dispatcher-skill"});
  expect(skill_context && skill_context->handled && !skill_context->output.empty() &&
             skill_context->output[0].find("skill  project  dispatcher-skill") != std::string::npos &&
             skill_context->output[0].find("status=changed") != std::string::npos && skill_context->output[0].find("AGENTS.md") == std::string::npos,
         "command dispatcher /context can filter to loaded skill freshness metadata");
  auto plugin_prompt_context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context review"});
  expect(plugin_prompt_context && plugin_prompt_context->handled && !plugin_prompt_context->output.empty() &&
             plugin_prompt_context->output[0].find("No context sources matching: review") != std::string::npos,
         "command dispatcher /context does not report disabled plugin prompt resources loaded before plugin enablement");
  auto filtered_context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context AGENTS"});
  expect(filtered_context && filtered_context->handled && !filtered_context->output.empty() &&
             filtered_context->output[0].find("Context freshness:") != std::string::npos &&
             filtered_context->output[0].find("AGENTS.md") != std::string::npos && filtered_context->output[0].find("status=changed") != std::string::npos &&
             filtered_context->output[0].find("prompt=override") == std::string::npos,
         "command dispatcher /context accepts backend context-source query text");
  auto reload_prompts = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload prompts"});
  expect(reload_prompts && reload_prompts->handled && !reload_prompts->output.empty() &&
             reload_prompts->output[0].find("Reload report:") != std::string::npos && reload_prompts->output[0].find("prompts: loaded") != std::string::npos &&
             reload_prompts->output[0].find("context_sources: 1") != std::string::npos &&
             ava::app::runtime::session_ts::rat(unlocked_session)->system_prompt().find("dispatcher context changed after session open") != std::string::npos,
         "command dispatcher /reload prompts re-reads prompt and context resources without restarting");
  write_app_test_file(paths.models_file, "{\n  \"scoped_model_cycle\": [\"openai/gpt-5.5\"]\n}\n");
  auto reload_models = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload models"});
  auto const model_reload_snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->scoped_model_cycle(), session_r->model().model_id};
  }();
  auto const& [scoped_model_cycle, active_model_id] = model_reload_snapshot;
  expect(reload_models && reload_models->handled && !reload_models->output.empty() && reload_models->output[0].find("models: loaded") != std::string::npos &&
             reload_models->output[0].find("scoped_cycle: configured") != std::string::npos &&
             reload_models->output[0].find("active_model: openai/gpt-5.5 (unchanged)") != std::string::npos && scoped_model_cycle &&
             scoped_model_cycle->size() == 1 && scoped_model_cycle->front() == "openai/gpt-5.5" && active_model_id == "gpt-5.5",
         "command dispatcher /reload models refreshes model config without silently switching active model");
  auto reload_all = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload"});
  expect(reload_all && reload_all->handled && !reload_all->output.empty() && reload_all->output[0].find("display: loaded") != std::string::npos &&
             reload_all->output[0].find("models: loaded") != std::string::npos && reload_all->output[0].find("trust: loaded") != std::string::npos &&
             reload_all->output[0].find("compaction: validated") != std::string::npos &&
             reload_all->output[0].find("keybindings: tui-runtime") != std::string::npos &&
             reload_all->output[0].find("auth: restart-required") != std::string::npos &&
             reload_all->output[0].find("permissions: restart-required") != std::string::npos &&
             reload_all->output[0].find("lsp: restart-required") != std::string::npos &&
             reload_all->output[0].find("mcp: restart-required") != std::string::npos &&
             reload_all->output[0].find("plugins: restart-required") != std::string::npos,
         "command dispatcher /reload reports hot-reloaded and restart-required config domains");
  write_app_test_file(paths.compaction_file, "{\n  \"max_summary_bytes\": 0\n}\n");
  auto reload_compaction = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload compaction"});
  expect(reload_compaction && reload_compaction->handled && !reload_compaction->output.empty() &&
             reload_compaction->output[0].find("compaction: error") != std::string::npos &&
             reload_compaction->output[0].find("compaction max summary bytes must be greater than zero") != std::string::npos,
         "command dispatcher /reload compaction reports config validation errors without crashing");
  write_app_test_file(paths.compaction_file, "{\n  \"provider\": \"anthropic\", \"model\": \"unknown-summary-model\"\n}\n");
  auto reload_unknown_compaction = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload compaction"});
  expect(reload_unknown_compaction && reload_unknown_compaction->handled && !reload_unknown_compaction->output.empty() &&
             reload_unknown_compaction->output[0].find("compaction: error") != std::string::npos &&
             reload_unknown_compaction->output[0].find("compaction_provider: anthropic") != std::string::npos &&
             reload_unknown_compaction->output[0].find("compaction_model: unknown-summary-model") != std::string::npos,
         "command dispatcher /reload compaction performs runtime provider/model catalog validation");
  std::error_code remove_compaction_error;
  std::filesystem::remove(paths.compaction_file, remove_compaction_error);
  auto filtered_sessions = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions " + session_id()});
  expect(filtered_sessions && filtered_sessions->handled && !filtered_sessions->output.empty() &&
             filtered_sessions->output[0].find(session_id()) != std::string::npos,
         "command dispatcher /sessions accepts backend session-id query text");
  auto archive_active = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions archive " + session_id() + " --confirm"});
  auto active_metadata_after_archive_attempt = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return ava::session::load_session_metadata(session_r->store);
  }();
  expect(archive_active && archive_active->handled && !archive_active->output.empty() &&
             archive_active->output[0].find("Cannot archive the active session") != std::string::npos && active_metadata_after_archive_attempt &&
             !active_metadata_after_archive_attempt->archived,
         "slash /sessions archive refuses to archive the active runtime session");
  auto branch = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return ava::session::create_session_branch(
        ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                           .root_dir = paths.sessions_dir,
                                           .source_session_id = session_r->store.session_id(),
                                           .branch_from_entry_id = {},
                                           .name = std::optional<std::string>("Review branch"),
                                           .labels = std::optional<std::vector<std::string>>(std::vector<std::string>{"review", "ui"}),
                                           .source_lease = &session_r->lease(),
                                           .mode = ava::session::SessionBranchMode::Fork,
                                           .actor = "test"});
  }();
  expect(branch.has_value(),
         branch ? "command dispatcher /sessions test creates a branch" : "command dispatcher /sessions test creates a branch: " + branch.error().format());
  auto tree_sessions = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions review"});
  expect(tree_sessions && tree_sessions->handled && !tree_sessions->output.empty() && tree_sessions->output[0].find("Sessions:") != std::string::npos &&
             tree_sessions->output[0].find("Review branch") != std::string::npos && tree_sessions->output[0].find("origin=fork") != std::string::npos &&
             tree_sessions->output[0].find("labels=review,ui") != std::string::npos && tree_sessions->output[0].find("parent=") != std::string::npos,
         "command dispatcher /sessions exposes tree branch names, labels, provenance, and parent links");
  if (branch)
  {
    branch->lease = ava::session::SessionLease{};
    auto const branch_session_id = branch->store.session_id();
    auto target_labels =
        ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions labels " + branch_session_id + " triage selected"});
    auto branch_metadata_after_labels = ava::session::load_session_metadata(branch->store);
    auto target_label_status = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions labels " + branch_session_id});
    auto target_label_query = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions triage"});
    expect(target_labels && target_labels->handled && !target_labels->output.empty() &&
               target_labels->output[0].find("session " + branch_session_id + " labels set: triage,selected") != std::string::npos &&
               branch_metadata_after_labels && branch_metadata_after_labels->labels.size() == 2 && branch_metadata_after_labels->labels[0] == "triage" &&
               branch_metadata_after_labels->labels[1] == "selected" && target_label_status && target_label_status->handled &&
               !target_label_status->output.empty() &&
               target_label_status->output[0].find("session " + branch_session_id + " labels: triage,selected") != std::string::npos && target_label_query &&
               target_label_query->handled && !target_label_query->output.empty() &&
               target_label_query->output[0].find("labels=triage,selected") != std::string::npos && session_id() != branch_session_id,
           "slash /sessions labels updates a selected session without switching runtime sessions");

    auto archive_without_confirm = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions archive " + branch_session_id});
    auto metadata_before_archive = ava::session::load_session_metadata(branch->store);
    expect(archive_without_confirm && archive_without_confirm->handled && !archive_without_confirm->output.empty() &&
               archive_without_confirm->output[0].find("--confirm") != std::string::npos && metadata_before_archive && !metadata_before_archive->archived,
           "slash /sessions archive requires explicit confirmation before mutating metadata");

    auto archived = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions archive " + branch_session_id + " --confirm"});
    auto metadata_after_archive = ava::session::load_session_metadata(branch->store);
    auto hidden_archived = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions review"});
    auto visible_archived = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions --archived review"});
    expect(archived && archived->handled && !archived->output.empty() &&
               archived->output[0].find("session " + branch_session_id + " archived") != std::string::npos && metadata_after_archive &&
               metadata_after_archive->archived && hidden_archived && hidden_archived->handled && !hidden_archived->output.empty() &&
               hidden_archived->output[0].find("Review branch") == std::string::npos && visible_archived && visible_archived->handled &&
               !visible_archived->output.empty() && visible_archived->output[0].find("Review branch") != std::string::npos &&
               visible_archived->output[0].find("archived") != std::string::npos,
           "slash /sessions archive hides sessions from default lists while --archived includes them");

    auto unarchived = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/sessions unarchive " + branch_session_id});
    auto metadata_after_unarchive = ava::session::load_session_metadata(branch->store);
    expect(unarchived && unarchived->handled && !unarchived->output.empty() &&
               unarchived->output[0].find("session " + branch_session_id + " unarchived") != std::string::npos && metadata_after_unarchive &&
               !metadata_after_unarchive->archived,
           "slash /sessions unarchive restores archived sessions to default views");
  }

  auto permissions_empty = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions"});
  expect(permissions_empty && permissions_empty->handled && !permissions_empty->output.empty() &&
             permissions_empty->output[0].find("Permission rules:") != std::string::npos &&
             permissions_empty->output[0].find("No persistent permission rules") != std::string::npos,
         "command dispatcher /permissions lists empty persistent permission rules with storage context");
  auto add_permission_rule =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=allow operation=read path=src/main.cpp "
                                                                                  "reason=\"trusted local read\""});
  expect(add_permission_rule && add_permission_rule->handled && !add_permission_rule->output.empty() &&
             add_permission_rule->output[0].find("added permission rule") != std::string::npos &&
             add_permission_rule->output[0].find("Allow file reads · src/main.cpp · Workspace") != std::string::npos &&
             add_permission_rule->output[0].find("Rule ID: permrule_") != std::string::npos &&
             add_permission_rule->output[0].find("trusted local read") == std::string::npos,
         "command dispatcher /permissions add leads with a human receipt and secondary rule id");
  auto extract_rule_id = [](std::string const& text) {
    auto const marker = std::string_view("Rule ID: ");
    auto const marker_pos = text.find(marker);
    auto const start = marker_pos == std::string::npos ? text.find("permrule_") : marker_pos + marker.size();
    if (start == std::string::npos)
      return std::string{};
    auto end = start;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
      ++end;
    return text.substr(start, end - start);
  };
  auto const permission_rule_id = add_permission_rule ? extract_rule_id(add_permission_rule->output[0]) : std::string{};
  expect(!permission_rule_id.empty() && permission_rule_id.starts_with("permrule_"),
         "command dispatcher /permissions add exposes the created rule id on a secondary line");

  auto add_explore_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=task tool=task command=explore "
                                                                                  "reason=\"no explore subagents\""});
  expect(add_explore_deny && add_explore_deny->handled && !add_explore_deny->output.empty() &&
             add_explore_deny->output[0].find("Block Explore subagents · Workspace") != std::string::npos,
         "command dispatcher /permissions add claims Explore only for command=explore with default tool=task");
  auto const explore_rule_id = add_explore_deny ? extract_rule_id(add_explore_deny->output[0]) : std::string{};
  expect(!explore_rule_id.empty(), "command dispatcher /permissions add exposes Explore rule id secondarily");

  auto add_skill_deny = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=skill tool=skill reason=\"no skills\""});
  expect(add_skill_deny && add_skill_deny->handled && !add_skill_deny->output.empty() &&
             add_skill_deny->output[0].find("Block skills · Workspace") != std::string::npos,
         "command dispatcher /permissions add summarizes skill blocks in human form");
  auto const skill_rule_id = add_skill_deny ? extract_rule_id(add_skill_deny->output[0]) : std::string{};

  auto add_global_read =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=allow operation=read scope=global "
                                                                                  "path=/etc/hosts reason=\"global reads\""});
  expect(add_global_read && add_global_read->handled && !add_global_read->output.empty() &&
             add_global_read->output[0].find("Allow file reads · /etc/hosts · Global") != std::string::npos,
         "command dispatcher /permissions add summarizes global file-read allows");
  auto const global_read_rule_id = add_global_read ? extract_rule_id(add_global_read->output[0]) : std::string{};

  auto add_exact_command_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=bash "
                                                                                  "command=\"git push origin main\" reason=\"block push\""});
  expect(add_exact_command_deny && add_exact_command_deny->handled && !add_exact_command_deny->output.empty() &&
             add_exact_command_deny->output[0].find("Block Exact command · Workspace") != std::string::npos &&
             add_exact_command_deny->output[0].find("git push origin main") == std::string::npos,
         "command dispatcher /permissions add receipts omit raw command bodies");
  auto const exact_command_rule_id = add_exact_command_deny ? extract_rule_id(add_exact_command_deny->output[0]) : std::string{};
  expect(!exact_command_rule_id.empty(), "command dispatcher /permissions add exposes exact-command rule id secondarily");

  auto add_path_command_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=bash path=scripts "
                                                                                  "command=\"git status\" reason=\"path qualified bash\""});
  expect(add_path_command_deny && add_path_command_deny->handled && !add_path_command_deny->output.empty() &&
             add_path_command_deny->output[0].find("Block Exact command · scripts · Workspace") != std::string::npos &&
             add_path_command_deny->output[0].find("git status") == std::string::npos,
         "command dispatcher /permissions add path-qualifies RunCommand summaries");
  auto const path_command_rule_id = add_path_command_deny ? extract_rule_id(add_path_command_deny->output[0]) : std::string{};

  auto add_path_explore_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=task tool=task command=explore "
                                                                                  "path=src reason=\"path qualified explore\""});
  expect(add_path_explore_deny && add_path_explore_deny->handled && !add_path_explore_deny->output.empty() &&
             add_path_explore_deny->output[0].find("Block Explore subagents · src · Workspace") != std::string::npos,
         "command dispatcher /permissions add path-qualifies TaskRun Explore summaries");
  auto const path_explore_rule_id = add_path_explore_deny ? extract_rule_id(add_path_explore_deny->output[0]) : std::string{};

  auto add_forged_task_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=task tool=task "
                                                                                  "command=\"rm -rf /tmp/forged\" reason=\"forged task body\""});
  expect(add_forged_task_deny && add_forged_task_deny->handled && !add_forged_task_deny->output.empty() &&
             add_forged_task_deny->output[0].find("Block subagents · Workspace") != std::string::npos &&
             add_forged_task_deny->output[0].find("rm -rf") == std::string::npos && add_forged_task_deny->output[0].find("/tmp/forged") == std::string::npos,
         "command dispatcher /permissions add never prints free-form TaskRun command bodies");
  auto const forged_task_rule_id = add_forged_task_deny ? extract_rule_id(add_forged_task_deny->output[0]) : std::string{};

  auto add_typed_task_allow =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=allow operation=task tool=task command=coder "
                                                                                  "reason=\"typed coder subagent\""});
  expect(add_typed_task_allow && add_typed_task_allow->handled && !add_typed_task_allow->output.empty() &&
             add_typed_task_allow->output[0].find("Allow subagents · coder · Workspace") != std::string::npos,
         "command dispatcher /permissions add appends only identifier-like TaskRun type qualifiers");
  auto const typed_task_rule_id = add_typed_task_allow ? extract_rule_id(add_typed_task_allow->output[0]) : std::string{};

  auto add_explore_explore_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=task tool=explore command=explore "
                                                                                  "reason=\"agreeing explore identity\""});
  expect(add_explore_explore_deny && add_explore_explore_deny->handled && !add_explore_explore_deny->output.empty() &&
             add_explore_explore_deny->output[0].find("Block Explore subagents · Workspace") != std::string::npos,
         "command dispatcher /permissions add claims Explore when command and tool both agree on explore");
  auto const explore_explore_rule_id = add_explore_explore_deny ? extract_rule_id(add_explore_explore_deny->output[0]) : std::string{};

  auto add_tool_only_explore_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=task tool=explore "
                                                                                  "reason=\"tool-only explore must not claim class\""});
  expect(add_tool_only_explore_deny && add_tool_only_explore_deny->handled && !add_tool_only_explore_deny->output.empty() &&
             add_tool_only_explore_deny->output[0].find("Block subagents · explore · Workspace") != std::string::npos &&
             add_tool_only_explore_deny->output[0].find("Block Explore subagents") == std::string::npos,
         "command dispatcher /permissions add keeps subagents class for tool-only explore identity");
  auto const tool_only_explore_rule_id = add_tool_only_explore_deny ? extract_rule_id(add_tool_only_explore_deny->output[0]) : std::string{};

  auto add_freeform_explore_tool_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=task tool=explore "
                                                                                  "command=\"rm -rf /tmp/rm-secret\" reason=\"free-form plus explore tool\""});
  expect(add_freeform_explore_tool_deny && add_freeform_explore_tool_deny->handled && !add_freeform_explore_tool_deny->output.empty() &&
             add_freeform_explore_tool_deny->output[0].find("Block subagents · explore · Workspace") != std::string::npos &&
             add_freeform_explore_tool_deny->output[0].find("Block Explore subagents") == std::string::npos &&
             add_freeform_explore_tool_deny->output[0].find("rm -rf") == std::string::npos &&
             add_freeform_explore_tool_deny->output[0].find("rm-secret") == std::string::npos &&
             add_freeform_explore_tool_deny->output[0].find("/tmp/") == std::string::npos,
         "command dispatcher /permissions add keeps subagents class for free-form command with explore tool and omits body");
  auto const freeform_explore_tool_rule_id = add_freeform_explore_tool_deny ? extract_rule_id(add_freeform_explore_tool_deny->output[0]) : std::string{};

  auto add_mixed_coder_explore_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=task tool=explore command=coder "
                                                                                  "reason=\"mixed coder command explore tool\""});
  expect(add_mixed_coder_explore_deny && add_mixed_coder_explore_deny->handled && !add_mixed_coder_explore_deny->output.empty() &&
             add_mixed_coder_explore_deny->output[0].find("Block subagents · coder · Workspace") != std::string::npos &&
             add_mixed_coder_explore_deny->output[0].find("Block Explore subagents") == std::string::npos,
         "command dispatcher /permissions add keeps subagents class for coder command with explore tool");
  auto const mixed_coder_explore_rule_id = add_mixed_coder_explore_deny ? extract_rule_id(add_mixed_coder_explore_deny->output[0]) : std::string{};

  auto add_mixed_explore_coder_deny =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=deny operation=task tool=coder command=explore "
                                                                                  "reason=\"mixed explore command conflicting tool\""});
  expect(add_mixed_explore_coder_deny && add_mixed_explore_coder_deny->handled && !add_mixed_explore_coder_deny->output.empty() &&
             add_mixed_explore_coder_deny->output[0].find("Block subagents · explore · Workspace") != std::string::npos &&
             add_mixed_explore_coder_deny->output[0].find("Block Explore subagents") == std::string::npos,
         "command dispatcher /permissions add keeps subagents class for explore command with conflicting tool");
  auto const mixed_explore_coder_rule_id = add_mixed_explore_coder_deny ? extract_rule_id(add_mixed_explore_coder_deny->output[0]) : std::string{};

  {
    ava::permissions::PersistentPermissionRule explore_empty_tool{
        .rule_id = "permrule_display_explore_empty_tool",
        .scope = ava::permissions::PermissionRuleScope::Workspace,
        .workspace_dir = {},
        .action = ava::permissions::PermissionAction::Deny,
        .operation = ava::permissions::Operation::TaskRun,
        .mode = ava::permissions::PermissionRuleMode::Any,
        .tool_name = {},
        .target_path = {},
        .command = "explore",
        .command_recipe_key = {},
        .recipe_display = {},
        .critical_acknowledged = false,
        .schema_version = ava::permissions::kCurrentPermissionRulesSchemaVersion,
        .reason = "explore with empty tool",
        .actor = {},
        .created_at = {},
    };
    expect(ava::app::format_permission_rule_summary(explore_empty_tool, session_workspace_dir()) == "Block Explore subagents · Workspace",
           "permission rule summary claims Explore for command=explore with empty tool_name");
  }

  auto add_mcp_read_collision =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=allow operation=mcp.tool.call tool=read "
                                                                                  "reason=\"mcp read collision\""});
  expect(add_mcp_read_collision && add_mcp_read_collision->handled && !add_mcp_read_collision->output.empty() &&
             add_mcp_read_collision->output[0].find("Allow MCP tool calls · read · Workspace") != std::string::npos &&
             add_mcp_read_collision->output[0].find("Allow file reads") == std::string::npos &&
             add_mcp_read_collision->output[0].find("Allow read ·") == std::string::npos,
         "command dispatcher /permissions add keeps MCP operation class when tool_name collides with read");
  auto const mcp_read_rule_id = add_mcp_read_collision ? extract_rule_id(add_mcp_read_collision->output[0]) : std::string{};

  auto add_plugin_bash_collision =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=allow operation=plugin.tool.call tool=bash "
                                                                                  "reason=\"plugin bash collision\""});
  expect(add_plugin_bash_collision && add_plugin_bash_collision->handled && !add_plugin_bash_collision->output.empty() &&
             add_plugin_bash_collision->output[0].find("Allow plugin tool calls · bash · Workspace") != std::string::npos &&
             add_plugin_bash_collision->output[0].find("Allow shell commands") == std::string::npos &&
             add_plugin_bash_collision->output[0].find("Allow bash ·") == std::string::npos,
         "command dispatcher /permissions add keeps plugin operation class when tool_name collides with bash");
  auto const plugin_bash_rule_id = add_plugin_bash_collision ? extract_rule_id(add_plugin_bash_collision->output[0]) : std::string{};

  auto add_forged_recipe_allow = ava::app::run_command(
      unlocked_session,
      ava::app::CommandRequest{.command = "/permissions add action=allow operation=bash "
                                          "recipe_key=sha256:ava-command-workspace-recipe-v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
                                          "recipe_display=\"shell commands\" reason=\"forged recipe subject\""});
  expect(add_forged_recipe_allow && add_forged_recipe_allow->handled && !add_forged_recipe_allow->output.empty() &&
             add_forged_recipe_allow->output[0].find("Allow recipe · shell commands · Workspace") != std::string::npos &&
             add_forged_recipe_allow->output[0].find("Allow shell commands · Workspace") == std::string::npos &&
             add_forged_recipe_allow->output[0].find("sha256:ava-command-workspace-recipe-v1:") == std::string::npos &&
             add_forged_recipe_allow->output[0].find("aaaaaaaaaaaaaaaa") == std::string::npos,
         "command dispatcher /permissions add frames recipe_display under fixed recipe class and omits recipe key hashes");
  auto const forged_recipe_rule_id = add_forged_recipe_allow ? extract_rule_id(add_forged_recipe_allow->output[0]) : std::string{};

  {
    std::string long_unicode_display;
    long_unicode_display.reserve(80 * 3);
    for (int index = 0; index < 80; ++index)
      long_unicode_display += "你";
    ava::permissions::PersistentPermissionRule long_recipe_rule{
        .rule_id = "permrule_display_only",
        .scope = ava::permissions::PermissionRuleScope::Workspace,
        .workspace_dir = {},
        .action = ava::permissions::PermissionAction::Allow,
        .operation = ava::permissions::Operation::RunCommand,
        .mode = ava::permissions::PermissionRuleMode::Any,
        .tool_name = {},
        .target_path = {},
        .command = {},
        .command_recipe_key = "sha256:ava-command-workspace-recipe-v1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        .recipe_display = long_unicode_display,
        .critical_acknowledged = false,
        .schema_version = ava::permissions::kCurrentPermissionRulesSchemaVersion,
        .reason = "unicode recipe bound",
        .actor = {},
        .created_at = {},
    };
    auto const long_summary = ava::app::format_permission_rule_summary(long_recipe_rule, session_workspace_dir());
    expect(long_summary.starts_with("Allow recipe · ") && long_summary.find(" · Workspace") != std::string::npos &&
               long_summary.find(long_unicode_display) == std::string::npos && long_summary.find("bbbbbbbb") == std::string::npos &&
               long_summary.size() < long_unicode_display.size(),
           "permission rule summary bounds long Unicode recipe_display without exposing recipe keys");
    expect(long_summary.find("你") != std::string::npos, "permission rule summary retains a UTF-8-safe recipe_display prefix");
  }

  {
    ava::permissions::PersistentPermissionRule network_read_collision{
        .rule_id = "permrule_display_network",
        .scope = ava::permissions::PermissionRuleScope::Workspace,
        .workspace_dir = {},
        .action = ava::permissions::PermissionAction::Allow,
        .operation = ava::permissions::Operation::NetworkFetch,
        .mode = ava::permissions::PermissionRuleMode::Any,
        .tool_name = "read",
        .target_path = {},
        .command = "https://example.test",
        .command_recipe_key = {},
        .recipe_display = {},
        .critical_acknowledged = false,
        .schema_version = ava::permissions::kCurrentPermissionRulesSchemaVersion,
        .reason = "network read collision",
        .actor = {},
        .created_at = {},
    };
    auto const network_summary = ava::app::format_permission_rule_summary(network_read_collision, session_workspace_dir());
    expect(network_summary == "Allow network fetches · read · Workspace",
           "permission rule summary keeps network operation class when tool_name collides with read");
  }

  auto permissions_list = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions list trusted"});
  expect(permissions_list && permissions_list->handled && !permissions_list->output.empty() &&
             permissions_list->output[0].find("1. Allow file reads · src/main.cpp · Workspace") != std::string::npos &&
             permissions_list->output[0].find(permission_rule_id) == std::string::npos &&
             permissions_list->output[0].find("built-in hard denies run first") != std::string::npos &&
             permissions_list->output[0].find("ordinals are not remove authority") != std::string::npos &&
             permissions_list->output[0].find("/permissions explain or /permissions remove") != std::string::npos,
         "command dispatcher /permissions list filters by reason privately and shows human ordinals only");

  auto permissions_list_human = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions list file reads"});
  expect(permissions_list_human && permissions_list_human->handled && !permissions_list_human->output.empty() &&
             permissions_list_human->output[0].find("Allow file reads · src/main.cpp · Workspace") != std::string::npos &&
             permissions_list_human->output[0].find("Allow file reads · /etc/hosts · Global") != std::string::npos &&
             permissions_list_human->output[0].find("Block Explore subagents") == std::string::npos,
         "command dispatcher /permissions list filters by human summary words");

  auto permissions_list_command_private =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions list git push origin main"});
  expect(permissions_list_command_private && permissions_list_command_private->handled && !permissions_list_command_private->output.empty() &&
             permissions_list_command_private->output[0].find("filter: git push origin main") != std::string::npos &&
             permissions_list_command_private->output[0].find("1. Block Exact command · Workspace") != std::string::npos &&
             permissions_list_command_private->output[0].find("command=\"") == std::string::npos &&
             permissions_list_command_private->output[0].find(exact_command_rule_id) == std::string::npos &&
             permissions_list_command_private->output[0].find("recipe_key=") == std::string::npos &&
             permissions_list_command_private->output[0].find("permrule_") == std::string::npos,
         "command dispatcher /permissions list can match command text privately without printing rule bodies");

  auto permissions_list_all = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions list"});
  expect(permissions_list_all && permissions_list_all->handled && !permissions_list_all->output.empty() &&
             permissions_list_all->output[0].find("1. ") != std::string::npos && permissions_list_all->output[0].find("2. ") != std::string::npos &&
             permissions_list_all->output[0].find("Block Explore subagents · Workspace") != std::string::npos &&
             permissions_list_all->output[0].find("Block skills · Workspace") != std::string::npos &&
             permissions_list_all->output[0].find("Block Exact command · scripts · Workspace") != std::string::npos &&
             permissions_list_all->output[0].find("Block Explore subagents · src · Workspace") != std::string::npos &&
             permissions_list_all->output[0].find("Allow MCP tool calls · read · Workspace") != std::string::npos &&
             permissions_list_all->output[0].find("Allow plugin tool calls · bash · Workspace") != std::string::npos &&
             permissions_list_all->output[0].find("Allow recipe · shell commands · Workspace") != std::string::npos &&
             permissions_list_all->output[0].find("permrule_") == std::string::npos &&
             permissions_list_all->output[0].find("command=\"") == std::string::npos &&
             permissions_list_all->output[0].find("sha256:ava-command-workspace-recipe-v1:") == std::string::npos,
         "command dispatcher /permissions list leads with stable human ordinals and omits rule ids and command bodies");

  auto permissions_explain = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permission-rules explain " + permission_rule_id});
  expect(permissions_explain && permissions_explain->handled && !permissions_explain->output.empty() &&
             permissions_explain->output[0].find("Allow file reads · src/main.cpp · Workspace") != std::string::npos &&
             permissions_explain->output[0].find("rule id: " + permission_rule_id) != std::string::npos &&
             permissions_explain->output[0].find("path: src/main.cpp") != std::string::npos &&
             permissions_explain->output[0].find("matching: exact operation") != std::string::npos &&
             permissions_explain->output[0].find("precedence: built-in hard denies run first") != std::string::npos,
         "command dispatcher /permissions explain leads with human summary and retains authority details");

  auto permissions_explain_command =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions explain " + exact_command_rule_id});
  expect(permissions_explain_command && permissions_explain_command->handled && !permissions_explain_command->output.empty() &&
             permissions_explain_command->output[0].find("Block Exact command · Workspace") != std::string::npos &&
             permissions_explain_command->output[0].find("command: git push origin main") != std::string::npos &&
             permissions_explain_command->output[0].find("rule id: " + exact_command_rule_id) != std::string::npos,
         "command dispatcher /permissions explain retains exact command authority details");

  auto permissions_remove_ordinal = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions remove 1"});
  expect(permissions_remove_ordinal && permissions_remove_ordinal->handled && !permissions_remove_ordinal->output.empty() &&
             permissions_remove_ordinal->output[0].find("list ordinals are display-only") != std::string::npos,
         "command dispatcher /permissions remove rejects display ordinals");

  auto permissions_explain_ordinal = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions explain 1"});
  expect(permissions_explain_ordinal && permissions_explain_ordinal->handled && !permissions_explain_ordinal->output.empty() &&
             permissions_explain_ordinal->output[0].find("list ordinals are display-only") != std::string::npos,
         "command dispatcher /permissions explain rejects display ordinals");

  auto permissions_diagnose = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/perms diagnose"});
  expect(permissions_diagnose && permissions_diagnose->handled && !permissions_diagnose->output.empty() &&
             permissions_diagnose->output[0].find("loaded rules: 17") != std::string::npos &&
             permissions_diagnose->output[0].find("outside the model-writable workspace") != std::string::npos,
         "command dispatcher /permissions diagnose reports storage and fail-closed behavior");
  auto const append_route = ava::app::runtime::session_ts::rat(unlocked_session)->owner_append_route_1();
  auto append_permission_audit =
      ava::agent::append_permission_decision(append_route, ava::tools::PermissionAuditEvent{.permission_request_id = "permreq_runtime_deny",
                                                                                            .operation = ava::permissions::Operation::RunCommand,
                                                                                            .mode = ava::agent::Mode::Build,
                                                                                            .tool_name = "bash",
                                                                                            .action = ava::permissions::PermissionAction::Deny,
                                                                                            .reason = "command can change external or destructive state",
                                                                                            .risk = ava::permissions::PermissionRisk::High,
                                                                                            .command = "git push origin main",
                                                                                            .resolution = "deny",
                                                                                            .resolution_source = "resolver",
                                                                                            .resolution_reason = "remembered deny | rule",
                                                                                            .actor = "tui",
                                                                                            .rule_id = permission_rule_id});
  expect(append_permission_audit.has_value(), append_permission_audit
                                                  ? "command dispatcher test appends a permission audit entry"
                                                  : "command dispatcher test appends a permission audit entry: " + append_permission_audit.error().format());
  auto permissions_audit = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions audit permreq_runtime"});
  expect(permissions_audit && permissions_audit->handled && !permissions_audit->output.empty() &&
             permissions_audit->output[0].find("Permission audit:") != std::string::npos &&
             permissions_audit->output[0].find("permreq_runtime_deny") != std::string::npos &&
             permissions_audit->output[0].find("command=\"<redacted one-shot command>\"") != std::string::npos &&
             permissions_audit->output[0].find("source=resolver") != std::string::npos &&
             permissions_audit->output[0].find("git push origin main") == std::string::npos &&
             permissions_audit->output[0].find("remembered deny | rule") == std::string::npos,
         "command dispatcher /permissions audit filters persisted permission decisions");
  auto permissions_audit_summary = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions audit summary permreq_runtime"});
  expect(permissions_audit_summary && permissions_audit_summary->handled && !permissions_audit_summary->output.empty() &&
             permissions_audit_summary->output[0].find("Permission audit summary:") != std::string::npos &&
             permissions_audit_summary->output[0].find("filter: permreq_runtime") != std::string::npos &&
             permissions_audit_summary->output[0].find("entries: 1 matching") != std::string::npos &&
             permissions_audit_summary->output[0].find("denials: 1") != std::string::npos &&
             permissions_audit_summary->output[0].find("by action: deny=1") != std::string::npos &&
             permissions_audit_summary->output[0].find("by resolution: deny=1") != std::string::npos &&
             permissions_audit_summary->output[0].find("by source: resolver=1") != std::string::npos &&
             permissions_audit_summary->output[0].find("by risk: high=1") != std::string::npos &&
             permissions_audit_summary->output[0].find("by tool: bash=1") != std::string::npos &&
             permissions_audit_summary->output[0].find("/permissions audit show permreq_runtime_deny") != std::string::npos,
         "command dispatcher /permissions audit summary groups matching permission decisions for browsing");
  auto permissions_audit_export = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions audit export permreq_runtime"});
  expect(permissions_audit_export && permissions_audit_export->handled && !permissions_audit_export->output.empty() &&
             permissions_audit_export->output[0].find("Permission audit export:") != std::string::npos &&
             permissions_audit_export->output[0].find("format: markdown table") != std::string::npos &&
             permissions_audit_export->output[0].find("| timestamp | entry | request | action | resolution |") != std::string::npos &&
             permissions_audit_export->output[0].find("| deny |") != std::string::npos &&
             permissions_audit_export->output[0].find("<redacted one-shot command>") != std::string::npos &&
             permissions_audit_export->output[0].find("git push origin main") == std::string::npos &&
             permissions_audit_export->output[0].find("remembered deny") == std::string::npos,
         "command dispatcher /permissions audit export renders copyable markdown and escapes table cells");
  auto permissions_diagnose_denial = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions diagnose permreq_runtime"});
  expect(permissions_diagnose_denial && permissions_diagnose_denial->handled && !permissions_diagnose_denial->output.empty() &&
             permissions_diagnose_denial->output[0].find("Permission rule diagnostics:") != std::string::npos &&
             permissions_diagnose_denial->output[0].find("Recent permission denials:") != std::string::npos &&
             permissions_diagnose_denial->output[0].find("decision=deny") != std::string::npos &&
             permissions_diagnose_denial->output[0].find("source=resolver") != std::string::npos &&
             permissions_diagnose_denial->output[0].find("command=\"<redacted one-shot command>\"") != std::string::npos &&
             permissions_diagnose_denial->output[0].find("reason: command can change external or destructive state") != std::string::npos &&
             permissions_diagnose_denial->output[0].find("git push origin main") == std::string::npos &&
             permissions_diagnose_denial->output[0].find("remembered deny | rule") == std::string::npos &&
             permissions_diagnose_denial->output[0].find("next: /permissions explain " + permission_rule_id) != std::string::npos,
         "command dispatcher /permissions diagnose explains recent denied decisions with follow-up commands");
  auto permissions_audit_detail = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions audit show permreq_runtime"});
  expect(permissions_audit_detail && permissions_audit_detail->handled && !permissions_audit_detail->output.empty() &&
             permissions_audit_detail->output[0].find("Permission audit detail:") != std::string::npos &&
             permissions_audit_detail->output[0].find("selector: permreq_runtime") != std::string::npos &&
             permissions_audit_detail->output[0].find("matched entries: 1") != std::string::npos &&
             permissions_audit_detail->output[0].find("request: permreq_runtime_deny") != std::string::npos &&
             permissions_audit_detail->output[0].find("command: <redacted one-shot command>") != std::string::npos &&
             permissions_audit_detail->output[0].find("git push origin main") == std::string::npos &&
             permissions_audit_detail->output[0].find("remembered deny | rule") == std::string::npos &&
             permissions_audit_detail->output[0].find("/permissions audit export permreq_runtime") != std::string::npos &&
             permissions_audit_detail->output[0].find("/permissions explain " + permission_rule_id) != std::string::npos,
         "command dispatcher /permissions audit show drills into a permission request by id prefix");
  auto permissions_audit_empty = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions audit unmatched-query"});
  expect(permissions_audit_empty && permissions_audit_empty->handled && !permissions_audit_empty->output.empty() &&
             permissions_audit_empty->output[0].find("No permission audit entries match") != std::string::npos,
         "command dispatcher /permissions audit reports empty filtered results");
  auto slash_items_after_permission_rule = ava::app::command_catalog_slash_items_1(unlocked_session, custom_hotkeys);
  auto const* permissions_completion_item = tui_test_support::find_slash_command_item(slash_items_after_permission_rule, "/permissions");
  auto const* hotkeys_completion_item = tui_test_support::find_slash_command_item(slash_items_after_permission_rule, "/hotkeys");
  auto find_argument_completion = [](ava::tui::SlashCommandItem const* item, std::size_t argument_index, std::string_view value,
                                     std::vector<std::string> const& previous_args) -> ava::tui::SlashCommandArgumentCompletion const* {
    if (item == nullptr)
      return nullptr;
    for (auto const& completion : item->argument_completions)
    {
      if (tui_test_support::slash_argument_completion_matches(completion, argument_index, value, previous_args))
        return &completion;
    }
    return nullptr;
  };
  // Production seam: short refs start at 6 payload chars and extend only on collision.
  {
    auto const collision_refs = ava::app::unique_short_id_refs({"permrule_abcdef111111", "permrule_abcdef222222", "permrule_zzzzzz999999"});
    expect(collision_refs.size() == 3 && collision_refs[0] == "abcdef1" && collision_refs[1] == "abcdef2" && collision_refs[2] == "zzzzzz" &&
               ava::app::id_payload_after_family_prefix("permrule_abcdef111111") == "abcdef111111" &&
               ava::app::unique_short_id_refs({"job_aaaaaa111", "job_aaaaaa111"}).size() == 2 &&
               ava::app::unique_short_id_refs({"job_aaaaaa111", "job_aaaaaa111"})[0] == ava::app::unique_short_id_refs({"job_aaaaaa111", "job_aaaaaa111"})[1],
           "unique_short_id_refs extends past six chars only until the displayed set is unique and stays stable for identical ids");
  }

  auto extract_completion_ref = [](std::string_view description) -> std::string {
    constexpr std::string_view kMarker = "ref ";
    auto const pos = description.rfind(kMarker);
    if (pos == std::string_view::npos)
      return {};
    return std::string(description.substr(pos + kMarker.size()));
  };
  auto match_index_for_value = [](std::vector<ava::tui::SlashCommandItem> const& matches, std::string_view value) -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < matches.size(); ++index)
    {
      if (matches[index].command == value)
        return index;
    }
    return std::nullopt;
  };

  auto const* permission_explain = find_argument_completion(permissions_completion_item, 1, permission_rule_id, {"explain"});
  auto const* permission_remove = find_argument_completion(permissions_completion_item, 1, permission_rule_id, {"remove"});
  constexpr char kPermissionSummary[] = "Allow file reads · src/main.cpp · Workspace";
  auto const permission_matches = ava::tui::filter_slash_commands("/permissions explain ", slash_items_after_permission_rule);
  auto const permission_match_index = match_index_for_value(permission_matches, permission_rule_id);
  auto const permission_selection =
      permission_match_index ? ava::tui::slash_command_selection_text("/permissions explain ", slash_items_after_permission_rule, *permission_match_index)
                             : std::string{};
  auto const permission_palette = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                       .provider = "openai",
                                                                                       .model = "gpt-5.5",
                                                                                       .session_id = "session_test",
                                                                                       .input = "/permissions explain ",
                                                                                       .status = "ready",
                                                                                       .transcript = {},
                                                                                       .slash_commands = slash_items_after_permission_rule,
                                                                                       .selected_slash_command_index = permission_match_index.value_or(0),
                                                                                       .width = 96,
                                                                                       .height = 14});
  auto const permission_ref = permission_explain ? extract_completion_ref(permission_explain->description) : std::string{};
  expect(
      permission_explain != nullptr && permission_remove != nullptr && permission_explain->value == permission_rule_id &&
          permission_remove->value == permission_rule_id && permission_explain->display_label == kPermissionSummary &&
          permission_remove->display_label == kPermissionSummary && permission_explain->display_label.find("permrule_") == std::string::npos &&
          permission_remove->display_label.find("permrule_") == std::string::npos && permission_explain->description.find("permrule_") == std::string::npos &&
          permission_remove->description.find("permrule_") == std::string::npos && permission_explain->description.starts_with("Explain rule · ref ") &&
          permission_remove->description.starts_with("Remove rule · ref ") && !permission_ref.empty() && permission_ref.size() >= 6 &&
          permission_explain->description.find(permission_rule_id) == std::string::npos && permission_match_index.has_value() &&
          permission_matches[*permission_match_index].display_label == kPermissionSummary &&
          permission_matches[*permission_match_index].display_label.find("permrule_") == std::string::npos &&
          permission_selection == "/permissions explain " + permission_rule_id &&
          std::ranges::none_of(permission_palette,
                               [](std::string const& line) {
                                 auto const visible = strip_sgr(line);
                                 return visible.find("[complete]") != std::string::npos || visible.find("permrule_") != std::string::npos;
                               }) &&
          std::ranges::any_of(permission_palette, [](std::string const& line) { return strip_sgr(line).find("Allow file reads") != std::string::npos; }) &&
          std::ranges::any_of(permission_palette, [&](std::string const& line) { return strip_sgr(line).find("ref " + permission_ref) != std::string::npos; }),
      "permission explain/remove completions keep exact rule ids as values while rendering human summaries plus short refs without raw ids or [complete]");

  // Two same-summary rules must stay visually distinct via short refs and still draft separate exact ids.
  auto add_duplicate_permission_rule =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions add action=allow operation=read path=src/main.cpp "
                                                                                  "reason=\"duplicate summary completion fixture\""});
  expect(add_duplicate_permission_rule && add_duplicate_permission_rule->handled && !add_duplicate_permission_rule->output.empty() &&
             add_duplicate_permission_rule->output[0].find(kPermissionSummary) != std::string::npos,
         "duplicate-summary permission rule fixture adds with the same human summary");
  auto const duplicate_permission_rule_id = add_duplicate_permission_rule ? extract_rule_id(add_duplicate_permission_rule->output[0]) : std::string{};
  expect(!duplicate_permission_rule_id.empty() && duplicate_permission_rule_id != permission_rule_id,
         "duplicate-summary permission rule fixture exposes a distinct authority id");
  auto const slash_items_duplicate_rules = ava::app::command_catalog_slash_items_1(unlocked_session, custom_hotkeys);
  auto const* permissions_duplicate_item = tui_test_support::find_slash_command_item(slash_items_duplicate_rules, "/permissions");
  auto const* permission_explain_a = find_argument_completion(permissions_duplicate_item, 1, permission_rule_id, {"explain"});
  auto const* permission_explain_b = find_argument_completion(permissions_duplicate_item, 1, duplicate_permission_rule_id, {"explain"});
  auto const* permission_remove_a = find_argument_completion(permissions_duplicate_item, 1, permission_rule_id, {"remove"});
  auto const* permission_remove_b = find_argument_completion(permissions_duplicate_item, 1, duplicate_permission_rule_id, {"remove"});
  auto const duplicate_permission_matches = ava::tui::filter_slash_commands("/permissions explain ", slash_items_duplicate_rules);
  auto const match_a = match_index_for_value(duplicate_permission_matches, permission_rule_id);
  auto const match_b = match_index_for_value(duplicate_permission_matches, duplicate_permission_rule_id);
  auto const selection_a = match_a ? ava::tui::slash_command_selection_text("/permissions explain ", slash_items_duplicate_rules, *match_a) : std::string{};
  auto const selection_b = match_b ? ava::tui::slash_command_selection_text("/permissions explain ", slash_items_duplicate_rules, *match_b) : std::string{};
  auto render_permission_palette = [&](std::size_t selected_index) {
    return ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                .provider = "openai",
                                                                .model = "gpt-5.5",
                                                                .session_id = "session_test",
                                                                .input = "/permissions explain ",
                                                                .status = "ready",
                                                                .transcript = {},
                                                                .slash_commands = slash_items_duplicate_rules,
                                                                .selected_slash_command_index = selected_index,
                                                                .width = 96,
                                                                .height = 16});
  };
  auto const duplicate_permission_palette_a = render_permission_palette(match_a.value_or(0));
  auto const duplicate_permission_palette_b = render_permission_palette(match_b.value_or(0));
  auto const ref_a = permission_explain_a ? extract_completion_ref(permission_explain_a->description) : std::string{};
  auto const ref_b = permission_explain_b ? extract_completion_ref(permission_explain_b->description) : std::string{};
  auto palette_hides_authority = [](std::vector<std::string> const& palette) {
    return std::ranges::none_of(palette, [](std::string const& line) {
      auto const visible = strip_sgr(line);
      return visible.find("[complete]") != std::string::npos || visible.find("permrule_") != std::string::npos;
    });
  };
  expect(permission_explain_a != nullptr && permission_explain_b != nullptr && permission_remove_a != nullptr && permission_remove_b != nullptr &&
             permission_explain_a->display_label == kPermissionSummary && permission_explain_b->display_label == kPermissionSummary &&
             permission_explain_a->value == permission_rule_id && permission_explain_b->value == duplicate_permission_rule_id &&
             permission_remove_a->description == "Remove rule · ref " + ref_a && permission_remove_b->description == "Remove rule · ref " + ref_b &&
             permission_explain_a->description == "Explain rule · ref " + ref_a && permission_explain_b->description == "Explain rule · ref " + ref_b &&
             !ref_a.empty() && !ref_b.empty() && ref_a != ref_b && permission_explain_a->description.find(permission_rule_id) == std::string::npos &&
             permission_explain_b->description.find(duplicate_permission_rule_id) == std::string::npos && match_a.has_value() && match_b.has_value() &&
             selection_a == "/permissions explain " + permission_rule_id && selection_b == "/permissions explain " + duplicate_permission_rule_id &&
             palette_hides_authority(duplicate_permission_palette_a) && palette_hides_authority(duplicate_permission_palette_b) &&
             std::ranges::any_of(duplicate_permission_palette_a,
                                 [&](std::string const& line) { return strip_sgr(line).find("ref " + ref_a) != std::string::npos; }) &&
             std::ranges::any_of(duplicate_permission_palette_b,
                                 [&](std::string const& line) { return strip_sgr(line).find("ref " + ref_b) != std::string::npos; }),
         "same-summary permission completions stay visually distinct via short refs and draft exact separate rule ids");

  auto const* submit_set = find_argument_completion(hotkeys_completion_item, 1, "submit", {"set"});
  auto const* submit_reset = find_argument_completion(hotkeys_completion_item, 1, "submit", {"reset"});
  auto const* submit_unset = find_argument_completion(hotkeys_completion_item, 1, "submit", {"unset"});
  auto const keybinding_matches = ava::tui::filter_slash_commands("/hotkeys set sub", slash_items_after_permission_rule);
  auto const keybinding_selection = ava::tui::slash_command_selection_text("/hotkeys set sub", slash_items_after_permission_rule, 0);
  expect(submit_set != nullptr && submit_reset != nullptr && submit_unset != nullptr && submit_set->display_label == "Submit custom" &&
             submit_reset->display_label == "Submit custom" && submit_unset->display_label == "Submit custom" &&
             submit_set->description.find("Ctrl+M") != std::string::npos && submit_set->description.find("submit") != std::string::npos &&
             submit_set->description.find("Submit custom (") == std::string::npos && !keybinding_matches.empty() &&
             keybinding_matches.front().display_label == "Submit custom" && keybinding_selection == "/hotkeys set submit " &&
             keybinding_selection.find("Submit custom") == std::string::npos,
         "keybinding set/reset/unset completions show human labels with keys-first descriptions while drafting canonical action ids");

  auto const coordinator_snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->subagent_coordinator(), session_r->store.session_id()};
  }();
  auto const& [coordinator, coordinator_session_id] = coordinator_snapshot;
  expect(coordinator != nullptr, "catalog human completion tests require a subagent coordinator");
  if (coordinator)
  {
    auto start_completed_job = [&](std::string title, std::string child_session_id, std::string subagent_type = "general") {
      return coordinator->start_background(coordinator_session_id,
                                           ava::agent::BackgroundJobStartOptions{
                                               .title = std::move(title),
                                               .description = "SECRET_PROMPT_MUST_NOT_APPEAR_IN_COMPLETION",
                                               .subagent_type = std::move(subagent_type),
                                               .child_session_id = std::move(child_session_id),
                                           },
                                           [](ava::agent::BackgroundJobContext const&) {
                                             return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Completed,
                                                                                        .final_text = "SECRET_SUMMARY_MUST_NOT_APPEAR",
                                                                                        .stop_reason = "completed"};
                                           });
    };

    auto started = start_completed_job("Review pull request", "child_catalog_completion_job");
    expect(started.has_value(), started ? "catalog completion job starts" : "catalog completion job starts: " + started.error().format());
    auto started_twin = start_completed_job("Review pull request", "child_catalog_completion_job_twin");
    expect(started_twin.has_value(),
           started_twin ? "catalog completion twin job starts" : "catalog completion twin job starts: " + started_twin.error().format());
    if (started && started_twin)
    {
      auto waited = coordinator->wait(coordinator_session_id, started->job.identity.job_id, std::chrono::seconds(2));
      auto waited_twin = coordinator->wait(coordinator_session_id, started_twin->job.identity.job_id, std::chrono::seconds(2));
      expect(waited && !waited->timed_out && waited_twin && !waited_twin->timed_out, "catalog completion jobs reach terminal state");
      auto const job_id = started->job.identity.job_id;
      auto const job_id_twin = started_twin->job.identity.job_id;
      auto const slash_items_with_job = ava::app::command_catalog_slash_items_1(unlocked_session, custom_hotkeys);
      auto const* jobs_item = tui_test_support::find_slash_command_item(slash_items_with_job, "/jobs");
      auto const* job_show = find_argument_completion(jobs_item, 1, job_id, {"show"});
      auto const* job_show_twin = find_argument_completion(jobs_item, 1, job_id_twin, {"show"});
      auto const* job_wait = find_argument_completion(jobs_item, 1, job_id, {"wait"});
      auto const* job_result = find_argument_completion(jobs_item, 1, job_id, {"result"});
      auto const* job_cancel = find_argument_completion(jobs_item, 1, job_id, {"cancel"});
      auto const job_matches = ava::tui::filter_slash_commands("/jobs show ", slash_items_with_job);
      auto const job_match = match_index_for_value(job_matches, job_id);
      auto const job_match_twin = match_index_for_value(job_matches, job_id_twin);
      auto const job_selection = job_match ? ava::tui::slash_command_selection_text("/jobs show ", slash_items_with_job, *job_match) : std::string{};
      auto const job_selection_twin =
          job_match_twin ? ava::tui::slash_command_selection_text("/jobs show ", slash_items_with_job, *job_match_twin) : std::string{};
      auto const job_palette = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                    .provider = "openai",
                                                                                    .model = "gpt-5.5",
                                                                                    .session_id = "session_test",
                                                                                    .input = "/jobs show ",
                                                                                    .status = "ready",
                                                                                    .transcript = {},
                                                                                    .slash_commands = slash_items_with_job,
                                                                                    .selected_slash_command_index = job_match.value_or(0),
                                                                                    .width = 96,
                                                                                    .height = 14});
      auto const job_ref = job_show ? extract_completion_ref(job_show->description) : std::string{};
      auto const job_ref_twin = job_show_twin ? extract_completion_ref(job_show_twin->description) : std::string{};
      expect(job_show != nullptr && job_show_twin != nullptr && job_wait != nullptr && job_result != nullptr && job_cancel != nullptr &&
                 job_show->value == job_id && job_show_twin->value == job_id_twin && job_show->display_label == "Review pull request" &&
                 job_show_twin->display_label == "Review pull request" && job_wait->display_label == "Review pull request" &&
                 job_result->display_label == "Review pull request" && job_cancel->display_label == "Review pull request" &&
                 job_show->display_label.find(job_id) == std::string::npos && job_show->description.find("Completed") != std::string::npos &&
                 job_show->description.find("Background") != std::string::npos && job_show->description.find("general") != std::string::npos &&
                 job_show->description.find(" · ref ") != std::string::npos && !job_ref.empty() && !job_ref_twin.empty() && job_ref != job_ref_twin &&
                 job_show->description.find(job_id) == std::string::npos && job_show_twin->description.find(job_id_twin) == std::string::npos &&
                 job_show->description.find("SECRET_PROMPT_MUST_NOT_APPEAR_IN_COMPLETION") == std::string::npos &&
                 job_show->description.find("SECRET_SUMMARY_MUST_NOT_APPEAR") == std::string::npos && job_match.has_value() && job_match_twin.has_value() &&
                 job_matches[*job_match].display_label == "Review pull request" && job_matches[*job_match_twin].display_label == "Review pull request" &&
                 job_selection == "/jobs show " + job_id && job_selection_twin == "/jobs show " + job_id_twin &&
                 std::ranges::none_of(job_palette,
                                      [&](std::string const& line) {
                                        auto const visible = strip_sgr(line);
                                        return visible.find("[complete]") != std::string::npos || visible.find(job_id) != std::string::npos ||
                                               visible.find(job_id_twin) != std::string::npos ||
                                               visible.find("SECRET_PROMPT_MUST_NOT_APPEAR_IN_COMPLETION") != std::string::npos ||
                                               visible.find("SECRET_SUMMARY_MUST_NOT_APPEAR") != std::string::npos;
                                      }) &&
                 std::ranges::any_of(job_palette, [](std::string const& line) { return strip_sgr(line).find("Review pull request") != std::string::npos; }) &&
                 std::ranges::any_of(job_palette, [&](std::string const& line) { return strip_sgr(line).find("ref " + job_ref) != std::string::npos; }) &&
                 std::ranges::any_of(job_palette, [&](std::string const& line) { return strip_sgr(line).find("ref " + job_ref_twin) != std::string::npos; }),
             "same-title job completions keep exact job ids as values while rendering titles plus distinct short refs without prompts, summaries, raw ids, or "
             "[complete]");

      // Empty-title fallback stays human (Background job / Foreground job) and never promotes the raw id.
      auto untitled = start_completed_job({}, "child_catalog_completion_untitled", "explore");
      expect(untitled.has_value(),
             untitled ? "untitled catalog completion job starts" : "untitled catalog completion job starts: " + untitled.error().format());
      if (untitled)
      {
        auto const untitled_id = untitled->job.identity.job_id;
        auto const slash_items_untitled = ava::app::command_catalog_slash_items_1(unlocked_session, custom_hotkeys);
        auto const* jobs_untitled_item = tui_test_support::find_slash_command_item(slash_items_untitled, "/jobs");
        auto const* untitled_show = find_argument_completion(jobs_untitled_item, 1, untitled_id, {"show"});
        auto const untitled_ref = untitled_show ? extract_completion_ref(untitled_show->description) : std::string{};
        expect(untitled_show != nullptr && untitled_show->value == untitled_id && untitled_show->display_label == "Background job" &&
                   untitled_show->display_label.find(untitled_id) == std::string::npos && untitled_show->description.find("explore") != std::string::npos &&
                   untitled_show->description.find(" · ref ") != std::string::npos && !untitled_ref.empty() &&
                   untitled_show->description.find(untitled_id) == std::string::npos,
               "job completions fall back to Background job plus short ref rather than raw ids when process-local title is empty");
      }
    }
  }
  auto remove_permission_rule = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions remove " + permission_rule_id});
  expect(remove_permission_rule && remove_permission_rule->handled && !remove_permission_rule->output.empty() &&
             remove_permission_rule->output[0].find("removed permission rule") != std::string::npos &&
             remove_permission_rule->output[0].find("Allow file reads · src/main.cpp · Workspace") != std::string::npos &&
             remove_permission_rule->output[0].find("Rule ID: " + permission_rule_id) != std::string::npos,
         "command dispatcher /permissions remove deletes persistent rules by exact id with human receipt");

  for (auto const& rule_id :
       {duplicate_permission_rule_id, explore_rule_id, skill_rule_id, global_read_rule_id, exact_command_rule_id, path_command_rule_id, path_explore_rule_id,
        forged_task_rule_id, typed_task_rule_id, explore_explore_rule_id, tool_only_explore_rule_id, freeform_explore_tool_rule_id, mixed_coder_explore_rule_id,
        mixed_explore_coder_rule_id, mcp_read_rule_id, plugin_bash_rule_id, forged_recipe_rule_id})
  {
    auto removed = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions remove " + rule_id});
    expect(removed && removed->handled && !removed->output.empty() && removed->output[0].find("Rule ID: " + rule_id) != std::string::npos,
           "command dispatcher /permissions remove deletes remaining fixture rules by exact id");
  }

  auto permissions_after_remove = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/permissions list"});
  expect(permissions_after_remove && permissions_after_remove->handled && !permissions_after_remove->output.empty() &&
             permissions_after_remove->output[0].find("No persistent permission rules") != std::string::npos,
         "command dispatcher /permissions list reflects removed persistent rules");
}

}  // namespace ava::tests::app_runtime_tests
