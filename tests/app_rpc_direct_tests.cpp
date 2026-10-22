#include "sys.h"
#include "tests/app_rpc_test_cases.h"
#include "tests/support/app_rpc_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/http/transport.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/session/record.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/json.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <istream>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace ava::tests::app_rpc_test {

void test_app_rpc_direct_run_command_permission_reply_executes_and_audits()
{
  auto const root = create_empty_root("app-rpc-direct-run-command");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "RPC direct command fixture workspace is owner-only for sealed command planning");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC direct command test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out,
                                    [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push(R"JSON({"id":"cmd-allow","type":"run_command","command":"printf rpc-direct"})JSON"
                    "\n");
  bool const permission_requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  auto const request_id = rpc_string_field_from_output(output_buffer.str(), "resolver_request_id");
  expect(permission_requested && request_id, "RPC direct command emits an Operation::RunCommand permission request before execution");
  if (request_id)
  {
    input_buffer.push("{\"id\":\"allow\",\"type\":\"permission_reply\",\"request_id\":\"" + *request_id +
                      "\",\"correlation_id\":\"cmd-allow\",\"decision\":\"allow\",\"reason\":\"approved direct rpc\"}\n");
  }
  bool const completed = wait_for_rpc_response(output_buffer, "cmd-allow", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  auto const jsonl = output_buffer.str();
  auto entries = session_r->store.load();
  auto const audited_allow = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                               return entry.type == ava::session::EntryType::PermissionDecision &&
                                      entry.data_json.find("\"operation\":\"bash\"") != std::string::npos &&
                                      entry.data_json.find("\"command\":\"<redacted one-shot command>\"") != std::string::npos &&
                                      entry.data_json.find("\"resolution\":\"allow\"") != std::string::npos;
                             });
  expect(result.has_value(), "RPC direct command loop completes successfully");
  expect(transport.requests().empty(), "RPC direct command execution does not dispatch provider requests");
  expect(completed && jsonl.find("\"id\":\"cmd-allow\"") != std::string::npos && jsonl.find("\"success\":true") != std::string::npos &&
             jsonl.find("\"operation\":\"bash\"") != std::string::npos && jsonl.find("\"command\":\"printf rpc-direct\"") != std::string::npos &&
             jsonl.find("\"family\":\"raw_shell\"") != std::string::npos && jsonl.find("\"backend_maximum_scope\":\"once\"") != std::string::npos &&
             jsonl.find("\"tool\":\"bash\"") != std::string::npos && jsonl.find("\"permission_request_ids\":[\"permreq_") != std::string::npos,
         "RPC direct command is a one-shot raw-shell bash operation linked to its permission request");
  expect(audited_allow, "RPC direct command persists permission audit decisions in the session");
}

void test_app_rpc_direct_run_command_permission_denial_blocks_execution()
{
  auto const root = create_empty_root("app-rpc-direct-run-command-deny");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "RPC direct command denial fixture workspace is owner-only for sealed command planning");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC direct command denial test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out,
                                    [&] noexcept { input_buffer.close(); });
  });

  std::string const secret = "RPC_DENIED_COMMAND_SECRET_SENTINEL";
  input_buffer.push("{\"id\":\"cmd-deny\",\"type\":\"run_bash\",\"command\":\"printf " + secret + " > denied.txt\"}\n");
  bool const permission_requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  auto const request_id = rpc_string_field_from_output(output_buffer.str(), "resolver_request_id");
  expect(permission_requested && request_id && output_buffer.str().find(secret) != std::string::npos,
         "RPC direct command denial emits a permission prompt that retains the authorized user's exact command");
  if (request_id)
  {
    input_buffer.push("{\"id\":\"deny\",\"type\":\"permission_reply\",\"request_id\":\"" + *request_id +
                      "\",\"correlation_id\":\"cmd-deny\",\"decision\":\"deny\",\"reason\":\"not approved\"}\n");
  }
  bool const denied = wait_for_rpc_response(output_buffer, "cmd-deny", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  auto const jsonl = output_buffer.str();
  auto entries = session_r->store.load();
  auto const audited_deny = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                              return entry.type == ava::session::EntryType::PermissionDecision &&
                                     entry.data_json.find("\"operation\":\"bash\"") != std::string::npos &&
                                     entry.data_json.find("\"command\":\"<redacted one-shot command>\"") != std::string::npos &&
                                     entry.data_json.find("\"resolution\":\"deny\"") != std::string::npos;
                            });
  expect(result.has_value(), "RPC direct command denial loop completes successfully");
  auto const session_secret_absent =
      entries && std::ranges::all_of(*entries, [&](ava::session::SessionEntry const& entry) { return entry.data_json.find(secret) == std::string::npos; });
  auto const denied_response = rpc_response_line(jsonl, "cmd-deny").value_or("");
  auto const command_result = ava::core::json::object_field(denied_response, "result");
  auto const timeline = command_result ? ava::core::json::objects_in_array_field(*command_result, "tool_timeline") : std::vector<std::string>{};
  bool const bash_failed = std::ranges::any_of(timeline, [](std::string const& entry) {
    return ava::core::json::string_field(entry, "tool") == "bash" && ava::core::json::string_field(entry, "status") == "error";
  });
  expect_all("RPC direct denial returns its failed bash result without leaking command arguments",
             {{"terminal response", denied}, {"bash error status", bash_failed}, {"argument only in permission prompt", count_substrings(jsonl, secret) == 1}});
  expect(!std::filesystem::exists(workspace / "denied.txt"), "RPC direct command denial blocks process execution before side effects");
  expect(audited_deny && session_secret_absent, "RPC direct command denial persists redacted session audits without command arguments");
}

void test_app_rpc_direct_run_command_active_rejects_and_cancels_process()
{
  auto const root = create_empty_root("app-rpc-direct-run-command-cancel");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "RPC direct command cancellation fixture workspace is owner-only for sealed command planning");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC direct command cancellation test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  auto const sleep_marker = workspace / "sleep-started";
  auto const sleep_command = "/bin/sh -c 'touch " + sleep_marker.string() + "; sleep 5'";
  input_buffer.push("{\"id\":\"cmd-sleep\",\"type\":\"run_command\",\"command\":\"" + ava::core::json::escape(sleep_command) + "\"}\n");
  bool const started = output_buffer.wait_contains("\"name\":\"tool_start\"", std::chrono::seconds(2));
  auto const launch_deadline = ava::tests::now_plus_seconds(2);
  while (!std::filesystem::exists(sleep_marker) && std::chrono::steady_clock::now() < launch_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  bool const process_started = std::filesystem::exists(sleep_marker);
  input_buffer.push(R"JSON({"id":"cmd-second","type":"run_bash","command":"printf should-not-run"})JSON"
                    "\n");
  bool const active_rejected = output_buffer.wait_contains("RPC command is unavailable while a prompt is active", std::chrono::seconds(2));
  input_buffer.push(R"JSON({"id":"cancel-sleep","type":"cancel"})JSON"
                    "\n");
  bool const canceled = output_buffer.wait_contains("\"canceled\":true", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC direct command cancellation loop completes successfully");
  expect(transport.requests().empty(), "RPC direct command cancellation does not dispatch provider requests");
  expect(started && process_started && active_rejected && canceled && jsonl.find("\"id\":\"cmd-second\"") != std::string::npos &&
             jsonl.find("should-not-run") == std::string::npos && jsonl.find("\"id\":\"cmd-sleep\"") != std::string::npos,
         "RPC direct command rejects concurrent commands and cancels the running process through the bash tool context");
}

void test_app_rpc_compact_provider_failure_is_error_response()
{
  auto const root = create_empty_root("app-rpc-compact-failure");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC compact failure test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "{\"error\":{\"message\":\"summary failed\"}}"}});
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";

  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"cmp-fail\",\"type\":\"compact\"}\n");
  bool const failed = output_buffer.wait_contains("compaction summary request failed with status 500", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  auto const jsonl = output_buffer.str();
  auto entries = session_r->store.load();
  expect(result.has_value() && failed, "RPC compact failure loop completes after error response");
  expect(jsonl.find("\"id\":\"cmp-fail\"") != std::string::npos && jsonl.find("\"success\":false") != std::string::npos &&
             jsonl.find("compaction summary request failed with status 500") != std::string::npos && jsonl.find("summary failed") == std::string::npos,
         "RPC compact provider failures are machine-readable responses without provider-controlled diagnostics");
  expect(entries && count_compaction_entries(*entries) == 0, "RPC compact provider failure leaves session without a compaction entry");
}

void test_app_rpc_compact_cancellation_is_error_response_without_provider_request()
{
  auto const root = create_empty_root("app-rpc-compact-canceled");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC compact cancellation test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"unused\"}"}});
  std::istringstream in("{\"id\":\"cmp-cancel\",\"type\":\"compact\"}\n");
  std::ostringstream out;
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  runtime_options.cancel_requested = [] { return true; };

  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, ava::app::rpc::RpcInputWake{});
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  auto const jsonl = out.str();
  auto entries = session_r->store.load();
  expect(result.has_value(), "RPC compact cancellation loop completes after error response");
  expect(jsonl.find("\"id\":\"cmp-cancel\"") != std::string::npos && jsonl.find("\"success\":false") != std::string::npos &&
             jsonl.find("agent loop canceled") != std::string::npos,
         "RPC compact cancellation is returned as a machine-readable error response");
  expect(transport.requests().empty(), "RPC compact cancellation avoids dispatching a provider summary request");
  expect(entries && count_compaction_entries(*entries) == 0, "RPC compact cancellation leaves session without a compaction entry");
}

}  // namespace ava::tests::app_rpc_test
