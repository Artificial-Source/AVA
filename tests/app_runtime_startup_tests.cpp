#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/http/transport.h"
#include "ava/app/commands.h"
#include "ava/app/interactive_internal.h"
#include "ava/app/project_trust.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/subagent_delivery_manager.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/message_builder.h"
#include "ava/agent/mode.h"
#include "ava/agent/tool_types.h"
#include "ava/session/assistant_output.h"
#include "ava/session/compaction.h"
#include "ava/session/record.h"
#include "ava/session/session_metadata.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/provider.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/core/error.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

void test_extension_resource_policy_derives_synthetic_paths_and_trust()
{
  std::filesystem::path const home = "/synthetic-extension-policy/home-root";
  ScopedEnvVar home_env("HOME", home.string());
  ava::config::XdgPaths paths;
  paths.ava_config_dir = "/synthetic-extension-policy/config-root";
  paths.ava_state_dir = "/synthetic-extension-policy/state-root";
  std::filesystem::path const workspace = "/synthetic-extension-policy/workspace-root";
  auto const expected_global_skill_dirs =
      std::vector<std::filesystem::path>{paths.ava_config_dir / "skills", home / ".agents" / "skills", home / ".claude" / "skills"};
  auto const expected_project_skill_dirs =
      std::vector<std::filesystem::path>{workspace / ".ava" / "skills", workspace / ".agents" / "skills", workspace / ".claude" / "skills"};

  auto const trusted = ava::app::runtime::make_extension_resource_policy(paths, workspace, true);
  expect(trusted.include_project_resources && trusted.plugin_discovery.global_plugins_dir == paths.ava_config_dir / "plugins" &&
             trusted.plugin_discovery.project_plugins_dir == workspace / ".ava" / "plugins" &&
             trusted.plugin_enablement_file == paths.ava_state_dir / "plugin-enablement.json" && trusted.mcp_config.workspace_dir == workspace &&
             trusted.mcp_config.global_config_file == paths.ava_config_dir / "mcp.json" &&
             trusted.mcp_config.project_config_file == workspace / ".ava" / "mcp.json" && trusted.global_lsp_config_file == paths.ava_config_dir / "lsp.json" &&
             trusted.project_lsp_config_file == workspace / ".ava" / "lsp.json" && trusted.global_skill_dirs == expected_global_skill_dirs &&
             trusted.project_skill_dirs == expected_project_skill_dirs,
         "trusted extension resource policy derives every path from synthetic XDG and workspace inputs");

  auto const untrusted = ava::app::runtime::make_extension_resource_policy(paths, workspace, false);
  expect(!untrusted.include_project_resources && untrusted.plugin_discovery.global_plugins_dir == trusted.plugin_discovery.global_plugins_dir &&
             untrusted.plugin_discovery.project_plugins_dir.empty() && untrusted.plugin_enablement_file == trusted.plugin_enablement_file &&
             untrusted.mcp_config.workspace_dir == trusted.mcp_config.workspace_dir &&
             untrusted.mcp_config.global_config_file == trusted.mcp_config.global_config_file && untrusted.mcp_config.project_config_file.empty() &&
             untrusted.global_lsp_config_file == trusted.global_lsp_config_file && untrusted.project_lsp_config_file.empty() &&
             untrusted.global_skill_dirs == expected_global_skill_dirs && untrusted.project_skill_dirs.empty(),
         "untrusted extension resource policy preserves synthetic global paths and workspace while omitting every project path");
}

void test_app_runtime_open_session_and_context_prompt()
{
  auto root = create_empty_root("app-runtime-open");

  auto workspace = root / "workspace";
  auto const current = workspace / "src";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(current);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(workspace / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << "workspace runtime instructions\n";
  }
  {
    std::ofstream file(current / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << "nested runtime instructions\n";
  }
  {
    std::ofstream file(paths.global_agents_file, std::ios::binary | std::ios::trunc);
    file << "global runtime instructions\n";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = current;
  open_context.mode = ava::agent::Mode::Plan;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "runtime session opens with selected model, prompt, and context" : unlocked_session_result.error().format());
  if (!unlocked_session_result)
    return;

  std::string session_id;
  {
    ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
    CRITICAL_AREA_BEGIN_R(session);
    expect(session_r->created && session_r->mode() == ava::agent::Mode::Plan && session_r->model().model_id == "gpt-5.5",
           "runtime session records created state, mode, and model");
    CRITICAL_AREA_END_R(session);
    auto const resource_policy = ava::app::runtime::make_extension_resource_policy_1(unlocked_session);
    expect(!resource_policy.include_project_resources && resource_policy.plugin_discovery.global_plugins_dir == paths.ava_config_dir / "plugins" &&
               resource_policy.plugin_discovery.project_plugins_dir.empty() && resource_policy.mcp_config.workspace_dir == workspace,
           "runtime session extension resource policy overload derives paths and the fail-closed trust decision");
    CRITICAL_AREA_CONTINUE_R(session);
    expect(session_r->context_sources().size() == 3, "runtime session records workspace and global context metadata");
    expect(!session_r->base_prompt().from_override && !session_r->base_prompt().source_path && session_r->base_prompt().byte_count > 0 &&
               session_r->base_prompt().content_fingerprint != 0,
           "runtime session records selected base prompt metadata without storing prompt text twice");
    expect(session_r->system_prompt().find("Plan before changing files") != std::string::npos &&
               session_r->system_prompt().find("workspace runtime instructions") != std::string::npos &&
               session_r->system_prompt().find("nested runtime instructions") != std::string::npos &&
               session_r->system_prompt().find("global runtime instructions") != std::string::npos,
           "runtime session system prompt combines selected prompt and formatted context");
    auto entries = session_r->store.load();
    expect(entries && entries->size() == 1 && (*entries)[0].type == ava::session::EntryType::SessionStart &&
               (*entries)[0].data_json.find("\"context_sources\":3") != std::string::npos,
           "runtime session appends session_start on creation");

    session_id = session_r->store.session_id();
  }
  ava::app::runtime::OpenContext reopen_context;
  reopen_context.workspace_dir = workspace;
  reopen_context.current_dir = current;
  reopen_context.mode = ava::agent::Mode::Plan;
  reopen_context.paths = paths;
  unlocked_session_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release runtime before reopen"));
  auto unlocked_reopened_result = ava::app::runtime::Session::open(reopen_context, {.sessionless = false,
                                                                   .requested_session_id = session_id.substr(0, 12),
                                                                   .fork_session_id = std::nullopt,
                                                                   .initial_session_name = std::nullopt,
                                                                   .continue_last_session = false,
                                                                   .initial_reasoning_level = std::nullopt,
                                                                   .expected_original_cwd = std::nullopt});
  expect(unlocked_reopened_result.has_value(), unlocked_reopened_result ? "runtime reopens the existing session by id prefix" : unlocked_reopened_result.error().format());
  if (unlocked_reopened_result)
  {
    SCOPED_CRITICAL_AREA_R(reopened_r, *unlocked_reopened_result);
    expect(!reopened_r->created && reopened_r->store.session_id() == session_id,
        "runtime session resolves requested session id prefixes without creating a new session");
    auto reopened_entries = reopened_r->store.load();
    expect(reopened_entries && reopened_entries->size() == 1,
        "runtime reopened session does not append another session_start");
  }
}

void test_app_runtime_preserves_legacy_subagent_job_tree()
{
  auto const root = create_empty_root("app-runtime-legacy-subagent-jobs");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const legacy = paths.ava_state_dir / "subagent-jobs";
  auto const journal = legacy / "parent.jsonl";
  auto const sentinel = legacy / "sentinel.bin";
  std::filesystem::create_directories(legacy);
  {
    std::ofstream out(journal, std::ios::binary | std::ios::trunc);
    out << "legacy journal bytes\n";
  }
  {
    std::ofstream out(sentinel, std::ios::binary | std::ios::trunc);
    std::string const bytes("sentinel\0bytes", 14);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  ::chmod(legacy.c_str(), 0711);
  ::chmod(journal.c_str(), 0640);
  ::chmod(sentinel.c_str(), 0604);

  struct stat before_directory{};
  struct stat before_journal{};
  struct stat before_sentinel{};
  bool const captured =
      ::lstat(legacy.c_str(), &before_directory) == 0 && ::lstat(journal.c_str(), &before_journal) == 0 && ::lstat(sentinel.c_str(), &before_sentinel) == 0;
  std::vector<std::string> before_names;
  for (auto const& entry : std::filesystem::directory_iterator(legacy)) before_names.push_back(entry.path().filename().string());
  std::ranges::sort(before_names);
  expect(captured && before_names == std::vector<std::string>({"parent.jsonl", "sentinel.bin"}),
         "runtime legacy-tree fixture captures exact names and metadata before construction");

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "runtime legacy subagent job tree test opens a real runtime session" : unlocked_session_result.error().format());
  if (unlocked_session_result)
  {
    ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
    CRITICAL_AREA_BEGIN_W(session);
    expect(session_w->subagent_coordinator() && session_w->subagent_delivery_manager(),
           "real runtime session constructs its application coordinator and delivery manager against the seeded XDG state root");
    auto coordinator = session_w->subagent_coordinator();
    auto manager = session_w->subagent_delivery_manager();
    std::string session_id = session_w->store.session_id();
    CRITICAL_AREA_END_W(session);

    auto started =
        coordinator->start(session_id, ava::agent::SubagentJobMode::Foreground, {.child_session_id = "child_runtime_legacy"}, [](auto const&) {
          return ava::agent::BackgroundJobCompletion{
              .state = ava::agent::BackgroundJobState::Completed, .final_text = "runtime result", .stop_reason = "completed"};
        });
    auto completed = started ? coordinator->wait(session_id, started->job.identity.job_id, std::chrono::seconds(2))
                             : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(started.error()));
    expect(completed && completed->job.delivery == ava::agent::SubagentDeliveryState::Direct,
           "real runtime coordinator executes a foreground process-local job without legacy persistence");
    manager->shutdown();
    coordinator->shutdown();
  }
  unlocked_session_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release runtime legacy fixture"));

  struct stat after_directory{};
  struct stat after_journal{};
  struct stat after_sentinel{};
  auto exact_metadata = [](struct stat const& before, struct stat const& after) {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_mode == after.st_mode && before.st_size == after.st_size &&
           before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
  };
  bool const metadata_unchanged = ::lstat(legacy.c_str(), &after_directory) == 0 && ::lstat(journal.c_str(), &after_journal) == 0 &&
                                  ::lstat(sentinel.c_str(), &after_sentinel) == 0 && exact_metadata(before_directory, after_directory) &&
                                  exact_metadata(before_journal, after_journal) && exact_metadata(before_sentinel, after_sentinel);
  std::vector<std::string> after_names;
  for (auto const& entry : std::filesystem::directory_iterator(legacy)) after_names.push_back(entry.path().filename().string());
  std::ranges::sort(after_names);
  std::ifstream journal_in(journal, std::ios::binary);
  std::ifstream sentinel_in(sentinel, std::ios::binary);
  std::string const journal_bytes((std::istreambuf_iterator<char>(journal_in)), {});
  std::string const sentinel_bytes((std::istreambuf_iterator<char>(sentinel_in)), {});
  expect(
      metadata_unchanged && after_names == before_names && journal_bytes == "legacy journal bytes\n" && sentinel_bytes == std::string("sentinel\0bytes", 14),
      "real runtime construction, coordinator use, delivery-manager shutdown, and session shutdown leave the legacy tree byte-for-byte and metadata unchanged");
}

void test_app_active_context_status_format_semantics()
{
  using ava::app::interactive_internal::format_active_context_status_value;

  expect(format_active_context_status_value(0, 100'000) == "0 (0.0%)" && format_active_context_status_value(0, std::nullopt) == "~0" &&
             format_active_context_status_value(0, 0) == "~0" && format_active_context_status_value(0, -1) == "~0",
         "active context status formats zero tokens with known percent and unknown-window tilde count");
  expect(format_active_context_status_value(1, 100'000) == "1 (<0.1%)" && format_active_context_status_value(99, 100'000) == "99 (<0.1%)" &&
             format_active_context_status_value(100, 100'000) == "100 (0.1%)",
         "active context status preserves the sub-tenth percent boundary");
  expect(format_active_context_status_value(999, 10'000) == "999 (10.0%)" && format_active_context_status_value(1'000, 10'000) == "1k (10.0%)" &&
             format_active_context_status_value(1'200, 10'000) == "1.2k (12.0%)" && format_active_context_status_value(150'300, 272'000) == "150.3k (55.3%)" &&
             format_active_context_status_value(1'000'000, 2'000'000) == "1m (50.0%)" &&
             format_active_context_status_value(1'200'000, 2'000'000) == "1.2m (60.0%)",
         "active context status keeps raw sub-1k counts and one-decimal k/m compact counts");
  expect(format_active_context_status_value(150'300, std::nullopt) == "~150.3k" && format_active_context_status_value(12'000, std::nullopt) == "~12k",
         "active context status preserves the unknown-window tilde count fallback exactly");
  expect(format_active_context_status_value(200'000, 100'000) == "200k (200.0%)" && format_active_context_status_value(150'300, 100'000) == "150.3k (150.3%)",
         "active context status reports over-window percentages without clamping");
}

void test_app_active_context_status_tracks_compaction_projection()
{
  auto const root = create_empty_root("app-active-context-status");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "active context status fixture opens a runtime session" : unlocked_session_result.error().format());
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 272'000;
  auto expected_status = [&](std::size_t active_tokens) {
    auto const system_prompt_tokens = ava::session::estimate_tokens(session_w->system_prompt());
    auto const total = active_tokens + system_prompt_tokens;
    auto const display =
        total > static_cast<std::size_t>(std::numeric_limits<long long>::max()) ? std::numeric_limits<long long>::max() : static_cast<long long>(total);
    return ava::app::interactive_internal::format_active_context_status_value(display, session_w->model().context_window_tokens);
  };

  CRITICAL_AREA_END_W(session);
  auto const initial_status = ava::app::interactive_internal::active_context_status_for_session(unlocked_session);
  CRITICAL_AREA_CONTINUE_W(session);
  auto authority = session_w->read_authority_1();
  auto entries = authority ? authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(authority.error()));
  auto const initial_tokens =
      entries ? ava::session::estimate_active_context_tokens(*entries) : ava::core::Result<std::size_t>(std::unexpected(entries.error()));
  expect(initial_status && initial_tokens && *initial_status == expected_status(*initial_tokens) && initial_status->find('(') != std::string::npos &&
             initial_status->find('%') != std::string::npos && !initial_status->starts_with('~'),
         "active context status uses compact count plus percent when the model window is known");

  session_w->model_selection().model.context_window_tokens = std::nullopt;
  CRITICAL_AREA_END_W(session);
  auto const unknown_status = ava::app::interactive_internal::active_context_status_for_session(unlocked_session);
  CRITICAL_AREA_CONTINUE_W(session);
  auto const unknown_expected = initial_tokens
                                    ? std::optional<std::string>{ava::app::interactive_internal::format_active_context_status_value(
                                          static_cast<long long>(*initial_tokens + ava::session::estimate_tokens(session_w->system_prompt())), std::nullopt)}
                                    : std::nullopt;
  expect(unknown_status && unknown_expected && *unknown_status == *unknown_expected && unknown_status->starts_with('~') &&
             unknown_status->find('%') == std::string::npos,
         "active context status falls back to tilde compact count when the model window is unknown");
  session_w->model_selection().model.context_window_tokens = 272'000;

  auto const appended_user = session_w->append_target()->append(ava::session::SessionEntry{.id = "active-context-user",
                                                                                         .parent_id = "",
                                                                                         .type = ava::session::EntryType::UserMessage,
                                                                                         .timestamp = "2026-01-01T00:00:00Z",
                                                                                         .data_json = "{\"text\":\"" + std::string(50'000, 'x') + "\"}"});
  CRITICAL_AREA_END_W(session);
  auto const grown_status = appended_user ? ava::app::interactive_internal::active_context_status_for_session(unlocked_session) : std::nullopt;
  CRITICAL_AREA_CONTINUE_W(session);
  auto grown_authority = session_w->read_authority_1();
  auto grown_entries =
      grown_authority ? grown_authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(grown_authority.error()));
  auto const grown_tokens =
      grown_entries ? ava::session::estimate_active_context_tokens(*grown_entries) : ava::core::Result<std::size_t>(std::unexpected(grown_entries.error()));

  ava::session::ManualCompactionRequest compaction_request;
  compaction_request.summary = "condensed history";
  compaction_request.config = ava::session::default_compaction_config();
  compaction_request.estimated_tokens = 12'500;
  auto compaction = ava::session::make_manual_compaction_entry(std::move(compaction_request));
  auto const appended_compaction =
      compaction ? session_w->append_target()->append(std::move(*compaction)) : ava::core::VoidResult(std::unexpected(compaction.error()));
  auto const appended_recent = appended_compaction ? session_w->append_target()->append(ava::session::SessionEntry{.id = "active-context-recent",
                                                                                                                 .parent_id = "",
                                                                                                                 .type = ava::session::EntryType::UserMessage,
                                                                                                                 .timestamp = "2026-01-01T00:00:01Z",
                                                                                                                 .data_json = "{\"text\":\"recent\"}"})
                                                   : ava::core::VoidResult(std::unexpected(appended_compaction.error()));
  CRITICAL_AREA_END_W(session);
  auto const compacted_status = appended_recent ? ava::app::interactive_internal::active_context_status_for_session(unlocked_session) : std::nullopt;
  CRITICAL_AREA_CONTINUE_W(session);
  auto compacted_authority = session_w->read_authority_1();
  auto compacted_entries = compacted_authority ? compacted_authority->load()
                                               : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(compacted_authority.error()));
  auto const active_tokens = compacted_entries ? ava::session::estimate_active_context_tokens(*compacted_entries)
                                               : ava::core::Result<std::size_t>(std::unexpected(compacted_entries.error()));
  auto const complete_tokens = compacted_entries ? ava::session::estimate_session_tokens(*compacted_entries)
                                                 : ava::core::Result<std::size_t>(std::unexpected(compacted_entries.error()));
  CRITICAL_AREA_END_W(session);
  auto const cumulative_token_status = ava::app::interactive_internal::token_status_for_session(unlocked_session);
  CRITICAL_AREA_CONTINUE_W(session);
  expect(initial_status && appended_user && grown_status && grown_tokens && compaction && appended_compaction && appended_recent && compacted_status &&
             active_tokens && complete_tokens && *grown_status != *initial_status && *grown_status == expected_status(*grown_tokens) &&
             *compacted_status != *grown_status && *compacted_status == expected_status(*active_tokens) && *active_tokens < *complete_tokens &&
             grown_status->find('(') != std::string::npos && compacted_status->find('(') != std::string::npos &&
             (!cumulative_token_status || *cumulative_token_status != *grown_status),
         "active context status grows with committed history, follows the compaction-active projection, and stays separate from cumulative token status");
}

void test_app_runtime_no_session_mode()
{
  auto const root = create_empty_root("app-runtime-no-session");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;

  auto unlocked_session_result = ava::app::runtime::Session::open(open_context, {.sessionless = true,
                                                               .requested_session_id = std::nullopt,
                                                               .fork_session_id = std::nullopt,
                                                               .initial_session_name = std::nullopt,
                                                               .continue_last_session = false,
                                                               .initial_reasoning_level = std::nullopt,
                                                               .expected_original_cwd = std::nullopt});
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "runtime no-session mode opens an ephemeral sessionless session" : unlocked_session_result.error().format());
  if (!unlocked_session_result)
    return;

  SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);

  expect(session_w->sessionless() && session_w->store.is_ephemeral(), "runtime opens no-session mode with an ephemeral store");

  auto entries = session_w->store.load();
  expect(entries && entries->size() == 1 && (*entries)[0].type == ava::session::EntryType::SessionStart,
         "runtime no-session mode records session_start in memory");
  expect(!std::filesystem::exists(session_w->store.session_path()), "runtime no-session mode does not create a resumable JSONL file");

  auto listed = ava::session::SessionStore::list_sessions(workspace, paths.sessions_dir);
  expect(listed && listed->empty(), "runtime no-session mode does not appear in persisted session listings");

  auto requested_result = ava::app::runtime::Session::open(open_context, {.sessionless = true,
                                                                        .requested_session_id = session_w->store.session_id(),
                                                                        .fork_session_id = std::nullopt,
                                                                        .initial_session_name = std::nullopt,
                                                                        .continue_last_session = false,
                                                                        .initial_reasoning_level = std::nullopt,
                                                                        .expected_original_cwd = std::nullopt});
  expect(!requested_result && requested_result.error().message().find("no-session") != std::string::npos,
         "runtime rejects no-session with requested session resume");

  auto continue_result = ava::app::runtime::Session::open(open_context, {.sessionless = true,
                                                                       .requested_session_id = std::nullopt,
                                                                       .fork_session_id = std::nullopt,
                                                                       .initial_session_name = std::nullopt,
                                                                       .continue_last_session = true,
                                                                       .initial_reasoning_level = std::nullopt,
                                                                       .expected_original_cwd = std::nullopt});
  expect(!continue_result && continue_result.error().message().find("no-session") != std::string::npos, "runtime rejects no-session with continue");
}

// Verify that replacement contexts inherit active runtime state while retaining
// frontend policy from the base context. Location-explicit creation accepts no lifecycle request.
void test_app_runtime_replacement_open_context()
{
  auto const root = create_empty_root("app-runtime-replacement-open-context");
  auto const workspace = root / "workspace";
  auto const current_dir = workspace / "current";
  auto const writable_dir = root / "additional-writable";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(current_dir);
  std::filesystem::create_directories(writable_dir);

  auto diagnostics = std::make_shared<ava::diagnostics::RuntimeDiagnostics>(paths);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = current_dir;
  open_context.mode = ava::agent::Mode::Plan;
  open_context.tool_visibility.mode = ava::agent::ToolVisibilityMode::NoBuiltinTools;
  open_context.tool_visibility.included_tools = {"sentinel-tool"};
  open_context.paths = paths;
  open_context.offline = true;
  open_context.additional_writable_dirs = {writable_dir};
  open_context.prompt_overrides.system_prompt = "replacement sentinel prompt";
  open_context.prompt_overrides.append_system_prompts = {"replacement sentinel appendix"};
  open_context.session_read_limits = ava::session::SessionReadLimits{.max_file_bytes = 123456, .max_line_bytes = 12345, .max_entries = 1234};
  open_context.diagnostics = diagnostics;

  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "replacement context test opens a non-default runtime session" : unlocked_session_result.error().format());
  if (!unlocked_session_result)
    return;

  {
    SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);

    ava::app::runtime::OpenContext base_context;
    base_context.workspace_dir = root / "wrong-workspace";
    base_context.current_dir = root / "wrong-current";
    base_context.pin_model_override = true;
    base_context.exact_session_id = true;

    auto replacement = session_w->replacement_open_context(base_context);
    bool const inherited_context =
        replacement.workspace_dir == session_w->workspace_dir() && replacement.current_dir == session_w->current_dir() && replacement.mode == session_w->mode() &&
        replacement.tool_visibility.mode == session_w->tool_visibility().mode &&
        replacement.tool_visibility.included_tools == session_w->tool_visibility().included_tools &&
        replacement.paths.sessions_dir == session_w->paths().sessions_dir && replacement.offline == session_w->is_offline() &&
        replacement.additional_writable_dirs == session_w->additional_writable_dirs() && replacement.anchor_set == session_w->anchor_set() &&
        replacement.prompt_overrides.system_prompt == session_w->prompt_overrides().system_prompt &&
        replacement.prompt_overrides.append_system_prompts == session_w->prompt_overrides().append_system_prompts && replacement.session_read_limits &&
        replacement.session_read_limits->max_file_bytes == session_w->session_read_limits().max_file_bytes &&
        replacement.session_read_limits->max_line_bytes == session_w->session_read_limits().max_line_bytes &&
        replacement.session_read_limits->max_entries == session_w->session_read_limits().max_entries &&
        replacement.subagent_coordinator == session_w->subagent_coordinator() && replacement.subagent_delivery_manager == session_w->subagent_delivery_manager() &&
        replacement.session_title_coordinator == session_w->session_title_coordinator() && replacement.diagnostics == session_w->diagnostics();
    expect(inherited_context, "replacement context inherits every active runtime-context and application-service field");
    expect(replacement.pin_model_override && replacement.exact_session_id,
           "replacement context retains frontend policy that is not represented by Session state");
  }

  ava::app::runtime::OpenContext at_context;
  at_context.paths = paths;
  auto unlocked_created_at_result = ava::app::runtime::Session::create_at(at_context, workspace, current_dir);
  expect(unlocked_created_at_result.has_value(), unlocked_created_at_result ? "runtime replacement context creates a session at an explicit location" : unlocked_created_at_result.error().format());
  if (!unlocked_created_at_result)
    return;

  SCOPED_CRITICAL_AREA_R(created_at_r, *unlocked_created_at_result);
  auto created_metadata = ava::session::load_session_metadata(created_at_r->store);
  expect(created_metadata && created_metadata->name.empty() && !created_metadata->has_manual_name,
         "location-explicit session creation has no startup lifecycle state");
}

void test_app_runtime_process_scope_lifecycle()
{
  auto const root = create_empty_root("app-runtime-process-scope");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto application_scope = ava::process::ProcessScopeV1::application(supervisor);
  expect(application_scope.has_value(),
         application_scope ? "runtime process-scope fixture creates one application authority" : application_scope.error().format());
  if (!application_scope)
    return;

  auto same_owner = [](ava::process::ProcessScopeV1 const& left, ava::process::ProcessScopeV1 const& right) {
    return left.owner_prefix().matches_prefix(right.owner_prefix()) && right.owner_prefix().matches_prefix(left.owner_prefix());
  };

  ava::app::runtime::OpenContext scoped_context;
  scoped_context.workspace_dir = workspace;
  scoped_context.current_dir = workspace;
  scoped_context.paths = paths;
  scoped_context.application_process_scope = *application_scope;
  auto first_result = ava::app::runtime::Session::open(scoped_context);
  expect(first_result.has_value(), first_result ? "runtime session derives one process scope" : first_result.error().format());
  if (!first_result)
    return;

  std::optional<ava::process::ProcessScopeV1> first_session_scope;
  ava::app::runtime::OpenContext replacement_context;
  {
    SCOPED_CRITICAL_AREA_R(first_r, *first_result);
    first_session_scope = first_r->session_process_scope();
    expect(first_session_scope && first_session_scope->owner_prefix().depth() == 2, "runtime session stores one generated session-level process scope");

    auto duplicate = first_r->lease().duplicate();
    auto authority = first_r->read_authority_1();
    expect(duplicate && authority, "runtime process-scope fixture obtains detached session authority");
    if (duplicate && authority)
    {
      auto detached = first_r->create_detached_state(std::move(*duplicate), std::move(*authority), first_r->subagent_delivery_manager());
      expect(detached.resources_.session_process_scope && first_session_scope && same_owner(*detached.resources_.session_process_scope, *first_session_scope) &&
                 &detached.resources_.session_process_scope->supervisor() == supervisor.get(),
             "detached state for the same runtime session shares its existing process scope");
    }

    replacement_context = first_r->replacement_open_context({});
  }

  expect(replacement_context.application_process_scope && replacement_context.application_process_scope->owner_prefix().depth() == 1 &&
             same_owner(*replacement_context.application_process_scope, *application_scope),
         "replacement context recovers the preserved application process root");
  auto replacement_result = ava::app::runtime::Session::open(replacement_context);
  expect(replacement_result.has_value(),
         replacement_result ? "replacement runtime session derives a fresh process scope" : replacement_result.error().format());
  if (!replacement_result || !first_session_scope)
    return;

  std::optional<ava::process::ProcessScopeV1> replacement_session_scope;
  {
    SCOPED_CRITICAL_AREA_R(replacement_r, *replacement_result);
    replacement_session_scope = replacement_r->session_process_scope();
    bool const distinct_session = replacement_session_scope && !same_owner(*replacement_session_scope, *first_session_scope);
    bool const shared_application = replacement_session_scope && same_owner(replacement_session_scope->application_scope(), *application_scope) &&
                                    &replacement_session_scope->supervisor() == supervisor.get();
    expect(distinct_session && shared_application,
           "replacement session receives a different generated session owner under the same application supervisor and root");
  }

  auto installed = ava::app::runtime::Session::replace_with(*first_result, *replacement_result);
  expect(installed.has_value(), installed ? "runtime replacement installs the fresh session process scope" : installed.error().format());
  if (installed)
  {
    SCOPED_CRITICAL_AREA_R(installed_r, *first_result);
    expect(installed_r->session_process_scope() && replacement_session_scope && same_owner(*installed_r->session_process_scope(), *replacement_session_scope),
           "session resource move and swap semantics retain the replacement session scope");
  }

  ava::app::runtime::OpenContext legacy_context;
  legacy_context.workspace_dir = workspace;
  legacy_context.current_dir = workspace;
  legacy_context.paths = paths;
  auto legacy_result = ava::app::runtime::Session::open(legacy_context);
  expect(legacy_result.has_value(),
         legacy_result ? "legacy non-spawning adapter remains constructible without process authority" : legacy_result.error().format());
  if (legacy_result)
  {
    SCOPED_CRITICAL_AREA_R(legacy_r, *legacy_result);
    auto legacy_replacement = legacy_r->replacement_open_context({});
    expect(!legacy_r->session_process_scope() && !legacy_replacement.application_process_scope,
           "missing optional application scope neither invents session authority nor appears in replacement construction");
  }

  auto const snapshot = supervisor->snapshot();
  expect(!snapshot.monitor_started && snapshot.live_records == 0 && snapshot.records.empty(),
         "runtime session process-scope propagation remains process and monitor inert");
}

void test_app_runtime_session_startup_options()
{
  auto const root = create_empty_root("app-runtime-session-startup-options");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext named_options;
  named_options.workspace_dir = workspace;
  named_options.current_dir = workspace;
  named_options.mode = ava::agent::Mode::Build;
  named_options.paths = paths;

  auto unlocked_named_result = ava::app::runtime::Session::open(named_options, {.sessionless = false,
                                                               .requested_session_id = std::nullopt,
                                                               .fork_session_id = std::nullopt,
                                                               .initial_session_name = "named startup",
                                                               .continue_last_session = false,
                                                               .initial_reasoning_level = std::nullopt,
                                                               .expected_original_cwd = std::nullopt});
  expect(unlocked_named_result.has_value(), unlocked_named_result ? "runtime startup --name opens a named persistent session" : unlocked_named_result.error().format());
  if (!unlocked_named_result)
    return;

  std::string named_session_id;
  ava::core::Result<std::vector<ava::session::SessionEntry>> named_entries;
  {
    SCOPED_CRITICAL_AREA_R(named_r, *unlocked_named_result);

    expect(unlocked_named_result.has_value() && named_r->created, "runtime opens a named startup session");

    named_session_id = named_r->store.session_id();
    auto named_metadata = ava::session::load_session_metadata(named_r->store);
    expect(named_metadata && named_metadata->name == "named startup" && named_metadata->actor == "cli", "runtime startup --name records session metadata");
    named_entries = named_r->store.load();
    expect(named_entries && named_entries->size() == 2 && (*named_entries)[0].type == ava::session::EntryType::SessionStart &&
               (*named_entries)[1].type == ava::session::EntryType::SessionMetadata,
           "runtime startup --name appends metadata after session_start");
  }

  auto custom_paths = paths;
  custom_paths.sessions_dir = root / "custom-sessions";
  ava::app::runtime::OpenContext custom_options;
  custom_options.workspace_dir = workspace;
  custom_options.current_dir = workspace;
  custom_options.paths = custom_paths;
  auto unlocked_custom_result = ava::app::runtime::Session::open(custom_options);
  expect(unlocked_custom_result.has_value(), unlocked_custom_result ? "runtime opens a session under a custom session directory" : unlocked_custom_result.error().format());
  if (unlocked_custom_result)
  {
    SCOPED_CRITICAL_AREA_R(custom_r, *unlocked_custom_result);
    expect(unlocked_custom_result.has_value() && custom_r->store.session_path().string().find(custom_paths.sessions_dir.string()) == 0,
           "runtime opens sessions under a custom session directory");
    auto default_sessions = ava::session::SessionStore::list_sessions(workspace, paths.sessions_dir);
    auto custom_sessions = ava::session::SessionStore::list_sessions(workspace, custom_paths.sessions_dir);
    expect(default_sessions && default_sessions->size() == 1 && default_sessions->front().session_id == named_session_id,
           "runtime custom session directory leaves default session listing unchanged");
    expect(custom_sessions && custom_sessions->size() == 1 && custom_sessions->front().session_id == custom_r->store.session_id(),
           "runtime custom session directory has its own session listing");
  }

  ava::app::runtime::OpenContext active_source_fork_options;
  active_source_fork_options.workspace_dir = workspace;
  active_source_fork_options.current_dir = workspace;
  active_source_fork_options.paths = paths;
  auto active_source_fork = ava::app::runtime::Session::open(active_source_fork_options, {.sessionless = false,
                                                                                        .requested_session_id = std::nullopt,
                                                                                        .fork_session_id = named_session_id,
                                                                                        .initial_session_name = std::nullopt,
                                                                                        .continue_last_session = false,
                                                                                        .initial_reasoning_level = std::nullopt,
                                                                                        .expected_original_cwd = std::nullopt});
  expect(!active_source_fork && active_source_fork.error().message().find("already owned") != std::string::npos,
         "runtime --fork reports an actionable lease conflict while another runtime owns the source");

  unlocked_named_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release source runtime before startup fork"));

  ava::app::runtime::OpenContext fork_options;
  fork_options.workspace_dir = workspace;
  fork_options.current_dir = workspace;
  fork_options.paths = paths;
  auto unlocked_forked_result = ava::app::runtime::Session::open(fork_options, {.sessionless = false,
                                                               .requested_session_id = std::nullopt,
                                                               .fork_session_id = named_session_id.substr(0, 12),
                                                               .initial_session_name = "forked startup",
                                                               .continue_last_session = false,
                                                               .initial_reasoning_level = std::nullopt,
                                                               .expected_original_cwd = std::nullopt});
  expect(unlocked_forked_result.has_value(), unlocked_forked_result ? "runtime startup --fork opens a new forked session from the source prefix" : unlocked_forked_result.error().format());
  if (unlocked_forked_result)
  {
    SCOPED_CRITICAL_AREA_R(forked_r, *unlocked_forked_result);
    expect(forked_r->created && forked_r->store.session_id() != named_session_id, "runtime --fork creates a new session from a source prefix");
    auto fork_metadata = ava::session::load_session_metadata(forked_r->store);
    expect(fork_metadata && fork_metadata->name == "forked startup" && fork_metadata->parent_session_id == named_session_id &&
               fork_metadata->source_session_id == named_session_id && fork_metadata->branch_origin == "fork" && fork_metadata->actor == "cli" &&
               !fork_metadata->branch_from_entry_id.empty(),
           "runtime --fork records branch metadata and startup name");
    auto fork_entries = forked_r->store.load();
    if (fork_entries && named_entries && named_entries->size() > 1)
    {
      auto const start_count = std::ranges::count_if(*fork_entries, [](auto const& entry) { return entry.type == ava::session::EntryType::SessionStart; });
      expect(start_count == 1 && fork_entries->size() == 3 && fork_entries->back().type == ava::session::EntryType::SessionMetadata,
             "runtime --fork copies source history and does not append an extra session_start");
      expect(fork_metadata && fork_metadata->branch_from_entry_id == (*named_entries)[1].id,
             "runtime --fork records the latest copied source entry as the branch point");
    }
  }

  auto fork_requested_result = ava::app::runtime::Session::open(fork_options, {.sessionless = false,
                                                                             .requested_session_id = named_session_id,
                                                                             .fork_session_id = named_session_id.substr(0, 12),
                                                                             .initial_session_name = std::nullopt,
                                                                             .continue_last_session = false,
                                                                             .initial_reasoning_level = std::nullopt,
                                                                             .expected_original_cwd = std::nullopt});
  expect(!fork_requested_result && fork_requested_result.error().message().find("fork") != std::string::npos,
         "runtime rejects --fork with requested session resume");

  auto fork_continue_result = ava::app::runtime::Session::open(fork_options, {.sessionless = false,
                                                                            .requested_session_id = std::nullopt,
                                                                            .fork_session_id = named_session_id.substr(0, 12),
                                                                            .initial_session_name = std::nullopt,
                                                                            .continue_last_session = true,
                                                                            .initial_reasoning_level = std::nullopt,
                                                                            .expected_original_cwd = std::nullopt});
  expect(!fork_continue_result && fork_continue_result.error().message().find("fork") != std::string::npos, "runtime rejects --fork with continue");

  auto fork_no_session_result = ava::app::runtime::Session::open(fork_options, {.sessionless = true,
                                                                              .requested_session_id = std::nullopt,
                                                                              .fork_session_id = named_session_id.substr(0, 12),
                                                                              .initial_session_name = std::nullopt,
                                                                              .continue_last_session = false,
                                                                              .initial_reasoning_level = std::nullopt,
                                                                              .expected_original_cwd = std::nullopt});
  expect(!fork_no_session_result && fork_no_session_result.error().message().find("no-session") != std::string::npos, "runtime rejects --fork with no-session");
}

void test_app_runtime_recovers_torn_tail_before_resume_and_startup_fork()
{
  for (std::string const mode : {"exact", "prefix", "continue"})
  {
    auto const root = create_empty_root("app-runtime-torn-resume-" + mode);

    auto const workspace = root / "workspace";
    auto const paths = app_test_paths(root);
    std::filesystem::create_directories(workspace);

    ava::app::runtime::OpenContext seed_options;
    seed_options.workspace_dir = workspace;
    seed_options.current_dir = workspace;
    seed_options.paths = paths;
    auto unlocked_seeded_result = ava::app::runtime::Session::open(seed_options);
    expect(unlocked_seeded_result.has_value(), unlocked_seeded_result ? ("runtime torn resume test creates source session for " + mode) : unlocked_seeded_result.error().format());
    if (!unlocked_seeded_result)
      continue;

    std::string session_id;
    std::filesystem::path session_path;
    {
      SCOPED_CRITICAL_AREA_R(seeded_r, *unlocked_seeded_result);
      session_id = seeded_r->store.session_id();
      session_path = seeded_r->store.session_path();
    }
    auto const valid_bytes = app_read_binary_file(session_path);
    {
      std::ofstream file(session_path, std::ios::binary | std::ios::app);
      file << "{\"version\":3,\"id\":\"torn";
    }
    unlocked_seeded_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release torn source before resume"));

    auto resume_context = seed_options;
    resume_context.exact_session_id = mode == "exact";
    ava::app::runtime::SessionLifecycleRequest resume_request;
    if (mode == "continue")
      resume_request.continue_last_session = true;
    else
      resume_request.requested_session_id = mode == "exact" ? session_id : session_id.substr(0, 12);
    auto unlocked_resumed_result = ava::app::runtime::Session::open(resume_context, resume_request);
    expect(unlocked_resumed_result.has_value(), unlocked_resumed_result ? "runtime torn-tail recovery resumes the quarantined source session" : unlocked_resumed_result.error().format());
    if (unlocked_resumed_result)
    {
      SCOPED_CRITICAL_AREA_R(resumed_r, *unlocked_resumed_result);
      auto loaded = unlocked_resumed_result ? resumed_r->store.load()
                                            : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(unlocked_resumed_result.error()));
      expect(unlocked_resumed_result && resumed_r->store.session_id() == session_id && loaded && loaded->size() == 1 &&
          app_read_binary_file(session_path) == valid_bytes, "runtime " + mode + " resume acquires the lease, quarantines the torn tail, and then loads validated history");
    }
  }

  auto const root = create_empty_root("app-runtime-torn-startup-fork");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext seed_options;
  seed_options.workspace_dir = workspace;
  seed_options.current_dir = workspace;
  seed_options.paths = paths;
  auto unlocked_source_result = ava::app::runtime::Session::open(seed_options);
  expect(unlocked_source_result.has_value(), unlocked_source_result ? "startup fork torn recovery test creates source session" : unlocked_source_result.error().format());
  if (!unlocked_source_result)
    return;
  std::string source_id;
  std::filesystem::path source_path;
  {
    SCOPED_CRITICAL_AREA_R(source_r, *unlocked_source_result);
    source_id = source_r->store.session_id();
    source_path = source_r->store.session_path();
  }
  auto const valid_source_bytes = app_read_binary_file(source_path);
  {
    std::ofstream file(source_path, std::ios::binary | std::ios::app);
    file << "{\"version\":3,\"id\":\"fork-torn";
  }
  unlocked_source_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release torn source before startup fork"));

  auto fork_context = seed_options;
  auto unlocked_forked_result = ava::app::runtime::Session::open(fork_context, {.sessionless = false,
                                                               .requested_session_id = std::nullopt,
                                                               .fork_session_id = source_id.substr(0, 12),
                                                               .initial_session_name = std::nullopt,
                                                               .continue_last_session = false,
                                                               .initial_reasoning_level = std::nullopt,
                                                               .expected_original_cwd = std::nullopt});
  expect(unlocked_forked_result.has_value(),
         unlocked_forked_result ? "startup --fork opens a recovered forked session" : unlocked_forked_result.error().format());
  if (unlocked_forked_result)
  {
    SCOPED_CRITICAL_AREA_W(forked_w, *unlocked_forked_result);
    auto fork_entries = forked_w->store.load();
    expect(forked_w->created && fork_entries && fork_entries->size() == 2 && app_read_binary_file(source_path) == valid_source_bytes,
           "startup --fork temporarily leases and recovers its source before holding the lease through branch creation");
  }

  auto no_recovery_artifacts = [](std::filesystem::path const& session_path) {
    auto const final_prefix = session_path.filename().string() + ".torn-tail.";
    auto const temporary_prefix = "." + session_path.filename().string() + ".torn-tail.tmp.";
    std::error_code iter_error;
    for (std::filesystem::directory_iterator iterator(session_path.parent_path(), iter_error), end; !iter_error && iterator != end;
         iterator.increment(iter_error))
    {
      auto const name = iterator->path().filename().string();
      if (name.starts_with(final_prefix) || name.starts_with(temporary_prefix))
        return false;
    }
    return true;
  };

  auto const bounded_root = create_empty_root("app-runtime-bounded-torn-resume");

  auto const bounded_workspace = bounded_root / "workspace";
  auto const bounded_paths = app_test_paths(bounded_root);
  std::filesystem::create_directories(bounded_workspace);
  ava::app::runtime::OpenContext bounded_seed_options;
  bounded_seed_options.workspace_dir = bounded_workspace;
  bounded_seed_options.current_dir = bounded_workspace;
  bounded_seed_options.paths = bounded_paths;
  auto unlocked_byte_limited_seed_result = ava::app::runtime::Session::open(bounded_seed_options);
  expect(unlocked_byte_limited_seed_result.has_value(), unlocked_byte_limited_seed_result ? "bounded runtime recovery test creates byte-limited source" : unlocked_byte_limited_seed_result.error().format());
  if (!unlocked_byte_limited_seed_result)
    return;
  std::string byte_limited_id;
  std::filesystem::path byte_limited_path;
  {
    SCOPED_CRITICAL_AREA_R(byte_limited_r, *unlocked_byte_limited_seed_result);
    byte_limited_id = byte_limited_r->store.session_id();
    byte_limited_path = byte_limited_r->store.session_path();
  }
  auto byte_limited_bytes = app_read_binary_file(byte_limited_path);
  byte_limited_bytes += "{\"version\":3,\"id\":\"bounded-byte-torn";
  {
    std::ofstream file(byte_limited_path, std::ios::binary | std::ios::trunc);
    file << byte_limited_bytes;
  }
  unlocked_byte_limited_seed_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release byte-limited runtime"));
  auto byte_limited_options = bounded_seed_options;
  byte_limited_options.exact_session_id = true;
  byte_limited_options.session_read_limits =
      ava::session::SessionReadLimits{.max_file_bytes = byte_limited_bytes.size() - 1, .max_line_bytes = byte_limited_bytes.size() - 1, .max_entries = 8};
  auto byte_limited_resume = ava::app::runtime::Session::open(byte_limited_options, {.sessionless = false,
                                                                                   .requested_session_id = byte_limited_id,
                                                                                   .fork_session_id = std::nullopt,
                                                                                   .initial_session_name = std::nullopt,
                                                                                   .continue_last_session = false,
                                                                                   .initial_reasoning_level = std::nullopt,
                                                                                   .expected_original_cwd = std::nullopt});
  expect(!byte_limited_resume && byte_limited_resume.error().message().find("byte limit") != std::string::npos &&
             app_read_binary_file(byte_limited_path) == byte_limited_bytes && no_recovery_artifacts(byte_limited_path),
         "bounded runtime/ACP-style recovery rejects an oversized source unchanged without quarantine");

  auto unlocked_entry_limited_seed_result = ava::app::runtime::Session::open(bounded_seed_options);
  expect(unlocked_entry_limited_seed_result.has_value(), unlocked_entry_limited_seed_result ? "bounded runtime recovery test creates entry-limited source" : unlocked_entry_limited_seed_result.error().format());
  if (!unlocked_entry_limited_seed_result)
    return;
  std::string entry_limited_id;
  std::filesystem::path entry_limited_path;
  {
    SCOPED_CRITICAL_AREA_W(entry_limited_w, *unlocked_entry_limited_seed_result);
    entry_limited_id = entry_limited_w->store.session_id();
    entry_limited_path = entry_limited_w->store.session_path();
    auto appended_entry = entry_limited_w->append_owned(ava::session::SessionEntry{.id = "bounded_second_entry",
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::UserMessage,
                                                                                      .timestamp = "2026-07-14T00:00:00Z",
                                                                                      .data_json = "{\"text\":\"second\"}"});
    expect(appended_entry.has_value(), "bounded runtime recovery test appends a second complete entry");
  }
  auto entry_limited_bytes = app_read_binary_file(entry_limited_path);
  entry_limited_bytes += "{\"version\":3,\"id\":\"bounded-entry-torn";
  {
    std::ofstream file(entry_limited_path, std::ios::binary | std::ios::trunc);
    file << entry_limited_bytes;
  }
  unlocked_entry_limited_seed_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release entry-limited runtime"));
  auto entry_limited_options = bounded_seed_options;
  entry_limited_options.exact_session_id = true;
  entry_limited_options.session_read_limits = ava::session::SessionReadLimits{.max_file_bytes = 4096, .max_line_bytes = 2048, .max_entries = 1};
  auto entry_limited_resume = ava::app::runtime::Session::open(entry_limited_options, {.sessionless = false,
                                                                                     .requested_session_id = entry_limited_id,
                                                                                     .fork_session_id = std::nullopt,
                                                                                     .initial_session_name = std::nullopt,
                                                                                     .continue_last_session = false,
                                                                                     .initial_reasoning_level = std::nullopt,
                                                                                     .expected_original_cwd = std::nullopt});
  expect(!entry_limited_resume && entry_limited_resume.error().message().find("entry count") != std::string::npos &&
             app_read_binary_file(entry_limited_path) == entry_limited_bytes && no_recovery_artifacts(entry_limited_path),
         "bounded runtime/ACP-style recovery rejects an over-entry source unchanged without quarantine");
}

void test_app_runtime_reconciles_committed_function_calls_on_resume()
{
  auto const root = create_empty_root("app-runtime-committed-function-reconciliation");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_seeded_result = ava::app::runtime::Session::open(options);
  expect(unlocked_seeded_result.has_value(), unlocked_seeded_result ? "committed-function reconciliation fixture opens a session" : unlocked_seeded_result.error().format());
  if (!unlocked_seeded_result)
    return;
  std::string session_id;
  {
    SCOPED_CRITICAL_AREA_R(seeded_r, *unlocked_seeded_result);
    session_id = seeded_r->store.session_id();
  }
  auto function = [](std::string id, std::string call_id, std::size_t sequence) {
    auto data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
        .assistant_turn_id = "reconcile-turn",
        .sequence = sequence,
        .kind = ava::session::AssistantOutputItemKind::FunctionCall,
        .provider_item_id = "provider-" + std::to_string(sequence),
        .provider_output_index = sequence,
        .payload = ava::session::AssistantOutputFunctionCall{.call_id = std::move(call_id), .name = "read_file", .arguments_json = R"({"path":"note.txt"})"}});
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::AssistantOutputItem,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = data.value_or("{}")};
  };
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "reconcile-turn",
                                                                                                               .item_count = 2,
                                                                                                               .provider = "openai",
                                                                                                               .model = "gpt-5.5",
                                                                                                               .finish_reason = "tool_calls",
                                                                                                               .usage_json = std::nullopt});
  auto first = function("reconcile-function-one", "reconcile-call-one", 0);
  auto second = function("reconcile-function-two", "reconcile-call-two", 1);
  {
    SCOPED_CRITICAL_AREA_W(seeded_w, *unlocked_seeded_result);
    auto committed = seeded_w->append_owned(first);
    committed = committed ? seeded_w->append_owned(second) : std::move(committed);
    committed = committed ? seeded_w->append_owned(ava::session::SessionEntry{.id = "reconcile-commit",
                                                                             .parent_id = "",
                                                                             .type = ava::session::EntryType::AssistantTurnCommit,
                                                                             .timestamp = ava::session::now_timestamp(),
                                                                             .data_json = commit_data.value_or("{}")})
                          : std::move(committed);
    auto partial_result = committed
                              ? ava::agent::append_tool_result(
                                    seeded_w->owner_append_route_1(),
                                    ava::agent::ToolDispatchResult{
                                        .call_id = "reconcile-call-one", .name = "read_file", .success = true, .result_text = R"({"ok":true,"path":"note.txt"})"},
                                    "reconcile-function-one")
                              : ava::core::VoidResult(std::unexpected(committed.error()));
    expect(committed && partial_result, "committed-function reconciliation fixture writes a committed turn with one preexisting exact result");
  }
  unlocked_seeded_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release reconciliation fixture before resume"));

  auto resume = options;
  resume.exact_session_id = true;
  auto unlocked_resumed_result = ava::app::runtime::Session::open(resume, {.sessionless = false,
                                                          .requested_session_id = session_id,
                                                          .fork_session_id = std::nullopt,
                                                          .initial_session_name = std::nullopt,
                                                          .continue_last_session = false,
                                                          .initial_reasoning_level = std::nullopt,
                                                          .expected_original_cwd = std::nullopt});
  ava::core::Result<std::vector<ava::session::SessionEntry>> entries;
  if (unlocked_resumed_result)
  {
    SCOPED_CRITICAL_AREA_R(resumed_r, *unlocked_resumed_result);
    entries = resumed_r->store.load();
  }
  else
    entries = ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(unlocked_resumed_result.error()));
  std::size_t first_results = 0;
  std::size_t second_results = 0;
  bool saw_unknown_nonretriable = false;
  if (entries)
  {
    for (auto const& entry : *entries)
    {
      if (entry.type != ava::session::EntryType::ToolResult)
        continue;
      auto const call_id = ava::core::json::string_field(entry.data_json, "call_id").value_or("");
      first_results += call_id == "reconcile-call-one";
      second_results += call_id == "reconcile-call-two";
      saw_unknown_nonretriable =
          saw_unknown_nonretriable ||
          (call_id == "reconcile-call-two" && entry.data_json.find("execution_outcome_unknown") != std::string::npos &&
           entry.data_json.find("Do not retry automatically") != std::string::npos && entry.data_json.find("reconcile-function-two") != std::string::npos);
    }
  }
  auto validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
  auto messages = entries ? ava::agent::build_provider_messages_from_entries(*entries, ava::agent::MessageBuildOptions{})
                          : ava::core::Result<std::vector<ava::provider::ChatMessage>>(std::unexpected(unlocked_resumed_result.error()));
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto request = messages ? provider.build_request(ava::provider::ProviderRequest{.provider_id = "openai",
                                                                                   .model_id = "gpt-5.5",
                                                                                   .system_prompt = "system",
                                                                                   .messages = std::move(*messages),
                                                                                   .tools_json = {},
                                                                                   .stream = true},
                                                    "token")
                           : ava::core::Result<ava::http::HttpRequest>(std::unexpected(messages.error()));
  expect(unlocked_resumed_result && entries && first_results == 1 && second_results == 1 && saw_unknown_nonretriable && validation.ok() && request,
         "resume closes only unresolved committed v4 functions, preserves exact bindings, validates replay, and builds the next provider request");

  unlocked_resumed_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release reconciled runtime before idempotence check"));
  auto unlocked_reopened_result = ava::app::runtime::Session::open(resume, {.sessionless = false,
                                                           .requested_session_id = session_id,
                                                           .fork_session_id = std::nullopt,
                                                           .initial_session_name = std::nullopt,
                                                           .continue_last_session = false,
                                                           .initial_reasoning_level = std::nullopt,
                                                           .expected_original_cwd = std::nullopt});
  ava::core::Result<std::vector<ava::session::SessionEntry>> reopened_entries;
  if (unlocked_reopened_result)
  {
    SCOPED_CRITICAL_AREA_R(reopened_r, *unlocked_reopened_result);
    reopened_entries = reopened_r->store.load();
  }
  else
    reopened_entries = ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(unlocked_reopened_result.error()));
  std::size_t second_results_after_reopen = 0;
  if (reopened_entries)
    for (auto const& entry : *reopened_entries)
      second_results_after_reopen +=
          entry.type == ava::session::EntryType::ToolResult && ava::core::json::string_field(entry.data_json, "call_id").value_or("") == "reconcile-call-two";
  expect(unlocked_reopened_result && reopened_entries && second_results_after_reopen == 1,
         "reopening an already reconciled committed function turn never writes a duplicate synthetic result");

  auto unlocked_zero_result_seed_result = ava::app::runtime::Session::open(options);
  expect(unlocked_zero_result_seed_result.has_value(), unlocked_zero_result_seed_result ? "zero-result reconciliation fixture opens a second session" : unlocked_zero_result_seed_result.error().format());
  if (!unlocked_zero_result_seed_result)
    return;
  std::string zero_result_session_id;
  auto zero_commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "reconcile-turn",
                                                                                                                      .item_count = 1,
                                                                                                                      .provider = "openai",
                                                                                                                      .model = "gpt-5.5",
                                                                                                                      .finish_reason = "tool_calls",
                                                                                                                      .usage_json = std::nullopt});
  {
    SCOPED_CRITICAL_AREA_W(zero_result_w, *unlocked_zero_result_seed_result);
    zero_result_session_id = zero_result_w->store.session_id();
    auto zero_committed = zero_result_w->append_owned(function("zero-result-function", "zero-result-call", 0));
    zero_committed = zero_committed ? zero_result_w->append_owned(ava::session::SessionEntry{.id = "zero-result-commit",
                                                                                            .parent_id = "",
                                                                                            .type = ava::session::EntryType::AssistantTurnCommit,
                                                                                            .timestamp = ava::session::now_timestamp(),
                                                                                            .data_json = zero_commit_data.value_or("{}")})
                                    : std::move(zero_committed);
    expect(zero_committed.has_value(), "zero-result reconciliation fixture writes a committed v4 function without any result");
  }
  unlocked_zero_result_seed_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release zero-result fixture before resume"));
  auto zero_resume = options;
  zero_resume.exact_session_id = true;
  auto unlocked_zero_result_reopened_result = ava::app::runtime::Session::open(zero_resume, {.sessionless = false,
                                                                            .requested_session_id = zero_result_session_id,
                                                                            .fork_session_id = std::nullopt,
                                                                            .initial_session_name = std::nullopt,
                                                                            .continue_last_session = false,
                                                                            .initial_reasoning_level = std::nullopt,
                                                                            .expected_original_cwd = std::nullopt});
  ava::core::Result<std::vector<ava::session::SessionEntry>> zero_entries;
  if (unlocked_zero_result_reopened_result)
  {
    SCOPED_CRITICAL_AREA_R(zero_reopened_r, *unlocked_zero_result_reopened_result);
    zero_entries = zero_reopened_r->store.load();
  }
  else
    zero_entries = ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(unlocked_zero_result_reopened_result.error()));
  std::size_t zero_synthetic_results = 0;
  if (zero_entries)
    for (auto const& entry : *zero_entries)
      zero_synthetic_results += entry.type == ava::session::EntryType::ToolResult &&
                                 ava::core::json::string_field(entry.data_json, "call_id").value_or("") == "zero-result-call" &&
                                 entry.data_json.find("execution_outcome_unknown") != std::string::npos;
  auto zero_validation = zero_entries ? ava::session::validate_session_replay(*zero_entries) : ava::session::SessionReplayValidation{};
  expect(unlocked_zero_result_reopened_result && zero_entries && zero_synthetic_results == 1 && zero_validation.ok(),
         "resume closes a committed v4 function with zero prior results without re-executing it");

  auto unlocked_invalid_seed_result = ava::app::runtime::Session::open(options);
  expect(unlocked_invalid_seed_result.has_value(), unlocked_invalid_seed_result ? "invalid exact-result reconciliation fixture opens a third session" : unlocked_invalid_seed_result.error().format());
  if (!unlocked_invalid_seed_result)
    return;
  std::string invalid_session_id;
  std::filesystem::path invalid_session_path;
  ava::core::VoidResult invalid_committed;
  ava::core::VoidResult invalid_result;
  {
    SCOPED_CRITICAL_AREA_W(invalid_w, *unlocked_invalid_seed_result);
    invalid_session_id = invalid_w->store.session_id();
    invalid_session_path = invalid_w->store.session_path();
    invalid_committed = invalid_w->append_owned(function("invalid-window-function", "invalid-window-call", 0));
    invalid_committed = invalid_committed ? invalid_w->append_owned(ava::session::SessionEntry{.id = "invalid-window-commit",
                                                                                               .parent_id = "",
                                                                                               .type = ava::session::EntryType::AssistantTurnCommit,
                                                                                               .timestamp = ava::session::now_timestamp(),
                                                                                               .data_json = zero_commit_data.value_or("{}")})
                                         : std::move(invalid_committed);
    invalid_committed = invalid_committed ? invalid_w->append_owned(ava::session::SessionEntry{.id = "invalid-window-user",
                                                                                               .parent_id = "",
                                                                                               .type = ava::session::EntryType::UserMessage,
                                                                                               .timestamp = ava::session::now_timestamp(),
                                                                                               .data_json = "{\"text\":\"later input\"}"})
                                         : std::move(invalid_committed);
    invalid_result = invalid_committed
                        ? ava::agent::append_tool_result(invalid_w->owner_append_route_1(),
                                                         {.call_id = "invalid-window-call", .name = "read_file", .success = true, .result_text = "late"},
                                                         "invalid-window-function")
                        : ava::core::VoidResult(std::unexpected(invalid_committed.error()));
  }
  auto const bytes_before_invalid_resume = app_read_binary_file(invalid_session_path);
  unlocked_invalid_seed_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release invalid reconciliation fixture before resume"));
  auto invalid_resume = options;
  invalid_resume.exact_session_id = true;
  auto invalid_reopened = ava::app::runtime::Session::open(invalid_resume, {.sessionless = false,
                                                                          .requested_session_id = invalid_session_id,
                                                                          .fork_session_id = std::nullopt,
                                                                          .initial_session_name = std::nullopt,
                                                                          .continue_last_session = false,
                                                                          .initial_reasoning_level = std::nullopt,
                                                                          .expected_original_cwd = std::nullopt});
  expect(invalid_committed && invalid_result && !invalid_reopened && app_read_binary_file(invalid_session_path) == bytes_before_invalid_resume,
         "runtime reconciliation rejects an out-of-window exact v4 result before appending any synthetic result");
}

void test_app_runtime_cli_prompt_overrides()
{
  auto const root = create_empty_root("app-runtime-cli-prompt-overrides");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  write_app_test_file(workspace / "AGENTS.md", "workspace context should remain after cli prompt override\n");
  write_app_test_file(paths.prompts_dir / "openai" / "gpt-5.5" / "plan.txt", "provider prompt override should be replaced\n");
  write_app_test_file(paths.ava_config_dir / "SYSTEM.md", "global system prompt should be replaced\n");
  write_app_test_file(paths.ava_config_dir / "APPEND_SYSTEM.md", "global append prompt should be replaced\n");
  write_app_test_file(workspace / ".ava" / "SYSTEM.md", "project system prompt should be replaced\n");
  write_app_test_file(workspace / ".ava" / "APPEND_SYSTEM.md", "project append prompt should be replaced\n");
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(),
         trusted ? "cli prompt override test trusts project resources" : "cli prompt override test trusts project resources: " + trusted.error().format());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Plan;
  open_context.paths = paths;
  open_context.prompt_overrides.system_prompt = "cli system prompt";
  open_context.prompt_overrides.append_system_prompts = {"cli append prompt one", "cli append prompt two"};
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "runtime session opens with cli prompt overrides" : unlocked_session_result.error().format());
  if (!unlocked_session_result)
    return;

  {
    ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
    CRITICAL_AREA_BEGIN_W(session);

    expect(session_w->base_prompt().from_override && !session_w->base_prompt().source_path &&
               session_w->base_prompt().byte_count == std::string_view("cli system prompt").size() && session_w->base_prompt().content_fingerprint != 0,
           "cli --system-prompt is recorded as base prompt metadata");
    expect(session_w->prompt_overrides().system_prompt && *session_w->prompt_overrides().system_prompt == "cli system prompt" &&
               session_w->prompt_overrides().append_system_prompts.size() == 2,
           "runtime session retains cli prompt overrides for reloads");
    expect(session_w->system_prompt().find("cli system prompt") != std::string::npos &&
               session_w->system_prompt().find("cli append prompt one") != std::string::npos &&
               session_w->system_prompt().find("cli append prompt two") != std::string::npos &&
               session_w->system_prompt().find("workspace context should remain") != std::string::npos &&
               session_w->system_prompt().find("provider prompt override should be replaced") == std::string::npos &&
               session_w->system_prompt().find("global system prompt should be replaced") == std::string::npos &&
               session_w->system_prompt().find("project system prompt should be replaced") == std::string::npos &&
               session_w->system_prompt().find("global append prompt should be replaced") == std::string::npos &&
               session_w->system_prompt().find("project append prompt should be replaced") == std::string::npos,
           "cli prompt overrides replace selected system/append prompt resources while preserving context");

    auto const system_source_count = std::ranges::count_if(
        session_w->freshness_sources(), [](auto const& source) { return source.kind == ava::app::runtime::FreshnessSourceKind::SystemPrompt; });
    auto const append_source_count = std::ranges::count_if(
        session_w->freshness_sources(), [](auto const& source) { return source.kind == ava::app::runtime::FreshnessSourceKind::AppendSystemPrompt; });
    expect(system_source_count == 1 && append_source_count == 2, "cli prompt overrides are tracked as system prompt freshness sources");

    CRITICAL_AREA_END_W(session);
    auto context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context --system-prompt"});
    expect(context && context->handled && !context->output.empty() && context->output[0].find("system_prompt_sources=3") != std::string::npos &&
               context->output[0].find("system_prompt  cli  --system-prompt  <inline>") != std::string::npos &&
               context->output[0].find("status=inline") != std::string::npos && context->output[0].find("SYSTEM.md") == std::string::npos,
           "context freshness reports cli system prompt overrides as inline sources");
    auto append_context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context --append-system-prompt"});
    expect(append_context && append_context->handled && !append_context->output.empty() &&
               append_context->output[0].find("append_system_prompt  cli  --append-system-prompt/1  <inline>") != std::string::npos &&
               append_context->output[0].find("append_system_prompt  cli  --append-system-prompt/2  <inline>") != std::string::npos &&
               append_context->output[0].find("APPEND_SYSTEM.md") == std::string::npos,
           "context freshness reports repeated cli append prompt overrides as inline sources");

    auto switched_mode = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/mode"});
    CRITICAL_AREA_CONTINUE_W(session);
    expect(switched_mode && switched_mode->handled && session_w->mode() == ava::agent::Mode::Build &&
               session_w->system_prompt().find("cli system prompt") != std::string::npos &&
               session_w->system_prompt().find("cli append prompt two") != std::string::npos &&
               session_w->system_prompt().find("Implement changes directly") == std::string::npos,
           "mode reloads preserve cli system prompt overrides");
  }
}

}  // namespace ava::tests::app_runtime_tests
