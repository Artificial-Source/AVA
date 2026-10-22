#include "sys.h"
#include "tests/support/app_rpc_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/rpc/input.h"
#include "ava/app/rpc/run_state.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/config/auth.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/thread.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <sys/stat.h>

namespace {

using namespace ava::tests;

void test_app_rpc_follow_up_transition_state_machine_is_atomic()
{
  using ava::app::rpc::RpcFollowUpTransitionKind;
  using ava::app::rpc::RpcRunKind;

  ava::app::rpc::RpcRunState canceled_state;
  ava::app::rpc::set_active_run(canceled_state, RpcRunKind::Prompt, "parent");
  auto queued = ava::app::rpc::queue_rpc_message(canceled_state.follow_up_messages, canceled_state, "follow_up", "child", "next");
  ava::app::rpc::begin_prompt_terminal_publication(canceled_state);
  auto cancellation = ava::app::rpc::begin_cancellation(canceled_state);
  bool retained_during_publication = false;
  {
    std::lock_guard lock(canceled_state.mutex);
    retained_during_publication = canceled_state.active_request_id == "parent" && canceled_state.follow_up_messages.size() == 1 &&
                                  canceled_state.outstanding_request_ids.contains("parent") && canceled_state.outstanding_request_ids.contains("child");
  }
  auto canceled = ava::app::rpc::transition_after_prompt_terminal_response(canceled_state);
  ava::app::rpc::complete_terminal_publication(canceled_state, "parent");
  expect_all("RPC cancellation retains publication identity until the post-flush transition skips the child",
             {{"queued", queued.has_value()},
              {"deferred", cancellation.deferred_to_terminal_publication},
              {"deferred child count", cancellation.deferred_follow_up_count == 1},
              {"identity retained", retained_during_publication},
              {"skipped", canceled.kind == RpcFollowUpTransitionKind::Skipped},
              {"not activated", !canceled.follow_up},
              {"cleared child", canceled.cleared.follow_up_messages.size() == 1 && canceled.cleared.follow_up_messages.front().request_id == "child"},
              {"parent inactive", !ava::app::rpc::active_run(canceled_state)}});

  ava::app::rpc::RpcRunState admitted_state;
  ava::app::rpc::set_active_run(admitted_state, RpcRunKind::Prompt, "parent");
  auto admitted = ava::app::rpc::queue_rpc_message(admitted_state.follow_up_messages, admitted_state, "follow_up", "child", "next");
  ava::app::rpc::begin_prompt_terminal_publication(admitted_state);
  bool child_shared_until_flush = false;
  {
    std::lock_guard lock(admitted_state.mutex);
    child_shared_until_flush = admitted_state.active_request_id == "parent" && admitted_state.follow_up_messages.size() == 1;
  }
  auto activated = ava::app::rpc::transition_after_prompt_terminal_response(admitted_state);
  ava::app::rpc::complete_terminal_publication(admitted_state, "parent");
  expect_all("RPC follow-up remains shared until a successful flush then atomically activates",
             {{"queued", admitted.has_value()},
              {"shared until flush", child_shared_until_flush},
              {"activated", activated.kind == RpcFollowUpTransitionKind::Activated},
              {"child identity", activated.follow_up && activated.follow_up->request_id == "child"},
              {"active prompt", ava::app::rpc::active_prompt_run(admitted_state)}});

  ava::app::rpc::RpcRunState deactivated_state;
  ava::app::rpc::set_active_run(deactivated_state, RpcRunKind::Prompt, "parent");
  ava::app::rpc::begin_prompt_terminal_publication(deactivated_state);
  auto deactivated = ava::app::rpc::transition_after_prompt_terminal_response(deactivated_state);
  ava::app::rpc::complete_terminal_publication(deactivated_state, "parent");
  auto late = ava::app::rpc::queue_rpc_message(deactivated_state.follow_up_messages, deactivated_state, "follow_up", "late", "too late");
  expect_all("RPC deactivation rejects late follow-up admission without an orphaned queue item",
             {{"deactivated", deactivated.kind == RpcFollowUpTransitionKind::Deactivated},
              {"inactive", !ava::app::rpc::active_run(deactivated_state)},
              {"late rejected", !late},
              {"invalid argument", !late && late.error().category() == ava::core::ErrorCategory::InvalidArgument}});

  ava::app::rpc::RpcRunState final_record_state;
  ava::app::rpc::set_active_run(final_record_state, RpcRunKind::Prompt, "parent");
  auto final_record_queued = ava::app::rpc::queue_rpc_message(final_record_state.follow_up_messages, final_record_state, "follow_up", "child", "next");
  ava::app::rpc::observe_input_terminal(final_record_state, ava::app::rpc::RpcInputTerminalOutcome::EofWithFinalRecord);
  auto const final_record_admission = ava::app::rpc::await_command_admission(final_record_state, "final-record-command");
  ava::app::rpc::begin_prompt_terminal_publication(final_record_state);
  auto final_record_transition = ava::app::rpc::transition_after_prompt_terminal_response(final_record_state);
  ava::app::rpc::complete_terminal_publication(final_record_state, "parent");
  bool final_record_open = false;
  {
    std::lock_guard lock(final_record_state.mutex);
    final_record_open =
        final_record_state.input_terminal_observed && !final_record_state.input_closed && !final_record_state.cancel_requested.load(std::memory_order_relaxed);
  }
  expect(final_record_queued && final_record_open && final_record_admission == ava::app::rpc::RpcCommandAdmission::Admitted &&
             final_record_transition.kind == RpcFollowUpTransitionKind::Skipped && final_record_transition.cleared.follow_up_messages.size() == 1 &&
             final_record_transition.cleared.follow_up_messages.front().request_id == "child",
         "RPC final-record input leaves command admission open but atomically skips queued continuation");

  for (auto const outcome :
       {ava::app::rpc::RpcInputTerminalOutcome::Eof, ava::app::rpc::RpcInputTerminalOutcome::Canceled, ava::app::rpc::RpcInputTerminalOutcome::Error})
  {
    ava::app::rpc::RpcRunState terminal_state;
    ava::app::rpc::set_active_run(terminal_state, RpcRunKind::Prompt, "parent");
    auto terminal_queued = ava::app::rpc::queue_rpc_message(terminal_state.follow_up_messages, terminal_state, "follow_up", "child", "next");
    ava::app::rpc::observe_input_terminal(terminal_state, outcome);
    ava::app::rpc::observe_input_terminal(terminal_state, outcome);
    auto first_close = ava::app::rpc::close_input_and_cancel(terminal_state);
    auto second_close = ava::app::rpc::close_input_and_cancel(terminal_state);
    bool terminal_closed = false;
    {
      std::lock_guard lock(terminal_state.mutex);
      terminal_closed =
          terminal_state.input_terminal_observed && terminal_state.input_closed && terminal_state.cancel_requested.load(std::memory_order_relaxed);
    }
    expect(terminal_queued && terminal_closed &&
               ava::app::rpc::await_command_admission(terminal_state, "later") == ava::app::rpc::RpcCommandAdmission::InputClosed &&
               first_close.follow_up_messages.size() == 1 && first_close.follow_up_messages.front().request_id == "child" &&
               second_close.steering_messages.empty() && second_close.follow_up_messages.empty(),
           "RPC non-final input terminal transitions close and cancel idempotently while retaining one queue-drain owner");
  }
}

void test_app_rpc_cancel_affects_subsequent_prompt()
{
  auto const root = create_empty_root("app-rpc-cancel");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC cancel test opens runtime session");
  if (!session)
    return;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(final_text_sse("prompt after cancel"))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"cancel\",\"type\":\"cancel\"}\n");
  bool const canceled = output_buffer.wait_contains("\"id\":\"cancel\"", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"state\",\"type\":\"get_state\"}\n");
  bool const state_reported_cancel = output_buffer.wait_contains("\"cancel_requested\":true", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"prompt\",\"type\":\"prompt\",\"message\":\"should run\"}\n");
  bool const completed = output_buffer.wait_contains("prompt after cancel", std::chrono::seconds(2));
  expect(wait_for_rpc_response(output_buffer, "prompt", std::chrono::seconds(2)), "RPC prompt after cancel reaches its terminal response before EOF");
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC cancel loop completes after prompt response");
  expect(transport.requests().size() == 1, "RPC stale cancel flag is cleared when a subsequent prompt starts");
  expect(jsonl.find("\"id\":\"cancel\"") != std::string::npos && jsonl.find("\"cancel_requested\":true") != std::string::npos &&
             jsonl.find("\"name\":\"cancel_requested\"") != std::string::npos && jsonl.find("\"payload_type\":\"cancellation\"") != std::string::npos &&
             canceled && state_reported_cancel && completed && jsonl.find("\"id\":\"prompt\"") != std::string::npos &&
             jsonl.find("\"success\":true") != std::string::npos,
         "RPC idle cancel updates state until the next accepted prompt clears it and runs normally");
}

void test_app_rpc_active_prompt_cancel_unblocks_pending_permission()
{
  auto const root = create_empty_root("app-rpc-active-cancel");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  {
    write_test_file(workspace / "note.txt", "rpc cancel note");
  }
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside cancel note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC active cancel test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string()))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read note\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  expect(requested, "RPC active cancel test observes pending permission request");
  input_buffer.push("{\"id\":\"cancel\",\"type\":\"cancel\"}\n");
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC active cancel loop exits successfully");
  expect(jsonl.find("\"id\":\"cancel\"") != std::string::npos && jsonl.find("\"active_run\":true") != std::string::npos &&
             jsonl.find("\"name\":\"cancel_requested\"") != std::string::npos && jsonl.find("\"name\":\"canceled\"") != std::string::npos &&
             jsonl.find("\"id\":\"p1\",\"type\":\"response\",\"success\":false") != std::string::npos &&
             jsonl.find("\"category\":\"unknown\",\"code\":\"canceled\",\"message\":\"agent loop canceled\"") != std::string::npos &&
             jsonl.find("\"details\":\"unknown: agent loop canceled\\n  boundary: after_tool_dispatch\"") != std::string::npos,
         "RPC active prompt cancel emits a terminal canceled event and stable canceled response while preserving boundary details");
}

void test_app_rpc_steer_applies_before_next_provider_request()
{
  auto const root = create_empty_root("app-rpc-steer");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside steer note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC steer test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string())), sse_response(final_text_sse("after steer"))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read before steer\"}\n");
  bool const requested = output_buffer.wait_contains("\"resolver_request_id\":\"permission_", std::chrono::seconds(2));
  auto const resolver_request_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(requested && !resolver_request_id.empty(), "RPC steer test observes permission wait safe point");
  input_buffer.push("{\"id\":\"s1\",\"type\":\"steer\",\"message\":\"steer this turn\"}\n");
  input_buffer.push("{\"id\":\"reply\",\"type\":\"permission_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"decision\":\"allow\"}\n");
  bool const completed = output_buffer.wait_contains("after steer", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value() && completed, "RPC steer loop exits successfully");
  expect(transport.requests().size() == 2 && transport.requests()[1].body.find("steer this turn") != std::string::npos,
         "RPC steer is appended before the next provider request after tool completion");
  expect(jsonl.find("\"name\":\"steer_queued\"") != std::string::npos && jsonl.find("\"name\":\"steer_applied\"") != std::string::npos &&
             jsonl.find("\"payload_type\":\"queue\"") != std::string::npos && jsonl.find("\"id\":\"s1\"") != std::string::npos &&
             jsonl.find("\"success\":true") != std::string::npos && jsonl.find("after steer") != std::string::npos,
         "RPC steer emits queued/applied events and active prompt completes");
}

void test_app_rpc_follow_up_runs_after_active_prompt()
{
  auto const root = create_empty_root("app-rpc-follow-up");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside follow note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC follow_up test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string())), sse_response(final_text_sse("first done")),
                                       sse_response(final_text_sse("follow done"))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"first prompt\"}\n");
  bool const requested = output_buffer.wait_contains("\"resolver_request_id\":\"permission_", std::chrono::seconds(2));
  auto const resolver_request_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(requested && !resolver_request_id.empty(), "RPC follow_up test observes active prompt wait");
  input_buffer.push("{\"id\":\"fu1\",\"type\":\"follow_up\",\"message\":\"follow message\"}\n");
  input_buffer.push("{\"id\":\"reply\",\"type\":\"permission_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"decision\":\"allow\"}\n");
  bool const followed = output_buffer.wait_contains("follow done", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  auto const first_response = jsonl.find("\"id\":\"p1\"");
  auto const started = jsonl.find("\"name\":\"follow_up_started\"");
  auto const follow_response = jsonl.find("\"id\":\"fu1\"");
  expect(result.has_value() && followed, "RPC follow_up loop runs queued follow-up successfully");
  expect(transport.requests().size() == 3 && transport.requests()[2].body.find("follow message") != std::string::npos,
         "RPC follow_up starts a new provider turn with the queued message");
  expect(jsonl.find("\"name\":\"follow_up_queued\"") != std::string::npos && first_response != std::string::npos && started != std::string::npos &&
             follow_response != std::string::npos && first_response < started && started < follow_response,
         "RPC follow_up emits queued/started events and responds after the active prompt response");
}

void test_app_rpc_prompt_start_failure_cleans_queued_messages()
{
  auto const root = create_empty_root("app-rpc-prompt-start-fail-queue");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto stored = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                          .access_token = "expired-queued-access",
                                                                                          .refresh_token = "queued-refresh",
                                                                                          .expires_at = 100,
                                                                                          .account_id = "acct_old",
                                                                                          .source_path = {}});
  expect(stored.has_value(), "RPC prompt start failure test stores expired credential");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC prompt start failure test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  BlockingResponseTransport transport(ava::http::HttpResponse{.status_code = 400, .headers = {}, .body = "{\"error\":\"refresh failed\"}"});
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, {}, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"will fail before run\"}\n");
  bool const refresh_requested = transport.wait_for_request(std::chrono::seconds(2));
  expect(refresh_requested, "RPC prompt start failure test blocks during OAuth refresh");
  input_buffer.push("{\"id\":\"s1\",\"type\":\"steer\",\"message\":\"never apply\"}\n");
  input_buffer.push("{\"id\":\"fu1\",\"type\":\"follow_up\",\"message\":\"never run\"}\n");
  bool const queued = output_buffer.wait_contains("\"name\":\"follow_up_queued\"", std::chrono::seconds(2));
  expect(queued, "RPC prompt start failure test queues follow-up before startup failure");
  transport.release();
  bool const skipped = output_buffer.wait_contains("prompt_start_failed", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC prompt start failure queue cleanup loop exits successfully");
  expect(skipped && jsonl.find("\"name\":\"steer_skipped\"") != std::string::npos && jsonl.find("\"name\":\"follow_up_skipped\"") != std::string::npos &&
             jsonl.find("\"request_id\":\"fu1\"") != std::string::npos && jsonl.find("\"id\":\"fu1\"") != std::string::npos &&
             jsonl.find("\"success\":false") != std::string::npos,
         "RPC prompt startup failure emits skipped queue events and a failed follow-up response");
  expect(transport.requests().size() == 1, "RPC prompt startup failure does not run queued follow-up provider calls");
}

void test_app_rpc_steer_after_follow_up_started_targets_follow_up()
{
  auto const root = create_empty_root("app-rpc-follow-up-steer");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside follow steer note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC follow-up steer test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(read_file_call_sse(outside_path.generic_string(), "call_read_parent")), sse_response(final_text_sse("first done")),
       sse_response(read_file_call_sse(outside_path.generic_string(), "call_read_follow_up")), sse_response(final_text_sse("follow steered done"))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"first prompt\"}\n");
  bool const parent_requested = output_buffer.wait_contains("\"resolver_request_id\":\"permission_", std::chrono::seconds(2));
  auto const parent_resolver_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(parent_requested && !parent_resolver_id.empty(), "RPC follow-up steer test observes parent permission wait");
  input_buffer.push("{\"id\":\"fu1\",\"type\":\"follow_up\",\"message\":\"follow message\"}\n");
  input_buffer.push("{\"id\":\"reply1\",\"type\":\"permission_reply\",\"request_id\":\"" + parent_resolver_id +
                    "\",\"correlation_id\":\"p1\",\"decision\":\"allow\"}\n");
  bool const started = output_buffer.wait_contains("\"name\":\"follow_up_started\"", std::chrono::seconds(2));
  expect(started, "RPC follow-up steer test observes follow-up start event");
  input_buffer.push("{\"id\":\"sfu\",\"type\":\"steer\",\"message\":\"steer follow turn\"}\n");
  bool const follow_requested = output_buffer.wait_contains("\"correlation_id\":\"fu1\",\"name\":\"permission_requested\"", std::chrono::seconds(2));
  auto const follow_resolver_id = extract_last_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(follow_requested && !follow_resolver_id.empty(), "RPC follow-up steer test observes follow-up permission wait");
  input_buffer.push("{\"id\":\"reply2\",\"type\":\"permission_reply\",\"request_id\":\"" + follow_resolver_id +
                    "\",\"correlation_id\":\"fu1\",\"decision\":\"allow\"}\n");
  bool const completed = output_buffer.wait_contains("follow steered done", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value() && completed, "RPC follow-up steer loop exits successfully");
  expect(transport.requests().size() == 4 && transport.requests()[3].body.find("steer follow turn") != std::string::npos,
         "RPC steer after follow_up_started is appended to the follow-up continuation request");
  expect(jsonl.find("\"request_id\":\"fu1\",\"correlation_id\":\"fu1\",\"name\":\"follow_up_started\"") != std::string::npos &&
             jsonl.find("\"id\":\"sfu\"") != std::string::npos && jsonl.find("\"correlation_id\":\"fu1\"") != std::string::npos,
         "RPC follow_up_started and subsequent steer use follow-up correlation");
}

void test_app_rpc_queue_limit_rejects_new_items()
{
  auto const root = create_empty_root("app-rpc-queue-limit");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside queue limit note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC queue limit test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string()))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read before limit\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  expect(requested, "RPC queue limit test observes active permission wait");
  for (std::size_t index = 0; index <= ava::app::rpc::kMaxRpcQueuedMessages; ++index)
  {
    input_buffer.push("{\"id\":\"s" + std::to_string(index) + "\",\"type\":\"steer\",\"message\":\"queued steer\"}\n");
  }
  bool const rejected = output_buffer.wait_contains("RPC queued message limit exceeded", std::chrono::seconds(2));
  expect(rejected, "RPC queue limit test observes capped steer rejection");
  input_buffer.push("{\"id\":\"cancel\",\"type\":\"cancel\"}\n");
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC queue limit loop exits successfully");
  auto const rejected_response = rpc_response_line(jsonl, "s" + std::to_string(ava::app::rpc::kMaxRpcQueuedMessages)).value_or("");
  auto const canceled_response = rpc_response_line(jsonl, "cancel").value_or("");
  expect_all(
      "RPC queue limit rejects the next entry and cancellation clears the bounded queue",
      {{"accepted entries", count_substrings(jsonl, "\"name\":\"steer_queued\"") == ava::app::rpc::kMaxRpcQueuedMessages},
       {"rejected overflow", rejected_response.find("\"success\":false") != std::string_view::npos},
       {"stable overflow code", rejected_response.find("\"code\":\"invalid_request\"") != std::string_view::npos},
       {"cleared entries", canceled_response.find("\"cleared_steer\":" + std::to_string(ava::app::rpc::kMaxRpcQueuedMessages)) != std::string_view::npos}});
}

void test_app_rpc_eof_clears_queued_follow_up_without_running()
{
  auto const root = create_empty_root("app-rpc-eof-clears-follow-up");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside eof note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC EOF queue cleanup test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(read_file_call_sse(outside_path.generic_string())), sse_response(final_text_sse("would only run if not canceled"))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read before eof\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  expect(requested, "RPC EOF queue cleanup test observes active permission wait");
  input_buffer.push("{\"id\":\"fu1\",\"type\":\"follow_up\",\"message\":\"must not run\"}\n");
  bool const queued = output_buffer.wait_contains("\"name\":\"follow_up_queued\"", std::chrono::seconds(2));
  expect(queued, "RPC EOF queue cleanup test observes queued follow-up");
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC EOF queue cleanup loop exits successfully");
  expect(transport.requests().size() == 1, "RPC EOF cancels active prompt and prevents queued follow-up provider calls");
  expect(jsonl.find("\"name\":\"follow_up_skipped\"") != std::string::npos && jsonl.find("\"reason\":\"canceled\"") != std::string::npos &&
             jsonl.find("\"id\":\"fu1\"") != std::string::npos && jsonl.find("agent loop canceled") != std::string::npos,
         "RPC EOF clears queued follow-up and sends its canceled response");
}

void test_app_rpc_cancel_clears_queued_steer_and_follow_up()
{
  auto const root = create_empty_root("app-rpc-queue-cancel");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside cancel queue note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC queued cancel test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string()))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read before cancel\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  expect(requested, "RPC queued cancel test observes active permission wait");
  input_buffer.push("{\"id\":\"s1\",\"type\":\"steer\",\"message\":\"never apply\"}\n");
  input_buffer.push("{\"id\":\"s2\",\"type\":\"steer\",\"message\":\"also never apply\"}\n");
  input_buffer.push("{\"id\":\"fu1\",\"type\":\"follow_up\",\"message\":\"never run\"}\n");
  input_buffer.push("{\"id\":\"fu2\",\"type\":\"follow_up\",\"message\":\"also never run\"}\n");
  bool const queued = output_buffer.wait_contains("\"request_id\":\"fu2\"", std::chrono::seconds(2));
  expect(queued, "RPC queued cancel test observes all queued follow-ups before cancellation");
  input_buffer.push("{\"id\":\"cancel\",\"type\":\"cancel\"}\n");
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC queued cancel loop exits successfully");
  expect(transport.requests().size() == 1, "RPC cancel prevents queued steer/follow-up provider requests");
  expect(count_substrings(jsonl, "\"name\":\"steer_queued\"") == 2 && count_substrings(jsonl, "\"name\":\"follow_up_queued\"") == 2 &&
             count_substrings(jsonl, "\"name\":\"steer_skipped\"") == 2 && count_substrings(jsonl, "\"name\":\"follow_up_skipped\"") == 2,
         "RPC active cancel emits one queued and skipped event per queued steer/follow-up");
  expect(
      jsonl.find("\"request_id\":\"cancel\",\"correlation_id\":\"p1\",\"name\":\"cancel_requested\"") != std::string::npos &&
          jsonl.find(
              "\"payload_type\":\"cancellation\",\"payload\":{\"active_run\":true,\"cleared_steer\":2,\"cleared_follow_up\":2,\"active_request_id\":\"p1\"}") !=
              std::string::npos &&
          jsonl.find("\"id\":\"cancel\",\"type\":\"response\",\"success\":true,\"result\":{\"cancel_requested\":true,\"active_run\":true,\"cleared_steer\":2,"
                     "\"cleared_follow_up\":2}}") != std::string::npos,
      "RPC active cancel reports stable active-run cancellation payload counters and active request id");
  expect(count_substrings(jsonl, "\"id\":\"fu1\",\"type\":\"response\",\"success\":false") == 1 &&
             count_substrings(jsonl, "\"id\":\"fu2\",\"type\":\"response\",\"success\":false") == 1 && count_substrings(jsonl, "agent loop canceled") >= 3 &&
             jsonl.find("\"name\":\"follow_up_started\"") == std::string::npos && jsonl.find("\"name\":\"steer_applied\"") == std::string::npos,
         "RPC active cancel clears queued follow-up errors without starting queued turns or applying steer");
}

void test_app_rpc_direct_command_rejects_prompt_only_queue_commands()
{
  auto const root = create_empty_root("app-rpc-direct-queue-reject");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "RPC direct queue workspace is owner-only for sealed command planning");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC direct queue rejection test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  ava::app::runtime::RunOptions runtime_options;
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"bash\",\"type\":\"run_bash\",\"command\":\"printf direct\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  auto const resolver_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(requested && !resolver_id.empty(), "RPC direct queue rejection test observes command permission wait");
  input_buffer.push("{\"id\":\"steer-direct\",\"type\":\"steer\",\"message\":\"no\"}\n");
  input_buffer.push("{\"id\":\"follow-direct\",\"type\":\"follow_up\",\"message\":\"no\"}\n");
  bool const rejected = output_buffer.wait_contains("\"code\":\"active_run\"", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"deny\",\"type\":\"permission_reply\",\"request_id\":\"" + resolver_id +
                    "\",\"correlation_id\":\"bash\",\"decision\":\"deny\"}\n");
  bool const completed = output_buffer.wait_contains("\"id\":\"bash\"", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result && rejected && completed && count_substrings(jsonl, "\"code\":\"active_run\"") == 2 &&
             jsonl.find("\"name\":\"steer_queued\"") == std::string::npos && jsonl.find("\"name\":\"follow_up_queued\"") == std::string::npos,
         "RPC direct command rejects steer and follow_up immediately without queue events");
}

void test_app_rpc_duplicate_outstanding_id_does_not_replace_original()
{
  auto const root = create_empty_root("app-rpc-duplicate-outstanding");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  write_app_test_file(outside_path, "duplicate id note");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC duplicate id test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string())), sse_response(final_text_sse("original done"))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"same\",\"type\":\"prompt\",\"message\":\"original\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  auto const resolver_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  input_buffer.push("{\"id\":\"same\",\"type\":\"get_state\"}\n");
  bool const duplicate = output_buffer.wait_contains("RPC request id is already outstanding", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"allow\",\"type\":\"permission_reply\",\"request_id\":\"" + resolver_id +
                    "\",\"correlation_id\":\"same\",\"decision\":\"allow\"}\n");
  bool const completed = output_buffer.wait_contains("original done", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result && requested && duplicate && completed && jsonl.find("\"code\":\"invalid_request\"") != std::string::npos &&
             jsonl.find("\"final_text\":\"original done\"") != std::string::npos && transport.requests().size() == 2,
         "RPC duplicate outstanding id is rejected without canceling or replacing the original prompt");
}

void test_app_rpc_active_prompt_rejects_second_prompt_and_session_switch()
{
  auto const root = create_empty_root("app-rpc-active-rejects");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  {
    write_test_file(workspace / "note.txt", "rpc active reject note");
  }
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside reject note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  auto unlocked_other_result = ava::app::runtime::Session::open(open_context);
  expect(session.has_value() && unlocked_other_result.has_value(), "RPC active reject test opens runtime sessions");
  if (!session || !unlocked_other_result)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string()))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto const initial_session_id = ava::app::runtime::session_ts::rat(unlocked_session)->store.session_id();
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read note\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  expect(requested, "RPC active reject test observes pending permission request");
  input_buffer.push("{\"id\":\"p2\",\"type\":\"prompt\",\"message\":\"second\"}\n");
  input_buffer.push("{\"id\":\"new\",\"type\":\"new_session\"}\n");
  input_buffer.push("{\"id\":\"switch\",\"type\":\"switch_session\",\"session_id\":\"" + ava::app::runtime::session_ts::rat(*unlocked_other_result)->store.session_id() + "\"}\n");
  input_buffer.push("{\"id\":\"messages\",\"type\":\"get_messages\"}\n");
  for (std::string_view id : {"p2", "new", "switch", "messages"})
    expect(wait_for_rpc_response(output_buffer, id, std::chrono::seconds(2)), "RPC active-run rejection completes for " + std::string(id));
  input_buffer.push("{\"id\":\"cancel\",\"type\":\"cancel\"}\n");
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC active reject loop exits successfully");
  for (std::string_view id : {"p2", "new", "switch", "messages"})
  {
    auto const response = rpc_response_line(jsonl, id).value_or("");
    expect_all("RPC rejects active-run command " + std::string(id), {{"failed response", response.find("\"success\":false") != std::string_view::npos},
                                                                     {"active_run code", response.find("\"code\":\"active_run\"") != std::string_view::npos}});
  }
  expect_eq(count_substrings(jsonl, "\"code\":\"active_run\""), std::size_t{4}, "RPC rejects all four active-run commands");
  expect_eq(ava::app::runtime::session_ts::rat(unlocked_session)->store.session_id(), initial_session_id,
            "RPC rejected session mutations preserve the active session");
}

}  // namespace

void run_app_rpc_queue_tests()
{
  ava::tests::HermeticTestEnvironment environment("ava-rpc-queue-");
  test_app_rpc_follow_up_transition_state_machine_is_atomic();
  test_app_rpc_cancel_affects_subsequent_prompt();
  test_app_rpc_active_prompt_cancel_unblocks_pending_permission();
  test_app_rpc_steer_applies_before_next_provider_request();
  test_app_rpc_follow_up_runs_after_active_prompt();
  test_app_rpc_prompt_start_failure_cleans_queued_messages();
  test_app_rpc_steer_after_follow_up_started_targets_follow_up();
  test_app_rpc_queue_limit_rejects_new_items();
  test_app_rpc_eof_clears_queued_follow_up_without_running();
  test_app_rpc_cancel_clears_queued_steer_and_follow_up();
  test_app_rpc_direct_command_rejects_prompt_only_queue_commands();
  test_app_rpc_duplicate_outstanding_id_does_not_replace_original();
  test_app_rpc_active_prompt_rejects_second_prompt_and_session_switch();
}
