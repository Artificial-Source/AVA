#include "sys.h"
#include "tests/app_rpc_test_cases.h"
#include "tests/support/app_rpc_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/app/project_trust.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/session_run_controller.h"
#include "ava/agent/background_job_registry.h"
#include "ava/agent/subagent_job.h"
#include "ava/plugin/enablement.h"
#include "ava/session/attachments.h"
#include "ava/session/record.h"
#include "ava/session/session_metadata.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/error.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <signal.h>
#include <sys/types.h>

namespace ava::tests::app_rpc_test {

void test_app_rpc_malformed_line_recovery_and_unknown_command()
{
  auto const root = create_empty_root("app-rpc-recovery");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC recovery test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "not json\n{\"id\":\"s1\",\"type\":\"get_state\"}\n"
      "{\"id\":\"u1\",\"type\":\"unknown\"}\n");
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();
  expect(result.has_value(), "RPC loop continues after malformed and unknown commands");
  auto const malformed = rpc_response_line(jsonl, "").value_or("");
  auto const state = rpc_response_line(jsonl, "s1").value_or("");
  auto const unknown = rpc_response_line(jsonl, "u1").value_or("");
  expect_all("RPC loop writes correlated errors and recovers for the following command",
             {{"three responses", std::count(jsonl.begin(), jsonl.end(), '\n') == 3},
              {"malformed code", malformed.find("\"code\":\"invalid_request\"") != std::string_view::npos},
              {"state session", state.find("\"session_id\":\"") != std::string_view::npos},
              {"state success", state.find("\"success\":true") != std::string_view::npos},
              {"unknown code", unknown.find("\"code\":\"invalid_request\"") != std::string_view::npos}});
}

void test_app_rpc_plugin_command_has_no_tui_authority()
{
  auto const root = create_empty_root("app-rpc-plugin-ui-denial");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.rpc-ui";
  std::filesystem::create_directories(plugin_dir);
  write_app_test_file(plugin_dir / "plugin.json",
                      "{\"schema_version\":1,\"id\":\"com.example.rpc-ui\",\"name\":\"RPC UI fixture\",\"version\":\"0.1.0\","
                      "\"api_version\":\"ava.plugin.v1\",\"entrypoint\":{\"command\":\"/bin/sh\",\"args\":[\"plugin.sh\"]},"
                      "\"capabilities\":[\"commands\",\"ui.status\"],\"contributes\":{\"tools\":[],\"commands\":[{\"name\":\"run\","
                      "\"description\":\"emit UI\"}],\"prompts\":[],\"skills\":[],\"event_hooks\":[]}}\n");
  write_app_test_file(plugin_dir / "plugin.sh",
                      "printf '%s\\n' $$ > child.pid\n"
                      "IFS= read -r initialize\n"
                      "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava.plugin.v1\","
                      "\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
                      "IFS= read -r command\n"
                      "printf '%s\\n' '{\"id\":\"RPC_UI_HIDDEN_REQUEST\",\"type\":\"ui.status\","
                      "\"text\":\"RPC_UI_RAW_CANARY_2d91\"}'\n"
                      "IFS= read -r action\n");
  auto enabled =
      ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.rpc-ui", true, ava::plugin::PluginScope::Project);
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(enabled && trusted, "RPC plugin UI fixture is trusted and enabled without credentials");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto application_process_scope = ava::process::ProcessScopeV1::application(supervisor);
  expect(application_process_scope.has_value(), "RPC plugin UI fixture creates explicit test process authority");
  if (!application_process_scope)
    return;
  open_context.application_process_scope = *application_process_scope;
  ava::app::runtime::SessionLifecycleRequest lifecycle;
  lifecycle.sessionless = true;
  auto opened = ava::app::runtime::Session::open(open_context, lifecycle);
  expect(opened.has_value(), "RPC plugin UI fixture opens a sessionless runtime");
  if (!opened)
    return;
  ava::app::runtime::session_ts session(std::move(*opened));

  std::vector<ava::permissions::Operation> prompted_operations;
  ava::app::runtime::RunOptions run_options;
  run_options.permission_resolver =
      [&prompted_operations](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompted_operations.push_back(prompt.operation);
    return ava::permissions::PermissionResolution::Allow;
  };
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"rpc-ui\",\"type\":\"run_plugin_command\",\"plugin_id\":\"com.example.rpc-ui\",\"name\":\"run\","
      "\"arguments\":{}}\n"
      "{\"id\":\"after-ui\",\"type\":\"get_state\"}\n");
  std::ostringstream out;
  auto loop = ava::app::run_rpc_loop(session, open_context, provider, transport, run_options, in, out, ava::app::rpc::RpcInputWake{});
  auto const output = out.str();

  bool child_gone = false;
  auto const pid_text = app_read_binary_file(plugin_dir / "child.pid");
  if (!pid_text.empty())
  {
    auto const child_pid = static_cast<pid_t>(std::stol(pid_text));
    errno = 0;
    child_gone = ::kill(child_pid, 0) < 0 && errno == ESRCH;
  }
  ava::core::Result<std::vector<ava::session::SessionEntry>> entries = [&]() {
    SCOPED_CRITICAL_AREA_R(session_r, session);
    auto authority = session_r->read_authority_1();
    if (!authority)
      return ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(std::move(authority.error())));
    return authority->load();
  }();
  bool const session_has_ui =
      entries && std::ranges::any_of(*entries, [](auto const& entry) {
        return entry.data_json.find("RPC_UI_RAW_CANARY_2d91") != std::string::npos || entry.data_json.find("RPC_UI_HIDDEN_REQUEST") != std::string::npos;
      });
  auto const ui_permission_prompted = std::ranges::find(prompted_operations, ava::permissions::Operation::PluginUiPresent) != prompted_operations.end();
  supervisor->stop_accepting();
  auto const process_cleanup = supervisor->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(2));
  expect(loop && process_cleanup.complete && output.find("\"id\":\"rpc-ui\"") != std::string::npos &&
             output.find("plugin UI capability is unavailable") != std::string::npos &&
             output.find("\"id\":\"after-ui\"") != std::string::npos && output.find("RPC_UI_RAW_CANARY_2d91") == std::string::npos &&
             output.find("RPC_UI_HIDDEN_REQUEST") == std::string::npos && output.find("\\\"type\\\":\\\"ui.status\\\"") == std::string::npos &&
             !ui_permission_prompted && child_gone && !session_has_ui,
         "RPC run_plugin_command executes with ordinary permissions but no TUI authority, contains the UI record as a generic failure, emits no UI "
         "record/canary, reaps the process, and continues serving protocol commands");
}

void test_app_rpc_state_list_sessions_and_open_session()
{
  auto const root = create_empty_root("app-rpc-state");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_first_result = ava::app::runtime::Session::open(open_context);
  auto unlocked_second_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_first_result.has_value() && unlocked_second_result.has_value(), "RPC state test opens multiple sessions");
  if (!unlocked_first_result || !unlocked_second_result)
    return;
  ava::app::runtime::session_ts& unlocked_first(*unlocked_first_result);
  ava::app::runtime::session_ts& unlocked_second(*unlocked_second_result);

  CRITICAL_AREA_BEGIN_R(first);
  CRITICAL_AREA_BEGIN_R(second);

  auto const first_id = first_r->store.options_r()->session_id;
  auto const second_id = second_r->store.options_r()->session_id;

  CRITICAL_AREA_END_R(second);
  CRITICAL_AREA_END_R(first);

  unlocked_first_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release target runtime before RPC switch"));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"state\",\"type\":\"get_state\"}\n"
      "{\"id\":\"list\",\"type\":\"list_sessions\"}\n"
      "{\"id\":\"open\",\"type\":\"open_session\",\"session_id\":\"" +
      first_id + "\"}\n");
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_second, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});

  CRITICAL_AREA_CONTINUE_R(second);

  auto const jsonl = out.str();
  expect(result.has_value(), "RPC state/list/open loop completes successfully");
  auto const state = rpc_response_line(jsonl, "state").value_or("");
  auto const listed = rpc_response_line(jsonl, "list").value_or("");
  auto const opened = rpc_response_line(jsonl, "open").value_or("");
  expect_all("RPC state, list_sessions, and open_session return their own session metadata",
             {{"state active session", state.find(second_id) != std::string_view::npos},
              {"list includes first", listed.find(first_id) != std::string_view::npos},
              {"list includes second", listed.find(second_id) != std::string_view::npos},
              {"open requested session", opened.find(first_id) != std::string_view::npos}});
  expect(second_r->store.options_r()->session_id == first_id, "RPC open_session switches the active runtime session");
}

void test_app_rpc_job_controls_are_active_safe_and_redacted()
{
  auto const root = temp_root() / "app-rpc-job-controls";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC job fixture opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  expect(static_cast<bool>(ava::app::runtime::session_ts::rat(unlocked_session)->subagent_coordinator()), "RPC job fixture requires a subagent coordinator");
  if (!ava::app::runtime::session_ts::rat(unlocked_session)->subagent_coordinator())
    return;

  struct WorkerState
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool release = false;
    ava::agent::BackgroundJobCompletion run(ava::agent::BackgroundJobContext const& context)
    {
      std::stop_callback wake(context.stop_token, [&] { changed.notify_all(); });
      std::unique_lock lock(mutex);
      started = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release || context.stop_token.stop_requested(); });
      if (context.stop_token.stop_requested())
        return {.state = ava::agent::BackgroundJobState::Canceled,
                .final_text = {},
                .stop_reason = "canceled",
                .error = std::nullopt,
                .provider_iterations = 0,
                .tool_calls = 0,
                .tool_iterations = 0};
      return {.state = ava::agent::BackgroundJobState::Completed,
              .final_text = "bounded RPC result",
              .stop_reason = "completed",
              .error = std::nullopt,
              .provider_iterations = 0,
              .tool_calls = 0,
              .tool_iterations = 0};
    }
    bool wait_started()
    {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, std::chrono::seconds(1), [&] { return started; });
    }
    void finish()
    {
      std::lock_guard lock(mutex);
      release = true;
      changed.notify_all();
    }
  };

  auto coordinator = ava::app::runtime::session_ts::rat(unlocked_session)->subagent_coordinator();
  auto const owner = ava::app::runtime::session_ts::rat(unlocked_session)->store.options_r()->session_id;
  auto promoted_state = std::make_shared<WorkerState>();
  auto canceled_state = std::make_shared<WorkerState>();
  auto promotable = coordinator->start(owner, ava::agent::SubagentJobMode::Foreground, {.child_session_id = "rpc_promotable"},
                                       [promoted_state](auto const& context) { return promoted_state->run(context); });
  auto cancelable = coordinator->start(owner, ava::agent::SubagentJobMode::Foreground, {.child_session_id = "rpc_cancelable"},
                                       [canceled_state](auto const& context) { return canceled_state->run(context); });
  auto failed = coordinator->start(owner, ava::agent::SubagentJobMode::Foreground, {.child_session_id = "rpc_failed"}, [](auto const&) {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "credential=rpc-secret raw provider body");
    error.with_context("command", "curl --token rpc-secret");
    return ava::agent::BackgroundJobCompletion{.state = ava::agent::BackgroundJobState::Failed,
                                               .final_text = {},
                                               .stop_reason = "failed",
                                               .error = std::move(error),
                                               .provider_iterations = 0,
                                               .tool_calls = 0,
                                               .tool_iterations = 0};
  });
  expect(promotable && cancelable && failed && promoted_state->wait_started() && canceled_state->wait_started(), "RPC job fixture starts controlled jobs");
  if (!promotable || !cancelable || !failed)
    return;
  static_cast<void>(coordinator->wait(owner, failed->job.identity.job_id, std::chrono::seconds(1)));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  BlockingResponseTransport transport(sse_response(final_text_sse("parent terminal")));
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult rpc_result;
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    rpc_result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"prompt\",\"type\":\"prompt\",\"message\":\"keep active\"}\n");
  expect(transport.wait_for_request(std::chrono::seconds(2)), "RPC job fixture has an active parent prompt");
  auto const promotable_id = promotable->job.identity.job_id;
  auto const cancelable_id = cancelable->job.identity.job_id;
  auto const failed_id = failed->job.identity.job_id;
  input_buffer.push("{\"id\":\"jobs\",\"type\":\"list_jobs\"}\n");
  input_buffer.push("{\"id\":\"status\",\"type\":\"get_job\",\"job_id\":\"" + promotable_id + "\"}\n");
  input_buffer.push("{\"id\":\"wait\",\"type\":\"wait_job\",\"job_id\":\"" + promotable_id + "\",\"timeout_ms\":1}\n");
  input_buffer.push("{\"id\":\"not-ready\",\"type\":\"get_job_result\",\"job_id\":\"" + promotable_id + "\"}\n");
  input_buffer.push("{\"id\":\"promote\",\"type\":\"promote_job\",\"job_id\":\"" + promotable_id + "\"}\n");
  input_buffer.push("{\"id\":\"cancel-job\",\"type\":\"cancel_job\",\"job_id\":\"" + cancelable_id + "\"}\n");
  input_buffer.push("{\"id\":\"failed-result\",\"type\":\"get_job_result\",\"job_id\":\"" + failed_id + "\"}\n");
  bool const controls_completed = wait_for_rpc_response(output_buffer, "failed-result", std::chrono::seconds(2));
  auto active_output = output_buffer.str();
  auto const promoted = rpc_response_line(active_output, "promote").value_or("");
  auto const waited = rpc_response_line(active_output, "wait").value_or("");
  auto const not_ready = rpc_response_line(active_output, "not-ready").value_or("");
  auto const failed_result = rpc_response_line(active_output, "failed-result").value_or("");
  auto const canceled_job = rpc_response_line(active_output, "cancel-job").value_or("");
  expect_all("RPC job controls remain active-safe and redact provider/error context",
             {{"all controls processed", controls_completed},
              {"promotion", promoted.find("\"was_promoted\":true") != std::string_view::npos},
              {"wait times out", waited.find("\"timed_out\":true") != std::string_view::npos},
              {"unfinished result", not_ready.find("\"code\":\"job_not_ready\"") != std::string_view::npos},
              {"failed job state", failed_result.find("\"state\":\"failed\"") != std::string_view::npos},
              {"failed job category", failed_result.find("\"error_category\":\"provider\"") != std::string_view::npos},
              {"cancel accepted", canceled_job.find("\"success\":true") != std::string_view::npos},
              {"secret redacted", active_output.find("rpc-secret") == std::string::npos},
              {"provider body redacted", active_output.find("raw provider body") == std::string::npos},
              {"active-safe", active_output.find("\"code\":\"active_run\"") == std::string::npos}});

  promoted_state->finish();
  static_cast<void>(coordinator->wait(owner, promotable_id, std::chrono::seconds(1)));
  transport.release();
  input_buffer.close();
  rpc_thread.join();
  expect(rpc_result.has_value(), "RPC job active-safe loop shuts down cleanly");
}

void test_app_rpc_current_session_reads_reject_path_replacement()
{
  auto const root = create_empty_root("app-rpc-current-session-replacement");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "replacement-safe current-session RPC test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  CRITICAL_AREA_BEGIN_W(session);

  expect(session_w
             ->append_owned(ava::session::SessionEntry{.id = "original_rpc_history",
                                                       .parent_id = "",
                                                       .type = ava::session::EntryType::UserMessage,
                                                       .timestamp = "2026-05-02T00:00:00Z",
                                                       .data_json = "{\"text\":\"ORIGINAL_RPC_HISTORY\"}"})
             .has_value(),
         "replacement-safe current-session RPC test seeds original history");

  auto replacement_result = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "replacement_rpc_history",
                                                                                                  .parent_id = "",
                                                                                                  .type = ava::session::EntryType::UserMessage,
                                                                                                  .timestamp = "2026-05-02T00:00:01Z",
                                                                                                  .data_json = "{\"text\":\"RPC_REPLACEMENT_CANARY\"}"});
  expect(replacement_result.has_value(), "replacement-safe current-session RPC test serializes replacement history");
  if (!replacement_result)
    return;
  auto const replacement = std::move(*replacement_result);
  bool replaced = false;
  auto const session_path = session_w->store.session_path();
  session_w->store.set_after_lease_bound_read_for_test([&replacement, &replaced, session_path] {
    if (replaced)
      return;
    replaced = true;
    std::filesystem::rename(session_path, session_path.string() + ".parked");
    std::ofstream file(session_path, std::ios::binary | std::ios::trunc);
    file << replacement << '\n';
  });

  CRITICAL_AREA_END_W(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"messages\",\"type\":\"get_messages\"}\n"
      "{\"id\":\"metadata\",\"type\":\"session_metadata\"}\n"
      "{\"id\":\"stats\",\"type\":\"get_session_stats\"}\n"
      "{\"id\":\"validate\",\"type\":\"validate_session\"}\n");
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});

  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);

  auto const jsonl = out.str();
  auto pathname_entries = session_r->store.load();
  expect(result && replaced && jsonl.find("replaced") != std::string::npos && jsonl.find("RPC_REPLACEMENT_CANARY") == std::string::npos && pathname_entries &&
             pathname_entries->size() == 1 && pathname_entries->front().data_json.find("RPC_REPLACEMENT_CANARY") != std::string::npos,
         "current-session RPC messages, metadata, stats, and validation fail closed after authority binding without serializing replacement content");
}

void test_app_rpc_session_metadata_name_and_labels()
{
  auto const root = create_empty_root("app-rpc-session-metadata");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC session metadata test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"initial\",\"type\":\"session_metadata\"}\n"
      "{\"id\":\"name\",\"type\":\"set_session_name\",\"session_name\":\"Auth follow-up\"}\n"
      "{\"id\":\"labels\",\"type\":\"set_session_labels\",\"labels\":[\"auth\",\"bug\"]}\n"
      "{\"id\":\"after\",\"type\":\"session_metadata\"}\n"
      "{\"id\":\"bad\",\"type\":\"set_session_labels\",\"labels\":[\"dup\",\"dup\"]}\n");
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});

  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  auto const jsonl = out.str();
  auto metadata = ava::session::load_session_metadata(session_r->store);
  auto entries = session_r->store.load();
  auto validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};

  expect(result.has_value() && metadata && metadata->name == "Auth follow-up" && metadata->labels.size() == 2 && metadata->labels[0] == "auth" &&
             metadata->labels[1] == "bug" && metadata->actor == "rpc" && validation.ok(),
         "RPC session metadata commands persist append-only name and label entries");
  expect(jsonl.find("\"id\":\"initial\"") != std::string::npos && jsonl.find("\"name\":\"\"") != std::string::npos &&
             jsonl.find("\"id\":\"name\"") != std::string::npos && jsonl.find("\"name\":\"Auth follow-up\"") != std::string::npos &&
             jsonl.find("\"labels\":[\"auth\",\"bug\"]") != std::string::npos && jsonl.find("\"actor\":\"rpc\"") != std::string::npos &&
             jsonl.find("session labels must be unique") != std::string::npos,
         "RPC session metadata responses expose current name/labels/actor and reject invalid labels");
}

void test_app_rpc_session_tree_command_and_switch_navigation()
{
  auto const root = create_empty_root("app-rpc-session-tree");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_parent_result = ava::app::runtime::Session::open(open_context);
  auto unlocked_child_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_parent_result.has_value() && unlocked_child_result.has_value(), "RPC session_tree test opens parent and child sessions");
  if (!unlocked_parent_result || !unlocked_child_result)
    return;
  ava::app::runtime::session_ts& unlocked_parent(*unlocked_parent_result);
  ava::app::runtime::session_ts& unlocked_child(*unlocked_child_result);

  CRITICAL_AREA_BEGIN_W(parent);
  CRITICAL_AREA_BEGIN_W(child);

  auto parent_entries = parent_w->store.load();
  expect(parent_entries && !parent_entries->empty(), "RPC session_tree test loads parent start entry");
  if (!parent_entries || parent_entries->empty())
    return;

  ava::session::SessionMetadataUpdate parent_metadata;
  parent_metadata.name = "Parent";
  parent_metadata.labels = std::vector<std::string>{"root"};
  parent_metadata.branch_origin = "root";
  parent_metadata.actor = "test";
  auto const parent_id = parent_w->store.options_r()->session_id;
  auto parent_meta = parent_w->append_metadata_1(std::move(parent_metadata));

  ava::session::SessionMetadataUpdate child_metadata;
  child_metadata.name = "Child";
  child_metadata.labels = std::vector<std::string>{"branch"};
  child_metadata.archived = true;
  child_metadata.parent_session_id = parent_id;
  child_metadata.source_session_id = parent_id;
  child_metadata.branch_from_entry_id = parent_entries->front().id;
  child_metadata.branch_origin = "fork";
  child_metadata.actor = "test";
  auto const child_id = child_w->store.options_r()->session_id;
  auto child_meta = child_w->append_metadata_1(std::move(child_metadata));
  expect(parent_meta && child_meta, "RPC session_tree test persists branch metadata");

  CRITICAL_AREA_END_W(child);
  CRITICAL_AREA_END_W(parent);

  unlocked_parent_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release parent runtime before RPC switch"));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  auto const requests = std::string("{\"id\":\"tree\",\"type\":\"session_tree\"}\n") + "{\"id\":\"switch\",\"type\":\"switch_session\",\"session_id\":\"" +
                        parent_id + "\"}\n" + "{\"id\":\"tree2\",\"type\":\"session_tree\"}\n";
  std::istringstream in(requests);
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_child, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});

  SCOPED_CRITICAL_AREA_R(child_r, unlocked_child);
  auto const jsonl = out.str();
  expect(result.has_value(), "RPC session_tree loop completes successfully");
  expect(jsonl.find("\"id\":\"tree\"") != std::string::npos && jsonl.find("\"current_session_id\":\"" + child_id + "\"") != std::string::npos &&
             jsonl.find("\"path\":[\"" + parent_id + "\",\"" + child_id + "\"]") != std::string::npos &&
             jsonl.find("\"parent_session_id\":\"" + parent_id + "\"") != std::string::npos &&
             jsonl.find("\"children\":[\"" + child_id + "\"]") != std::string::npos && jsonl.find("\"labels\":[\"branch\"]") != std::string::npos &&
             jsonl.find("\"labels_updated\":\"") != std::string::npos && jsonl.find("\"archived\":true") != std::string::npos &&
             jsonl.find("\"actor\":\"test\"") != std::string::npos,
         "RPC session_tree returns current path, children, labels, archive state, actor, and provenance metadata");
  expect(jsonl.find("\"id\":\"switch\"") != std::string::npos && jsonl.find("\"current_session_id\":\"" + parent_id + "\"") != std::string::npos &&
             child_r->store.options_r()->session_id == parent_id,
         "RPC switch_session navigates the active session used by following tree calls");
}

void test_app_rpc_session_fork_and_clone_commands()
{
  auto const root = create_empty_root("app-rpc-session-branch");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC session branch test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  CRITICAL_AREA_BEGIN_R(session);

  auto const source_id = session_r->store.options_r()->session_id;
  auto source_entries = session_r->store.load();
  expect(source_entries && !source_entries->empty(), "RPC session branch test loads source start entry");
  if (!source_entries || source_entries->empty())
    return;
  auto const branch_from_entry_id = source_entries->front().id;
  auto const source_count = source_entries->size();
  auto const valid_source_bytes = app_read_binary_file(session_r->store.session_path());
  {
    std::ofstream file(session_r->store.session_path(), std::ios::binary | std::ios::app);
    file << "{\"version\":3,\"id\":\"rpc-current-torn";
  }

  CRITICAL_AREA_END_R(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  auto const requests = std::string(
                            "{\"id\":\"fork\",\"type\":\"fork_session\","
                            "\"branch_from_entry_id\":\"") +
                        branch_from_entry_id +
                        "\",\"session_name\":\"Forked\",\"labels\":[\"forked\"]}\n"
                        "{\"id\":\"fork_meta\",\"type\":\"session_metadata\"}\n"
                        "{\"id\":\"clone\",\"type\":\"clone_session\",\"session_id\":\"" +
                        source_id +
                        "\",\"session_name\":\"Cloned\"}\n"
                        "{\"id\":\"clone_meta\",\"type\":\"session_metadata\"}\n";
  std::istringstream in(requests);
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});

  CRITICAL_AREA_CONTINUE_R(session);

  auto const jsonl = out.str();
  auto source_store = ava::session::SessionStore::open(workspace, source_id, paths.sessions_dir);
  bool source_unchanged = false;
  if (source_store)
  {
    auto source_after = source_store->load();
    source_unchanged = source_after && source_after->size() == source_count && app_read_binary_file(source_store->session_path()) == valid_source_bytes;
  }
  expect(result.has_value(), "RPC fork/clone loop completes successfully");
  expect(source_unchanged, "RPC fork uses its existing lease to recover the current source and later branching does not add source entries");
  expect(jsonl.find("\"id\":\"fork\"") != std::string::npos && jsonl.find("\"id\":\"fork_meta\"") != std::string::npos &&
             jsonl.find("\"created\":true") != std::string::npos && jsonl.find("\"name\":\"Forked\"") != std::string::npos &&
             jsonl.find("\"labels\":[\"forked\"]") != std::string::npos && jsonl.find("\"parent_session_id\":\"" + source_id + "\"") != std::string::npos &&
             jsonl.find("\"branch_from_entry_id\":\"" + branch_from_entry_id + "\"") != std::string::npos &&
             jsonl.find("\"branch_origin\":\"fork\"") != std::string::npos,
         "RPC fork_session creates and switches to a fork with provenance metadata");
  expect(jsonl.find("\"id\":\"clone\"") != std::string::npos && jsonl.find("\"id\":\"clone_meta\"") != std::string::npos &&
             jsonl.find("\"name\":\"Cloned\"") != std::string::npos && jsonl.find("\"branch_origin\":\"clone\"") != std::string::npos,
         "RPC clone_session creates and switches to a clone with provenance metadata");
  auto active_destination_contender = ava::session::SessionLease::acquire(session_r->store.session_path());
  expect(!active_destination_contender && active_destination_contender.error().message().find("already owned") != std::string::npos,
         "RPC fork/clone transfers the active destination lease directly into the replacement runtime");
}

void test_app_rpc_branch_construction_failure_rolls_back_created_file()
{
  auto const root = create_empty_root("app-rpc-branch-rollback");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_source_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_source_result.has_value(), "RPC rollback test opens an active source session");
  if (!unlocked_source_result)
    return;
  ava::app::runtime::session_ts& unlocked_source(*unlocked_source_result);

  CRITICAL_AREA_BEGIN_W(source);

  auto entries = source_w->store.load();
  expect(entries && !entries->empty(), "RPC rollback test loads the source start entry");
  if (!entries || entries->empty())
    return;
  auto appended =
      source_w->append_owned(ava::session::SessionEntry{.id = "entry_rpc_rollback_attachment",
                                                        .parent_id = entries->back().id,
                                                        .type = ava::session::EntryType::UserMessage,
                                                        .timestamp = "2026-07-16T00:00:00Z",
                                                        .data_json = "{\"text\":\"attachment\",\"attachments\":[{\"id\":\"rpc_rollback_img\","
                                                                     "\"type\":\"image\",\"mime_type\":\"image/png\",\"byte_size\":5,"
                                                                     "\"sha256\":\"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\","
                                                                     "\"storage_path\":\"attachments/rollback.txt\"}]}",
                                                        .version = 2});
  auto const source_attachment = ava::session::attachment_storage_root(source_w->store) / "attachments" / "rollback.txt";
  write_app_test_file(source_attachment, "hello");
  expect(appended.has_value(), "RPC rollback test appends a copyable source attachment reference");
  if (!appended)
    return;

  auto const source_id = source_w->store.options_r()->session_id;
  auto const source_path = source_w->store.session_path();

  CRITICAL_AREA_END_W(source);

  std::filesystem::create_directories(paths.models_file);
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in("{\"id\":\"fork-rollback\",\"type\":\"fork_session\"}\n");
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_source, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});

  SCOPED_CRITICAL_AREA_R(source_r, unlocked_source);
  auto const jsonl = out.str();
  auto const marker = std::string("created_session_id: ");
  auto const marker_offset = jsonl.find(marker);
  std::optional<std::string> created_id;
  if (marker_offset != std::string::npos)
  {
    auto const value_start = marker_offset + marker.size();
    auto const value_end = jsonl.find("\\n", value_start);
    created_id = jsonl.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
  }
  bool destination_jsonl_removed = false;
  bool destination_attachment_retained = false;
  if (created_id)
  {
    auto destination =
        ava::session::SessionStore(ava::session::SessionStoreOptions{.root_dir = paths.sessions_dir, .workspace_dir = workspace, .session_id = *created_id});
    auto const destination_attachment = ava::session::attachment_storage_root(destination) / "attachments" / "rollback.txt";
    destination_jsonl_removed = !std::filesystem::exists(destination.session_path());
    destination_attachment_retained = app_read_binary_file(destination_attachment) == "hello";
  }
  auto source_contender = ava::session::SessionLease::acquire(source_path);
  expect(result && created_id && jsonl.find("\"id\":\"fork-rollback\"") != std::string::npos && jsonl.find("\"success\":false") != std::string::npos &&
             jsonl.find("rollback_attachment_disposition: preserved") != std::string::npos && destination_jsonl_removed && destination_attachment_retained &&
             source_r->store.options_r()->session_id == source_id && !source_contender && source_contender.error().message().find("already owned") != std::string::npos,
         "RPC branch runtime-construction failure preserves the primary error, removes only destination JSONL, retains copied attachments, and leaves the "
         "source active");
}

void test_app_rpc_noncurrent_branch_source_recovers_torn_tail()
{
  auto const root = create_empty_root("app-rpc-noncurrent-torn-branch");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_source_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_source_result.has_value(), "RPC noncurrent torn branch test opens source runtime");
  if (!unlocked_source_result)
    return;
  ava::app::runtime::session_ts& unlocked_source(*unlocked_source_result);

  CRITICAL_AREA_BEGIN_R(source);

  auto const source_id = source_r->store.options_r()->session_id;
  auto const source_path = source_r->store.session_path();

  CRITICAL_AREA_END_R(source);

  auto const valid_source_bytes = app_read_binary_file(source_path);
  unlocked_source_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release source runtime before RPC branch"));

  auto unlocked_current_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_current_result.has_value(), "RPC noncurrent torn branch test opens a different current runtime");
  if (!unlocked_current_result)
    return;
  ava::app::runtime::session_ts& unlocked_current(*unlocked_current_result);
  {
    std::ofstream file(source_path, std::ios::binary | std::ios::app);
    file << "{\"version\":3,\"id\":\"rpc-torn";
  }

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in("{\"id\":\"clone\",\"type\":\"clone_session\",\"session_id\":\"" + source_id + "\"}\n");
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_current, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});
  SCOPED_CRITICAL_AREA_R(current_r, unlocked_current);
  auto cloned_entries = current_r->store.load();
  auto cloned_metadata = ava::session::load_session_metadata(current_r->store);
  expect(result && out.str().find("\"id\":\"clone\"") != std::string::npos && current_r->store.options_r()->session_id != source_id && cloned_entries &&
             cloned_entries->size() == 2 && cloned_metadata && cloned_metadata->source_session_id == source_id && cloned_metadata->branch_origin == "clone" &&
             app_read_binary_file(source_path) == valid_source_bytes,
         "RPC branching temporarily leases and recovers a different source before holding it through clone creation");
}

void test_app_rpc_maintenance_reservation_blocks_generic_mutation()
{
  auto const root = create_empty_root("app-rpc-maintenance-reservation");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto opened = ava::app::runtime::Session::open(open_context);
  expect(opened.has_value(), "RPC maintenance fixture opens a runtime session");
  if (!opened)
    return;
  ava::app::runtime::session_ts session(std::move(*opened));
  std::string session_id;
  std::optional<ava::app::SessionMaintenanceReservation> reservation;
  {
    SCOPED_CRITICAL_AREA_R(session_r, session);
    session_id = session_r->store.options_r()->session_id;
    auto acquired = session_r->run_controller()->reserve_maintenance();
    expect(acquired.has_value(), acquired ? "RPC fixture acquires exclusive maintenance" : acquired.error().format());
    if (!acquired)
      return;
    reservation.emplace(std::move(*acquired));
  }

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in("{\"id\":\"new\",\"type\":\"new_session\"}\n");
  std::ostringstream out;
  auto result = ava::app::run_rpc_loop(session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});
  SCOPED_CRITICAL_AREA_R(session_r, session);
  expect(result && session_r->store.options_r()->session_id == session_id && out.str().find("\"code\":\"invalid_request\"") != std::string::npos &&
             out.str().find("exclusive session maintenance") != std::string::npos,
         "central RPC mutation gate observes maintenance without classifying it as a normal active run");
}

void test_app_rpc_summarize_branch_appends_to_source_session()
{
  auto const root = create_empty_root("app-rpc-branch-summary");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC summarize_branch test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  CRITICAL_AREA_BEGIN_R(session);

  auto const source_id = session_r->store.options_r()->session_id;
  auto source_entries = session_r->store.load();
  expect(source_entries && !source_entries->empty(), "RPC summarize_branch test loads source start entry");
  if (!source_entries || source_entries->empty())
    return;
  auto const source_count = source_entries->size();
  auto const root_entry_id = source_entries->front().id;
  auto const tip_entry_id = source_entries->back().id;

  CRITICAL_AREA_END_R(session);

  auto noncurrent_store = ava::session::SessionStore(
      ava::session::SessionStoreOptions{.root_dir = paths.sessions_dir, .workspace_dir = workspace, .session_id = "session_rpc_noncurrent_summary"});
  auto noncurrent_start = ava::session::SessionEntry{.id = "entry_noncurrent_root",
                                                     .parent_id = "",
                                                     .type = ava::session::EntryType::SessionStart,
                                                     .timestamp = "2026-08-04T00:00:00Z",
                                                     .data_json =
                                                         "{\"mode\":\"build\",\"provider\":\"openai\",\"model\":\"gpt-test\","
                                                         "\"context_sources\":0,\"context_window_tokens\":128000,\"max_output_tokens\":4096,"
                                                         "\"prompt_override\":false,\"supports_tools\":true,\"supports_streaming\":true,"
                                                         "\"supports_reasoning\":true,\"reports_usage\":true}"};
  auto noncurrent_tip = ava::session::SessionEntry{.id = "entry_noncurrent_tip",
                                                   .parent_id = "entry_noncurrent_root",
                                                   .type = ava::session::EntryType::UserMessage,
                                                   .timestamp = "2026-08-04T00:00:01Z",
                                                   .data_json = "{\"text\":\"noncurrent source\"}"};
  auto noncurrent_seeded = append_session_entry_for_test(noncurrent_store, noncurrent_start) && append_session_entry_for_test(noncurrent_store, noncurrent_tip);
  expect(noncurrent_seeded, "RPC summarize_branch test creates a noncurrent source session");
  if (!noncurrent_seeded)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  auto const summary_request = std::string("\"type\":\"summarize_branch\",") + "\"branch_root_entry_id\":\"" + root_entry_id + "\",\"branch_tip_entry_id\":\"" +
                               tip_entry_id +
                               "\",\"summary\":\"Abandoned path was not needed.\","
                               "\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}";
  auto const noncurrent_request =
      "\"type\":\"summarize_branch\",\"session_id\":\"session_rpc_noncurrent_summary\","
      "\"branch_root_entry_id\":\"entry_noncurrent_root\",\"branch_tip_entry_id\":\"entry_noncurrent_tip\","
      "\"summary\":\"Noncurrent abandoned path.\",\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}";
  auto const requests = std::string("{\"id\":\"summary\",") + summary_request + "\n{\"id\":\"summary-duplicate\"," + summary_request +
                        "\n{\"id\":\"noncurrent-summary\"," + noncurrent_request + "\n{\"id\":\"noncurrent-summary-duplicate\"," + noncurrent_request +
                        "\n{\"id\":\"stats\",\"type\":\"get_session_stats\"}\n";
  std::istringstream in(requests);
  std::ostringstream out;
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});

  CRITICAL_AREA_CONTINUE_R(session);

  auto const jsonl = out.str();
  auto source_store = ava::session::SessionStore::open(workspace, source_id, paths.sessions_dir);
  bool source_has_summary = false;
  std::string persisted_summary_id;
  std::optional<ava::session::SessionEntry> persisted_summary_entry;
  if (source_store)
  {
    auto source_after = source_store->load();
    source_has_summary = source_after && source_after->size() == source_count + 1 && source_after->back().type == ava::session::EntryType::BranchSummary &&
                         source_after->back().parent_id == tip_entry_id;
    if (source_has_summary)
    {
      persisted_summary_id = source_after->back().id;
      persisted_summary_entry = source_after->back();
    }
  }
  auto noncurrent_after = noncurrent_store.load();
  bool const noncurrent_has_one = noncurrent_after && noncurrent_after->size() == 3 &&
                                  noncurrent_after->back().type == ava::session::EntryType::BranchSummary &&
                                  noncurrent_after->back().parent_id == "entry_noncurrent_tip";
  auto response_repeats_entry = [&](std::string const& entry_id) {
    auto const first = jsonl.find("\"entry_id\":\"" + entry_id + "\"");
    return first != std::string::npos && jsonl.find("\"entry_id\":\"" + entry_id + "\"", first + 1) != std::string::npos;
  };
  bool const duplicate_results_match =
      source_has_summary && noncurrent_has_one && response_repeats_entry(persisted_summary_id) && response_repeats_entry(noncurrent_after->back().id);
  expect(result.has_value(), "RPC summarize_branch loop completes successfully");
  expect(source_has_summary && noncurrent_has_one && session_r->store.options_r()->session_id == source_id,
         "RPC summarize_branch appends once to current and noncurrent source sessions without switching sessions");
  expect(jsonl.find("\"id\":\"summary\"") != std::string::npos && jsonl.find("\"id\":\"summary-duplicate\"") != std::string::npos &&
             jsonl.find("\"id\":\"noncurrent-summary\"") != std::string::npos && jsonl.find("\"id\":\"noncurrent-summary-duplicate\"") != std::string::npos &&
             jsonl.find("\"source_session_id\":\"" + source_id + "\"") != std::string::npos && jsonl.find("\"type\":\"branch_summary\"") != std::string::npos &&
             jsonl.find("Abandoned path was not needed.") != std::string::npos && jsonl.find("\"branch_summary\":1") != std::string::npos &&
             duplicate_results_match && transport.requests().empty(),
         "RPC summarize_branch returns the existing result for an exact duplicate without a second append or provider request");

  auto const bytes_before_owner_duplicate = app_read_binary_file(session_r->store.session_path());
  auto owner_duplicate = persisted_summary_entry && session_r->run_controller()
                             ? session_r->run_controller()->append_branch_summary_if_absent(*persisted_summary_entry)
                             : ava::core::Result<ava::session::SessionConditionalAppendResult>(
                                   std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "owner duplicate fixture setup failed")));
  expect(owner_duplicate && owner_duplicate->disposition == ava::session::SessionAppendDisposition::Existing &&
             app_read_binary_file(session_r->store.session_path()) == bytes_before_owner_duplicate,
         "current owner route performs the same final duplicate recheck without appending bytes");

  auto growth = ava::session::SessionEntry{.id = "summary-latch-growth",
                                           .parent_id = persisted_summary_id,
                                           .type = ava::session::EntryType::UserMessage,
                                           .timestamp = ava::session::now_timestamp(),
                                           .data_json = "{\"text\":\"new range before append latch\"}"};
  auto latch_growth =
      session_r->run_controller()
          ? session_r->run_controller()->append(std::move(growth))
          : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "missing summary route controller")));
  auto invalid = ava::session::SessionEntry{
      .id = "summary-route-latch", .parent_id = "", .type = ava::session::EntryType::Error, .timestamp = ava::session::now_timestamp(), .data_json = ""};
  auto latched = session_r->run_controller()
                     ? session_r->run_controller()->append(std::move(invalid))
                     : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "missing summary route controller")));

  CRITICAL_AREA_END_R(session);

  std::istringstream blocked_in(std::string("{\"id\":\"blocked-summary\",\"type\":\"summarize_branch\",") + "\"branch_root_entry_id\":\"" + root_entry_id +
                                "\",\"branch_tip_entry_id\":\"summary-latch-growth\","
                                "\"summary\":\"must not bypass latch\",\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}\n");
  std::ostringstream blocked_out;
  auto blocked_loop = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, blocked_in, blocked_out,
                                             ava::app::rpc::RpcInputWake{});
  CRITICAL_AREA_CONTINUE_R(session);
  auto blocked_entries = session_r->store.load();
  CRITICAL_AREA_END_R(session);

  auto latch_message = std::string(
      "current-session RPC summaries perform lease-bound reads but append only through the owner route and cannot bypass its "
      "persistence latch");
  if (latched)
    latch_message += ": invalid append unexpectedly succeeded";
  if (!latch_growth)
    latch_message += ": growth append failed: " + latch_growth.error().format();
  if (!blocked_loop)
    latch_message += ": blocked loop failed: " + blocked_loop.error().format();
  if (!blocked_entries)
    latch_message += ": load failed: " + blocked_entries.error().format();
  else if (blocked_entries->size() != source_count + 2)
    latch_message += ": unexpected entry count " + std::to_string(blocked_entries->size());
  latch_message += ": output=" + blocked_out.str();
  expect(!latched && latch_growth && blocked_loop && blocked_entries && blocked_entries->size() == source_count + 2 &&
             blocked_out.str().find("blocked-summary") != std::string::npos && blocked_out.str().find("append_commit_state") != std::string::npos,
         latch_message);
}

}  // namespace ava::tests::app_rpc_test
