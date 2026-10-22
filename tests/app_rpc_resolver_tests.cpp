#include "sys.h"
#include "tests/support/app_rpc_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "ava/event/events.h"
#include "ava/app/headless_policy.h"
#include "ava/app/rpc/output.h"
#include "ava/app/rpc/resolvers.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/json.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "debug.h"

namespace {

using namespace ava::tests;

std::string wait_for_pending_permission(ThreadSafeStringBuf& output, ava::app::rpc::PendingResolverState& pending)
{
  if (!require(output.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(5)), "RPC resolver publishes its permission request"))
    return {};
  std::lock_guard lock(pending.mutex);
  if (!require(!pending.permission_requests.empty(), "RPC published permission request remains pending until its reply"))
    return {};
  return pending.permission_requests.begin()->first;
}

class ResolverPublicationStreamBuf final : public std::streambuf
{
 public:
  explicit ResolverPublicationStreamBuf(bool block_first_write = false, bool fail_after_release = false)
      : block_first_write_(block_first_write), fail_after_release_(fail_after_release)
  {
  }

  bool wait_until_write_started(std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return write_started_; });
  }

  void release_write()
  {
    {
      std::lock_guard lock(mutex_);
      write_released_ = true;
    }
    cv_.notify_all();
  }

  std::string str() const
  {
    std::lock_guard lock(mutex_);
    return text_;
  }

 protected:
  int overflow(int ch) override
  {
    if (ch == traits_type::eof())
      return traits_type::not_eof(ch);
    char const value = static_cast<char>(ch);
    return write(&value, 1) == 1 ? ch : traits_type::eof();
  }

  std::streamsize xsputn(char const* data, std::streamsize count) override { return write(data, count); }

  int sync() override
  {
    std::lock_guard lock(mutex_);
    return fail_after_release_ ? -1 : 0;
  }

 private:
  std::streamsize write(char const* data, std::streamsize count)
  {
    std::unique_lock lock(mutex_);
    text_.append(data, static_cast<std::size_t>(count));
    if (block_first_write_ && !write_started_)
    {
      write_started_ = true;
      cv_.notify_all();
      cv_.wait(lock, [&] { return write_released_; });
    }
    return count;
  }

  bool block_first_write_ = false;
  bool fail_after_release_ = false;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::string text_;
  bool write_started_ = false;
  bool write_released_ = false;
};

void test_app_rpc_resolver_payload_builders_preserve_wire_shapes()
{
  auto const permission_payload_json = std::string("{\"resolver_request_id\":\"permission_1\",\"operation\":\"read\",\"reason\":\"needs read\"}");
  auto permission = ava::app::rpc::resolver_event_envelope("permission_requested", "request_1", "correlation_1", "session_1",
                                                           ava::app::rpc::resolver_permission_payload(permission_payload_json));
  expect(permission.payload_type == "permission" && permission.payload_json == permission_payload_json,
         "typed permission resolver payload builder preserves the existing JSON fields");
  auto const permission_json = ava::event::serialize_event_envelope_json(permission);
  expect(permission_json.find("\"payload_type\":\"permission\"") != std::string::npos &&
             permission_json.find("\"payload\":" + permission_payload_json) != std::string::npos &&
             permission_json.find("\"reason\":\"needs read\"") != std::string::npos,
         "typed permission resolver envelope keeps payload family and compatibility aliases");

  auto const question_payload_json = std::string(
      "{\"resolver_request_id\":\"question_1\",\"prompt\":\"Choose\",\"options\":[\"yes\"],"
      "\"multiple\":false}");
  auto question = ava::app::rpc::resolver_event_envelope("question_requested", "request_2", "correlation_2", "session_1",
                                                         ava::app::rpc::resolver_question_payload(question_payload_json));
  expect(question.payload_type == "question" && question.payload_json == question_payload_json,
         "typed question resolver payload builder preserves the existing JSON fields");

  auto const queue_payload_json = std::string("{\"message\":\"follow next\",\"reason\":\"canceled\"}");
  auto queue = ava::app::rpc::resolver_event_envelope("follow_up_skipped", "request_3", "correlation_3", "session_1",
                                                      ava::app::rpc::resolver_queue_payload(queue_payload_json));
  expect(queue.payload_type == "queue" && queue.payload_json == queue_payload_json, "typed queue resolver payload builder preserves the existing JSON fields");
  auto const queue_json = ava::event::serialize_event_envelope_json(queue);
  expect(queue_json.find("\"payload_type\":\"queue\"") != std::string::npos && queue_json.find("\"message\":\"follow next\"") != std::string::npos &&
             queue_json.find("\"reason\":\"canceled\"") != std::string::npos,
         "typed queue resolver envelope keeps payload family and top-level aliases");
}

void test_app_rpc_resolver_output_failure_callback_preserves_lock_order()
{
  auto const root = create_empty_root("app-rpc-resolver-write-failure");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = app_test_paths(root);
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC resolver write failure test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::app::rpc::PendingResolverState pending_state;
  ava::app::rpc::RpcRunState run_state;
  std::ostringstream stream;
  stream.setstate(std::ios::badbit);
  ava::app::rpc::output_ts output(stream, [&] { static_cast<void>(ava::app::rpc::cancel_pending_resolvers(output, pending_state)); });
  auto resolver = ava::app::rpc::make_rpc_permission_resolver(pending_state, output, run_state, unlocked_session, nullptr, "prompt-write-fail");
  auto result = resolver(ava::permissions::PermissionPrompt{.permission_request_id = "permreq_write_fail",
                                                            .operation = ava::permissions::Operation::ReadFile,
                                                            .mode = ava::agent::Mode::Build,
                                                            .workspace_dir = workspace,
                                                            .target_path = workspace / "note.txt",
                                                            .command = "",
                                                            .tool_name = "read_file",
                                                            .reason = "test write failure",
                                                            .risk = ava::permissions::PermissionRisk::Low,
                                                            .diff_preview = "",
                                                            .diff_truncated = false});
  bool no_pending = false;
  {
    std::lock_guard lock(pending_state.mutex);
    no_pending = pending_state.permission_requests.empty() && pending_state.question_requests.empty();
  }
  expect(!result && result.error().category() == ava::core::ErrorCategory::Io && no_pending,
         "RPC resolver output-failure callback acquires output before pending state and releases waiters without a user timeout");
}

enum class ResolverRaceKind
{
  Permission,
  Question,
};

struct ResolverRaceObservation
{
  bool pending_inserted = false;
  bool publication_started = false;
  bool cancellation_applied = false;
  bool resolver_canceled = false;
  bool request_published = false;
};

ResolverRaceObservation run_resolver_cancellation_race(ResolverRaceKind kind, bool cancellation_wins)
{
  ResolverRaceObservation observed;
  ava::app::rpc::PendingResolverState pending_state;
  ResolverPublicationStreamBuf output_buffer(!cancellation_wins);
  std::ostream out(&output_buffer);
  ava::app::rpc::output_ts output(out, [] { });
  std::string const request_id = kind == ResolverRaceKind::Permission ? "permission-race" : "question-race";
  std::string const record = kind == ResolverRaceKind::Permission ? "permission_requested\n" : "question_requested\n";

  if (kind == ResolverRaceKind::Permission)
  {
    auto pending = std::make_shared<ava::app::rpc::PendingPermissionRequest>();
    {
      std::lock_guard lock(pending_state.mutex);
      pending_state.permission_requests.emplace(request_id, pending);
      observed.pending_inserted = true;
    }

    auto gate = [&] {
      std::lock_guard lock(pending_state.mutex);
      auto const found = pending_state.permission_requests.find(request_id);
      return found != pending_state.permission_requests.end() && found->second == pending;
    };
    if (cancellation_wins)
    {
      observed.cancellation_applied = ava::app::rpc::cancel_pending_resolvers(output, pending_state);
      auto written = ava::app::rpc::Output::write_record_if(output, record, gate);
      observed.resolver_canceled = written && *written == ava::app::rpc::OutputWriteResult::Skipped && pending->resolved && pending->error;
    }
    else
    {
      ava::core::Result<ava::app::rpc::OutputWriteResult> written = ava::app::rpc::OutputWriteResult::Skipped;
      ava::core::JoinThread publisher = ava::core::JoinThread::create("publisher", [&] { written = ava::app::rpc::Output::write_record_if(output, record, gate); });
      observed.publication_started = output_buffer.wait_until_write_started(std::chrono::seconds(2));
      ava::core::JoinThread canceler = ava::core::JoinThread::create("canceler", [&] { observed.cancellation_applied = ava::app::rpc::cancel_pending_resolvers(output, pending_state); });
      output_buffer.release_write();
      publisher.join();
      canceler.join();
      observed.resolver_canceled = written && *written == ava::app::rpc::OutputWriteResult::Written && pending->resolved && pending->error;
    }
  }
  else
  {
    auto pending = std::make_shared<ava::app::rpc::PendingQuestionRequest>();
    {
      std::lock_guard lock(pending_state.mutex);
      pending_state.question_requests.emplace(request_id, pending);
      observed.pending_inserted = true;
    }

    auto gate = [&] {
      std::lock_guard lock(pending_state.mutex);
      auto const found = pending_state.question_requests.find(request_id);
      return found != pending_state.question_requests.end() && found->second == pending;
    };
    if (cancellation_wins)
    {
      observed.cancellation_applied = ava::app::rpc::cancel_pending_resolvers(output, pending_state);
      auto written = ava::app::rpc::Output::write_record_if(output, record, gate);
      observed.resolver_canceled = written && *written == ava::app::rpc::OutputWriteResult::Skipped && pending->resolved && pending->error;
    }
    else
    {
      ava::core::Result<ava::app::rpc::OutputWriteResult> written = ava::app::rpc::OutputWriteResult::Skipped;
      ava::core::JoinThread publisher = ava::core::JoinThread::create("publisher", [&] { written = ava::app::rpc::Output::write_record_if(output, record, gate); });
      observed.publication_started = output_buffer.wait_until_write_started(std::chrono::seconds(2));
      ava::core::JoinThread canceler = ava::core::JoinThread::create("canceler", [&] { observed.cancellation_applied = ava::app::rpc::cancel_pending_resolvers(output, pending_state); });
      output_buffer.release_write();
      publisher.join();
      canceler.join();
      observed.resolver_canceled = written && *written == ava::app::rpc::OutputWriteResult::Written && pending->resolved && pending->error;
    }
  }
  observed.request_published =
      output_buffer.str().find(kind == ResolverRaceKind::Permission ? "permission_requested" : "question_requested") != std::string::npos;
  return observed;
}

void test_app_rpc_resolver_publication_linearizes_permission_and_question_cancellation()
{
  for (auto const kind : {ResolverRaceKind::Permission, ResolverRaceKind::Question})
  {
    auto const canceled = run_resolver_cancellation_race(kind, true);
    auto const published = run_resolver_cancellation_race(kind, false);
    auto const kind_name = kind == ResolverRaceKind::Permission ? std::string("permission") : std::string("question");
    expect(canceled.pending_inserted && canceled.cancellation_applied && canceled.resolver_canceled && !canceled.request_published,
           "RPC " + kind_name + " resolver cancellation wins before publication without emitting a stale request");
    expect(published.pending_inserted && published.publication_started && published.cancellation_applied && published.resolver_canceled &&
               published.request_published,
           "RPC " + kind_name + " resolver publication wins once its gated record enters the output stream");
  }
}

void test_app_rpc_resolver_exact_request_identity_gates_publication_and_cleanup()
{
  ava::app::rpc::PendingResolverState gate_state;
  auto original = std::make_shared<ava::app::rpc::PendingPermissionRequest>();
  auto gate_replacement = std::make_shared<ava::app::rpc::PendingPermissionRequest>();
  {
    std::lock_guard lock(gate_state.mutex);
    gate_state.permission_requests.emplace("permission-replaced", original);
    gate_state.permission_requests["permission-replaced"] = gate_replacement;
  }
  std::ostringstream gate_stream;
  ava::app::rpc::output_ts gate_output(gate_stream, [] { });
  auto skipped = ava::app::rpc::Output::write_record_if(gate_output, "permission_requested\n", [&] {
    std::lock_guard lock(gate_state.mutex);
    auto const found = gate_state.permission_requests.find("permission-replaced");
    return found != gate_state.permission_requests.end() && found->second == original;
  });
  bool gate_preserved_replacement = false;
  {
    std::lock_guard lock(gate_state.mutex);
    auto const found = gate_state.permission_requests.find("permission-replaced");
    gate_preserved_replacement = found != gate_state.permission_requests.end() && found->second == gate_replacement;
  }

  auto const root = create_empty_root("app-rpc-resolver-exact-identity");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = app_test_paths(root);
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC resolver exact-identity test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::app::rpc::PendingResolverState pending_state;
  ava::app::rpc::RpcRunState run_state;
  ResolverPublicationStreamBuf output_buffer(true, true);
  std::ostream out(&output_buffer);
  ava::app::rpc::output_ts output(out, [] { });
  auto resolver = ava::app::rpc::make_rpc_permission_resolver(pending_state, output, run_state, unlocked_session, nullptr, "prompt-identity");
  ava::core::Result<ava::permissions::PermissionResolutionDecision> result = ava::permissions::PermissionResolution::Deny;
  ava::core::JoinThread resolver_thread = ava::core::JoinThread::create("resolver_thread", [&] {
    result = resolver(ava::permissions::PermissionPrompt{.permission_request_id = "permreq-identity",
                                                         .operation = ava::permissions::Operation::ReadFile,
                                                         .mode = ava::agent::Mode::Build,
                                                         .workspace_dir = workspace,
                                                         .target_path = workspace / "note.txt",
                                                         .command = "",
                                                         .tool_name = "read_file",
                                                         .reason = "identity test",
                                                         .risk = ava::permissions::PermissionRisk::Low,
                                                         .diff_preview = "",
                                                         .diff_truncated = false});
  });

  bool const write_started = output_buffer.wait_until_write_started(std::chrono::seconds(2));
  bool request_inserted = false;
  std::string request_id;
  auto replacement = std::make_shared<ava::app::rpc::PendingPermissionRequest>();
  bool replaced = false;
  {
    std::lock_guard lock(pending_state.mutex);
    request_inserted = !pending_state.permission_requests.empty();
    if (request_inserted)
    {
      request_id = pending_state.permission_requests.begin()->first;
      pending_state.permission_requests.begin()->second = replacement;
      replaced = true;
    }
  }
  output_buffer.release_write();
  resolver_thread.join();

  bool replacement_retained = false;
  {
    std::lock_guard lock(pending_state.mutex);
    auto const found = pending_state.permission_requests.find(request_id);
    replacement_retained = found != pending_state.permission_requests.end() && found->second == replacement;
  }
  expect(skipped && *skipped == ava::app::rpc::OutputWriteResult::Skipped && gate_stream.str().empty() && gate_preserved_replacement && request_inserted &&
             write_started && replaced && !result && result.error().category() == ava::core::ErrorCategory::Io && replacement_retained,
         "RPC resolver publication gates and output-failure cleanup preserve an exact shared request replacement");
}

void test_app_rpc_permission_policy_auto_allows_before_resolver_event()
{
  auto const root = create_empty_root("app-rpc-policy-auto-allow");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside permission note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC permission policy auto-allow test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string())), sse_response(final_text_sse("after policy allow"))});
  ava::app::HeadlessPermissionPolicyOptions policy_options;
  auto read_only_added = ava::app::add_headless_allow_policy(policy_options, "read-only");
  expect(read_only_added.has_value(), "RPC permission policy test configures read-only allow");
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  runtime_options.permission_resolver = ava::app::build_headless_permission_resolver(policy_options);
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read outside\"}\n");
  bool const completed = output_buffer.wait_contains("after policy allow", std::chrono::seconds(2));
  expect(wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2)), "RPC auto-allowed prompt reaches its terminal response before EOF");
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value() && completed, "RPC permission policy auto-allow prompt completes");
  expect(jsonl.find("permission_requested") == std::string::npos, "RPC permission policy auto-allows matching read prompt before resolver event");
}

void test_app_rpc_permission_reply_allow_and_deny_flows()
{
  for (std::string_view decision : {"allow", "deny"})
  {
    auto const decision_text = std::string(decision);
    auto const root = create_empty_root("app-rpc-permission-" + decision_text);

    auto const workspace = root / "workspace";
    auto const paths = app_test_paths(root);
    std::filesystem::create_directories(workspace);
    {
      write_test_file(workspace / "note.txt", "rpc permission note");
    }
    auto const outside_path = root / "outside.txt";
    {
      write_test_file(outside_path, "outside permission note");
    }

    ava::app::runtime::OpenContext open_context;
    open_context.workspace_dir = workspace;
    open_context.current_dir = workspace;
    open_context.paths = paths;
    auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
    expect(unlocked_session_result.has_value(), "RPC permission reply test opens runtime session");
    if (!unlocked_session_result)
      return;
    ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

    ava::provider::OpenAIProvider const provider("https://api.example.test");
    ava::tests::FakeTransport transport(
        {sse_response(read_file_call_sse(outside_path.generic_string())), sse_response(final_text_sse("after " + decision_text))});
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

    input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read note\"}\n");
    bool const requested = output_buffer.wait_contains("\"resolver_request_id\":\"permission_", std::chrono::seconds(2));
    auto const resolver_request_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
    expect(requested && !resolver_request_id.empty(), "RPC permission reply test observes resolver request id");
    auto const reply_reason_json = decision == "deny" ? std::string(",\"reason\":\"not approved for this run\"") : std::string();
    input_buffer.push("{\"id\":\"reply\",\"type\":\"permission_reply\",\"request_id\":\"" + resolver_request_id +
                      "\",\"correlation_id\":\"p1\",\"decision\":\"" + decision_text + "\"" + reply_reason_json + "}\n");
    bool const completed = output_buffer.wait_contains("after " + decision_text, std::chrono::seconds(2));
    expect(wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2)), "RPC permission " + decision_text + " prompt reaches its terminal response");
    input_buffer.close();
    rpc_thread.join();

    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    auto const jsonl = output_buffer.str();
    expect(result.has_value() && completed, "RPC permission reply loop exits successfully");
    auto const reply_response = rpc_response_line(jsonl, "reply").value_or("");
    auto const prompt_response = rpc_response_line(jsonl, "p1").value_or("");
    expect_all("RPC permission " + decision_text + " reply unblocks its run",
               {{"reply success", reply_response.find("\"success\":true") != std::string_view::npos},
                {"prompt answer", prompt_response.find("after " + decision_text) != std::string_view::npos},
                {"reply event", jsonl.find("\"name\":\"permission_replied\"") != std::string::npos},
                {"decision", jsonl.find("\"decision\":\"" + decision_text + "\"") != std::string::npos},
                {"second provider request", transport.requests().size() == 2}});
    if (transport.requests().size() == 2)
      expect_eq(transport.requests()[1].body.find("outside permission note") != std::string::npos, decision == "allow",
                "RPC permission " + decision_text + " controls whether protected file content reaches the provider");
    if (decision == "deny")
    {
      expect(jsonl.find("\"reason\":\"not approved for this run\"") != std::string::npos,
             "RPC permission deny reply preserves the client resolution reason in the event stream");
      auto entries = session_r->store.load();
      auto audits = entries ? permission_entries(*entries) : std::vector<ava::session::SessionEntry>{};
      expect(audits.size() >= 2 && ava::core::json::string_field(audits.back().data_json, "resolution") == "deny" &&
                 ava::core::json::string_field(audits.back().data_json, "resolution_source") == "resolver" &&
                 ava::core::json::string_field(audits.back().data_json, "resolution_reason") == "not approved for this run",
             "RPC permission deny reply preserves the client resolution reason in durable audit entries");
    }
  }
}

void test_app_rpc_permission_reply_session_grant_flow()
{
  auto const root = create_empty_root("app-rpc-permission-session-grant");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    write_test_file(outside_path, "outside grant note");
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC permission session grant test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(read_file_call_sse(outside_path.generic_string(), "call_read_first")), sse_response(final_text_sse("first grant done")),
       sse_response(read_file_call_sse(outside_path.generic_string(), "call_read_second")), sse_response(final_text_sse("second grant done"))});
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

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read outside once\"}\n");
  bool const requested = output_buffer.wait_contains("\"resolver_request_id\":\"permission_", std::chrono::seconds(2));
  auto const resolver_request_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(requested && !resolver_request_id.empty(), "RPC permission session grant test observes resolver request id");
  input_buffer.push("{\"id\":\"reply\",\"type\":\"permission_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"decision\":\"allow_session\"}\n");
  input_buffer.push("{\"id\":\"p2\",\"type\":\"follow_up\",\"message\":\"read outside again\"}\n");
  bool const first_completed = wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"grants\",\"type\":\"permission_grants\"}\n");
  bool const grant_listed = wait_for_rpc_response(output_buffer, "grants", std::chrono::seconds(2));
  bool const second_completed = wait_for_rpc_response(output_buffer, "p2", std::chrono::seconds(2));
  auto const grant_output = output_buffer.str();
  auto const grant_response = rpc_response_line(grant_output, "grants").value_or("");
  auto const grant_id = extract_json_string_field(grant_response, "grant_id");
  expect(!grant_id.empty(), "RPC grant listing returns the grant identity for revocation");
  input_buffer.push("{\"id\":\"revoke\",\"type\":\"permission_grant_revoke\",\"grant_id\":\"" + grant_id + "\"}\n");
  input_buffer.push("{\"id\":\"revoke-again\",\"type\":\"permission_grant_revoke\",\"grant_id\":\"" + grant_id + "\"}\n");
  input_buffer.push("{\"id\":\"after-revoke\",\"type\":\"permission_grants\"}\n");
  expect(wait_for_rpc_response(output_buffer, "after-revoke", std::chrono::seconds(2)), "RPC grant revocation and listing complete");
  input_buffer.close();
  rpc_thread.join();

  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  auto const jsonl = output_buffer.str();
  auto const revoked = rpc_response_line(jsonl, "revoke").value_or("");
  auto const unknown = rpc_response_line(jsonl, "revoke-again").value_or("");
  auto const after_revoke = rpc_response_line(jsonl, "after-revoke").value_or("");
  expect_all("RPC grant revocation removes the grant and rejects repeated revocation",
             {{"revoked", revoked.find("\"revoked\":true") != std::string_view::npos},
              {"revoked identity", !grant_id.empty() && revoked.find(grant_id) != std::string_view::npos},
              {"unknown rejected", unknown.find("\"code\":\"invalid_request\"") != std::string_view::npos},
              {"grant absent", after_revoke.find("\"grants\":[]") != std::string_view::npos}});
  expect(result.has_value() && first_completed && grant_listed && second_completed, "RPC permission session grant loop exits successfully");
  expect_all("RPC grant list correlates its session, operation, and exact target",
             {{"grant identity", !grant_id.empty()},
              {"session", grant_response.find("\"session_id\":\"" + session_r->store.options_r()->session_id + "\"") != std::string_view::npos},
              {"operation", grant_response.find("\"operation\":\"read\"") != std::string_view::npos},
              {"target", grant_response.find("\"target_path\":\"" + outside_path.string() + "\"") != std::string_view::npos}});
  expect(count_substrings(jsonl, "\"name\":\"permission_requested\"") == 1 && jsonl.find("\"payload_type\":\"permission\"") != std::string::npos &&
             jsonl.find("\"decision\":\"allow_session\"") != std::string::npos && jsonl.find("\"id\":\"grants\"") != std::string::npos &&
             jsonl.find("\"session_id\":\"" + session_r->store.options_r()->session_id + "\"") != std::string::npos &&
             jsonl.find("\"operation\":\"read\"") != std::string::npos && jsonl.find("\"target_path\":\"" + outside_path.string() + "\"") != std::string::npos,
         "RPC session grants are serialized with their exact session and suppress only repeated matching prompts");

  auto entries = session_r->store.load();
  auto audits = entries ? permission_entries(*entries) : std::vector<ava::session::SessionEntry>{};
  expect(audits.size() == 4 && ava::core::json::string_field(audits[3].data_json, "resolution_source") == "session_grant" &&
             ava::core::json::string_field(audits[3].data_json, "resolution") == "allow",
         "session grant approvals are still audited as backend permission outcomes");
}

void test_app_rpc_session_grants_are_exact_session_scoped_and_cannot_override_deny()
{
  auto const root = create_empty_root("app-rpc-session-grant-bounds");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = app_test_paths(root);
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC session grant bounds test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::app::rpc::PendingResolverState pending_state;
  ava::app::rpc::RpcRunState run_state;
  ThreadSafeStringBuf output_buffer;
  std::ostream output_stream(&output_buffer);
  ava::app::rpc::output_ts output(output_stream, [] { });
  ava::permissions::CommandPermissionMetadata metadata;
  metadata.level = ava::command::CommandLevel::Standard;
  metadata.containment_status = ava::permissions::CommandContainmentStatus::Available;
  metadata.backend_maximum_scope = ava::command::InteractiveScope::Workspace;
  metadata.global_recipe_key = "sha256:ava-command-recipe-v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  metadata.workspace_recipe_key = "sha256:ava-command-workspace-recipe-v1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  metadata.effective_allowed_scopes = {ava::command::InteractiveScope::Once, ava::command::InteractiveScope::Session,
                                       ava::command::InteractiveScope::Workspace};
  ava::permissions::PermissionPrompt const prompt{.permission_request_id = "permreq_build_test",
                                                  .operation = ava::permissions::Operation::RunCommand,
                                                  .mode = ava::agent::Mode::Build,
                                                  .workspace_dir = workspace,
                                                  .target_path = {},
                                                  .command = "ctest --test-dir build",
                                                  .tool_name = "bash",
                                                  .reason = "repository test execution requires explicit approval",
                                                  .risk = ava::permissions::PermissionRisk::High,
                                                  .command_metadata = metadata};
  {
    std::lock_guard lock(pending_state.mutex);
    pending_state.permission_session_grants.push_back(ava::app::rpc::PermissionSessionGrant{
        .grant_id = "permgrant_matching",
        .permission_request_id = prompt.permission_request_id,
        .session_id = ava::app::runtime::session_ts::rat(unlocked_session)->store.options_r()->session_id,
        .operation = prompt.operation,
        .mode = prompt.mode,
        .tool_name = prompt.tool_name,
        .target_path = prompt.target_path,
        .command = prompt.command,
        .command_recipe_key = metadata.workspace_recipe_key,
        .command_recipe_display = "ctest",
        .reason = "explicit current-session test grant",
        .risk = prompt.risk,
    });
  }
  auto resolver = ava::app::rpc::make_rpc_permission_resolver(pending_state, output, run_state, unlocked_session, nullptr, "prompt_1");
  auto matched = resolver(prompt);
  expect(matched && *matched == ava::permissions::PermissionResolution::AllowSessionGrant,
         "a session grant authorizes a matching stable workspace recipe rather than an execution fingerprint");

  {
    std::lock_guard lock(pending_state.mutex);
    pending_state.permission_session_grants.front().session_id = "session_other";
  }
  std::optional<ava::core::Result<ava::permissions::PermissionResolutionDecision>> mismatched;
  ava::core::JoinThread resolver_thread = ava::core::JoinThread::create("resolver_thread", [&] { mismatched = resolver(prompt); });
  auto const resolver_request_id = wait_for_pending_permission(output_buffer, pending_state);
  auto denied = resolver_request_id.empty()
                    ? ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing permission request")))
                    : ava::app::rpc::resolve_permission_reply(pending_state, resolver_request_id, "prompt_1", "deny");
  if (resolver_request_id.empty())
  {
    run_state.cancel_requested.store(true);
    static_cast<void>(ava::app::rpc::cancel_pending_resolvers(output, pending_state));
  }
  resolver_thread.join();
  expect(!resolver_request_id.empty() && denied && mismatched && *mismatched && **mismatched == ava::permissions::PermissionResolution::Deny,
         "a grant from another session cannot authorize an otherwise matching repository test invocation");

  {
    std::lock_guard lock(pending_state.mutex);
    pending_state.permission_session_grants.clear();
  }
  std::optional<ava::core::Result<ava::permissions::PermissionResolutionDecision>> one_shot;
  ava::core::JoinThread one_shot_thread = ava::core::JoinThread::create("one_shot_thread", [&] { one_shot = resolver(prompt); });
  auto const one_shot_request_id = wait_for_pending_permission(output_buffer, pending_state);
  auto one_shot_allowed = one_shot_request_id.empty()
                              ? ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing permission request")))
                              : ava::app::rpc::resolve_permission_reply(pending_state, one_shot_request_id, "prompt_1", "allow");
  if (one_shot_request_id.empty())
  {
    run_state.cancel_requested.store(true);
    static_cast<void>(ava::app::rpc::cancel_pending_resolvers(output, pending_state));
  }
  one_shot_thread.join();
  expect(!one_shot_request_id.empty() && one_shot_allowed && one_shot && *one_shot && **one_shot == ava::permissions::PermissionResolution::Allow,
         "a one-shot approval authorizes the requested repository test invocation");

  {
    std::lock_guard lock(pending_state.mutex);
    pending_state.permission_session_grants.push_back(ava::app::rpc::PermissionSessionGrant{
        .grant_id = "permgrant_matching_again",
        .permission_request_id = prompt.permission_request_id,
        .session_id = ava::app::runtime::session_ts::rat(unlocked_session)->store.options_r()->session_id,
        .operation = prompt.operation,
        .mode = prompt.mode,
        .tool_name = prompt.tool_name,
        .target_path = prompt.target_path,
        .command = prompt.command,
        .command_recipe_key = metadata.workspace_recipe_key,
        .command_recipe_display = "ctest",
        .reason = "explicit current-session test grant",
        .risk = prompt.risk,
    });
  }
  auto hard_deny = ava::app::rpc::make_rpc_permission_resolver(
      pending_state, output, run_state, unlocked_session,
      [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::Deny, "hard policy deny"};
        decision.authoritative = true;
        return decision;
      },
      "prompt_2");
  auto denied_by_policy = hard_deny(prompt);
  expect(denied_by_policy && *denied_by_policy == ava::permissions::PermissionResolution::Deny,
         "an authoritative deny takes precedence over a matching session grant");
}

void test_app_rpc_command_one_shot_blocks_reusable_grants()
{
  auto const root = temp_root() / "app-rpc-command-one-shot";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const paths = app_test_paths(root);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC command one-shot test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::app::rpc::PendingResolverState pending_state;
  ava::app::rpc::RpcRunState run_state;
  ThreadSafeStringBuf output_buffer;
  std::ostream output_stream(&output_buffer);
  ava::app::rpc::output_ts output(output_stream, [] { });

  // The sealed command path always attaches metadata with
  // backend_maximum_scope == Once, so command_permission_allows_reusable_grant
  // returns false and allow_session replies must not create grants.
  ava::permissions::CommandPermissionMetadata metadata;
  metadata.level = ava::command::CommandLevel::Critical;
  metadata.backend_maximum_scope = ava::command::InteractiveScope::Once;
  metadata.fingerprint = "sha256:ava-command-plan-v3:one-shot-regression";
  metadata.executor_identity_verified = true;

  ava::permissions::PermissionPrompt const prompt{.permission_request_id = "permreq_one_shot",
                                                  .operation = ava::permissions::Operation::RunCommand,
                                                  .mode = ava::agent::Mode::Build,
                                                  .workspace_dir = workspace,
                                                  .target_path = {},
                                                  .command = "true",
                                                  .tool_name = "bash",
                                                  .reason = "sealed command requires approval",
                                                  .risk = ava::permissions::PermissionRisk::Critical,
                                                  .command_metadata = metadata};

  auto resolver = ava::app::rpc::make_rpc_permission_resolver(pending_state, output, run_state, unlocked_session, nullptr, "prompt_os");

  // First invocation: reply with allow_session. Because the command is
  // one-shot only, no session grant must be created.
  std::optional<ava::core::Result<ava::permissions::PermissionResolutionDecision>> first_result;
  ava::core::JoinThread first_thread = ava::core::JoinThread::create("first_thread", [&] { first_result = resolver(prompt); });
  auto const first_request_id = wait_for_pending_permission(output_buffer, pending_state);
  auto first_reply = first_request_id.empty()
                         ? ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing first permission request")))
                         : ava::app::rpc::resolve_permission_reply(pending_state, first_request_id, "prompt_os", "allow_session");
  if (first_request_id.empty())
  {
    run_state.cancel_requested.store(true);
    static_cast<void>(ava::app::rpc::cancel_pending_resolvers(output, pending_state));
  }
  first_thread.join();
  bool no_grant_after_first = false;
  {
    std::lock_guard lock(pending_state.mutex);
    no_grant_after_first = pending_state.permission_session_grants.empty();
  }
  expect(!first_request_id.empty() && first_reply && first_result && *first_result && **first_result == ava::permissions::PermissionResolution::Allow &&
             no_grant_after_first,
         "allow_session for a one-shot command returns Allow without creating a reusable session grant");

  // Second invocation: because no grant was created, the resolver must be
  // called again and create a fresh pending request.
  std::optional<ava::core::Result<ava::permissions::PermissionResolutionDecision>> second_result;
  ava::core::JoinThread second_thread = ava::core::JoinThread::create("second_thread", [&] { second_result = resolver(prompt); });
  auto const second_request_id = wait_for_pending_permission(output_buffer, pending_state);
  auto second_reply = second_request_id.empty()
                          ? ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing second permission request")))
                          : ava::app::rpc::resolve_permission_reply(pending_state, second_request_id, "prompt_os", "allow");
  if (second_request_id.empty())
  {
    run_state.cancel_requested.store(true);
    static_cast<void>(ava::app::rpc::cancel_pending_resolvers(output, pending_state));
  }
  second_thread.join();
  bool no_grant_after_second = false;
  {
    std::lock_guard lock(pending_state.mutex);
    no_grant_after_second = pending_state.permission_session_grants.empty();
  }
  expect(!second_request_id.empty() && second_reply && second_result && *second_result && **second_result == ava::permissions::PermissionResolution::Allow &&
             no_grant_after_second && first_request_id != second_request_id,
         "the permission resolver is called twice for the same one-shot command and no reusable grant is created");
}

void test_app_rpc_permission_request_includes_mutation_diff_preview()
{
  auto const root = create_empty_root("app-rpc-permission-diff");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside-created.txt";

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC permission diff test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(write_file_call_sse(outside_path.generic_string(), "rpc new\n")), sse_response(final_text_sse("after diff deny"))});
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

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"write outside\"}\n");
  bool const requested = output_buffer.wait_contains("\"diff_preview\"", std::chrono::seconds(2));
  auto const resolver_request_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  auto const permission_request_id = extract_json_string_field(output_buffer.str(), "permission_request_id");
  expect(requested && !resolver_request_id.empty() && permission_request_id.starts_with("permreq_"),
         "RPC permission diff test observes mutation diff preview with stable permission request id");
  input_buffer.push("{\"id\":\"reply\",\"type\":\"permission_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"decision\":\"deny\"}\n");
  bool const completed = output_buffer.wait_contains("after diff deny", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value() && completed && !std::filesystem::exists(outside_path), "RPC permission diff test completes after denied mutation without writing");
  expect(jsonl.find("\"name\":\"permission_requested\"") != std::string::npos && jsonl.find("\"payload_type\":\"permission\"") != std::string::npos &&
             jsonl.find("\"permission_request_id\":\"" + permission_request_id + "\"") != std::string::npos &&
             jsonl.find("\"diff_preview\"") != std::string::npos && jsonl.find("+rpc new") != std::string::npos &&
             jsonl.find("\"diff_truncated\":false") != std::string::npos && jsonl.find("\"name\":\"permission_replied\"") != std::string::npos &&
             jsonl.find("\"decision\":\"deny\"") != std::string::npos && jsonl.find("request_id: " + permission_request_id) != std::string::npos &&
             jsonl.find("/permissions audit show " + permission_request_id) != std::string::npos &&
             jsonl.find("/permissions diagnose " + permission_request_id) != std::string::npos,
         "RPC permission request payload includes backend-provided unified diff preview, reply event, and actionable denial details");
}

void test_app_rpc_persistent_permission_rule_lifecycle()
{
  auto const root = create_empty_root("app-rpc-persistent-permission-rule");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    std::ofstream file(outside_path, std::ios::binary | std::ios::trunc);
    file << "outside persistent rule note";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC persistent permission rule test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(read_file_call_sse(outside_path.generic_string())), sse_response(final_text_sse("persistent rule allow done"))});
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
  auto const rpc_timeout = std::chrono::seconds(5);

  input_buffer.push(
      "{\"id\":\"bad-rule\",\"type\":\"permission_rule_add\",\"action\":\"allow\","
      "\"target_path\":\"" +
      outside_path.string() + "\",\"reason\":\"missing operation\"}\n");
  bool const invalid_rejected = wait_for_rpc_response(output_buffer, "bad-rule", rpc_timeout);
  input_buffer.push(
      "{\"id\":\"add-rule\",\"type\":\"permission_rule_add\",\"action\":\"allow\","
      "\"operation\":\"read\",\"target_path\":\"" +
      outside_path.string() + "\",\"reason\":\"allow exact outside read\"}\n");
  bool const added = output_buffer.wait_contains("\"name\":\"permission_rule_added\"", rpc_timeout);
  auto const rule_id = extract_json_string_field(output_buffer.str(), "rule_id");
  input_buffer.push("{\"id\":\"rules\",\"type\":\"permission_rules\"}\n");
  bool const listed = wait_for_rpc_response(output_buffer, "rules", rpc_timeout);
  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"read outside via persistent rule\"}\n");
  bool const completed = wait_for_rpc_response(output_buffer, "p1", rpc_timeout);
  input_buffer.push("{\"id\":\"remove-rule\",\"type\":\"permission_rule_remove\",\"rule_id\":\"" + rule_id + "\"}\n");
  bool const removed = wait_for_rpc_response(output_buffer, "remove-rule", rpc_timeout);
  bool const removal_event = output_buffer.wait_for_record(
      [&rule_id](std::string_view record) {
        if (ava::core::json::string_field(record, "name") != "permission_rule_removed" || ava::core::json::string_field(record, "request_id") != "remove-rule")
          return false;
        auto const payload = ava::core::json::object_field(record, "payload").value_or("{}");
        auto const rule = ava::core::json::object_field(payload, "rule").value_or("{}");
        return payload.find("\"removed\":true") != std::string::npos && ava::core::json::string_field(rule, "rule_id") == rule_id;
      },
      rpc_timeout);
  input_buffer.close();
  rpc_thread.join();

  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);

  auto const jsonl = output_buffer.str();
  auto const invalid_response = rpc_response_line(jsonl, "bad-rule").value_or("");
  auto const invalid_error = ava::core::json::object_field(invalid_response, "error").value_or("{}");
  expect_all("RPC malformed persistent rule is rejected with invalid_request",
             {{"failed response", invalid_response.find("\"success\":false") != std::string_view::npos},
              {"stable code", ava::core::json::string_field(invalid_error, "code") == "invalid_request"}});
  auto const added_response = rpc_response_line(jsonl, "add-rule").value_or("");
  auto const added_result = ava::core::json::object_field(added_response, "result").value_or("{}");
  auto const added_rule = ava::core::json::object_field(added_result, "rule").value_or("{}");
  expect_all("RPC persistent rule addition returns the published rule",
             {{"successful response", added_response.find("\"success\":true") != std::string_view::npos},
              {"published rule identity", !rule_id.empty() && ava::core::json::string_field(added_rule, "rule_id") == rule_id}});
  auto const listed_response = rpc_response_line(jsonl, "rules").value_or("");
  auto const listed_result = ava::core::json::object_field(listed_response, "result").value_or("{}");
  auto const listed_rules = ava::core::json::objects_in_array_field(listed_result, "rules");
  bool const expected_rule_listed = std::ranges::any_of(
      listed_rules, [&rule_id](std::string const& rule) { return !rule_id.empty() && ava::core::json::string_field(rule, "rule_id") == rule_id; });
  expect_all("RPC persistent rule listing includes the added rule",
             {{"successful response", listed_response.find("\"success\":true") != std::string_view::npos}, {"expected rule identity", expected_rule_listed}});
  auto const prompt_response = rpc_response_line(jsonl, "p1").value_or("");
  auto const prompt_result = ava::core::json::object_field(prompt_response, "result").value_or("{}");
  expect_all("RPC persistent rule permits the prompt to return its expected answer",
             {{"successful response", prompt_response.find("\"success\":true") != std::string_view::npos},
              {"expected answer", ava::core::json::string_field(prompt_result, "final_text") == "persistent rule allow done"}});
  auto const removed_response = rpc_response_line(jsonl, "remove-rule").value_or("");
  auto const removed_result = ava::core::json::object_field(removed_response, "result").value_or("{}");
  auto const removed_rule = ava::core::json::object_field(removed_result, "rule").value_or("{}");
  expect_all("RPC persistent rule removal returns and emits removal of the expected rule",
             {{"successful response", removed_response.find("\"success\":true") != std::string_view::npos},
              {"removed flag", removed_result.find("\"removed\":true") != std::string::npos},
              {"removed rule identity", !rule_id.empty() && ava::core::json::string_field(removed_rule, "rule_id") == rule_id},
              {"correlated removal event", removal_event}});
  auto entries = session_r->store.load();
  auto audits = entries ? permission_entries(*entries) : std::vector<ava::session::SessionEntry>{};
  bool persistent_audited = false;
  for (auto const& audit : audits)
  {
    persistent_audited = persistent_audited || (ava::core::json::string_field(audit.data_json, "resolution_source") == "persistent_rule" &&
                                                ava::core::json::string_field(audit.data_json, "rule_id") == rule_id &&
                                                ava::core::json::string_field(audit.data_json, "actor") == "agent");
  }
  // Temporary split of boolean test.
  std::cout << "jsonl = \"" << jsonl << "\"." << std::endl;
  expect(result.has_value(), "result.has_value()");
  expect(invalid_rejected, "invalid_rejected");
  expect(added, "added");
  expect(!rule_id.empty(), "!rule_id.empty()");
  expect(listed, "listed");
  expect(completed, "completed");
  expect(removed, "removed");
  expect_all("RPC persistent permission rule add/list/apply/remove flow completes", {{"loop", result.has_value()},
                                                                                     {"invalid rule rejected", invalid_rejected},
                                                                                     {"rule added", added},
                                                                                     {"rule id", !rule_id.empty()},
                                                                                     {"rule listed", listed},
                                                                                     {"prompt completed", completed},
                                                                                     {"rule removed", removed}});
  // expect(false, "Forced failure");
  expect(jsonl.find("\"name\":\"permission_requested\"") == std::string::npos && persistent_audited,
         "persistent permission rules resolve matching RPC permission prompts without resolver events and are audited");
}

void test_app_rpc_question_reply_flow()
{
  auto const root = create_empty_root("app-rpc-question");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC question reply test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(question_call_sse()), sse_response(final_text_sse("question done"))});
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

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"ask question\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"question_requested\"", std::chrono::seconds(2));
  auto const resolver_request_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(requested && !resolver_request_id.empty(), "RPC question reply test observes question request event");
  input_buffer.push("{\"id\":\"reply\",\"type\":\"question_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"answer\":\"custom ok\"}\n");
  bool const completed = output_buffer.wait_contains("question done", std::chrono::seconds(2));
  expect(wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2)), "RPC question prompt reaches its terminal response before EOF");
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value() && completed, "RPC question reply loop exits successfully");
  auto const reply_response = rpc_response_line(jsonl, "reply").value_or("");
  auto const prompt_response = rpc_response_line(jsonl, "p1").value_or("");
  expect_all("RPC question reply emits its event and unblocks the matching prompt",
             {{"reply success", reply_response.find("\"success\":true") != std::string_view::npos},
              {"answer", prompt_response.find("question done") != std::string_view::npos},
              {"question event", jsonl.find("\"payload_type\":\"question\"") != std::string::npos},
              {"reply event", jsonl.find("\"name\":\"question_replied\"") != std::string::npos},
              {"custom text", jsonl.find("\"answer\":\"custom ok\"") != std::string::npos}});
}

void test_app_rpc_question_reply_selected_option_flow()
{
  auto const root = create_empty_root("app-rpc-question-selected");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC selected question reply test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(question_call_sse()), sse_response(final_text_sse("selected question done"))});
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

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"ask question\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"question_requested\"", std::chrono::seconds(2));
  auto const resolver_request_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(requested && !resolver_request_id.empty(), "RPC selected question reply observes request event");
  input_buffer.push("{\"id\":\"bad\",\"type\":\"question_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"selected\":\"no\"}\n");
  bool const rejected = wait_for_rpc_response(output_buffer, "bad", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"reply\",\"type\":\"question_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"selected\":\"yes\"}\n");
  bool const completed = output_buffer.wait_contains("selected question done", std::chrono::seconds(2));
  expect(wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2)), "RPC selected question prompt reaches its terminal response before EOF");
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value() && rejected && completed, "RPC selected question reply loop exits successfully");
  auto const rejected_response = rpc_response_line(jsonl, "bad").value_or("");
  auto const reply_response = rpc_response_line(jsonl, "reply").value_or("");
  auto const prompt_response = rpc_response_line(jsonl, "p1").value_or("");
  expect(rejected_response.find("\"code\":\"invalid_request\"") != std::string_view::npos,
         "RPC selected question reply rejects an invalid option without resolving the pending request");
  expect_all("RPC selected question reply unblocks the matching question",
             {{"reply success", reply_response.find("\"success\":true") != std::string_view::npos},
              {"prompt answer", prompt_response.find("selected question done") != std::string_view::npos},
              {"reply event", jsonl.find("\"name\":\"question_replied\"") != std::string::npos},
              {"selected option", jsonl.find("\"selected\":\"yes\"") != std::string::npos}});
}

void test_app_rpc_question_reply_selected_options_flow()
{
  auto const root = create_empty_root("app-rpc-question-selected-options");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC multi question reply test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(multi_question_call_sse()), sse_response(final_text_sse("multi question done"))});
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

  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"ask multi question\"}\n");
  bool const requested = output_buffer.wait_contains("\"name\":\"question_requested\"", std::chrono::seconds(2));
  auto const resolver_request_id = extract_json_string_field(output_buffer.str(), "resolver_request_id");
  expect(requested && !resolver_request_id.empty(), "RPC multi question reply observes request event");
  input_buffer.push("{\"id\":\"bad\",\"type\":\"question_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"selected_options\":[\"alpha\",\"alpha\"]}\n");
  bool const duplicate_rejected = wait_for_rpc_response(output_buffer, "bad", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"reply\",\"type\":\"question_reply\",\"request_id\":\"" + resolver_request_id +
                    "\",\"correlation_id\":\"p1\",\"selected_options\":[\"alpha\",\"beta\"],"
                    "\"answer\":\"Use both\"}\n");
  bool const completed = wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value() && duplicate_rejected && completed, "RPC multi question reply loop exits successfully");
  expect(jsonl.find("\"multiple\":true") != std::string::npos, "RPC multi question reply request preserves multi-select metadata");
  auto const rejected_response = rpc_response_line(jsonl, "bad").value_or("");
  auto const reply_response = rpc_response_line(jsonl, "reply").value_or("");
  auto const prompt_response = rpc_response_line(jsonl, "p1").value_or("");
  expect(rejected_response.find("\"code\":\"invalid_request\"") != std::string_view::npos,
         "RPC multi question reply rejects duplicate selected options without resolving the pending request");
  expect_all("RPC multi question reply publishes its selections and unblocks the matching prompt",
             {{"reply success", reply_response.find("\"success\":true") != std::string_view::npos},
              {"prompt answer", prompt_response.find("multi question done") != std::string_view::npos},
              {"reply event", jsonl.find("\"name\":\"question_replied\"") != std::string::npos},
              {"selections", jsonl.find("\"selected_options\":[\"alpha\",\"beta\"]") != std::string::npos},
              {"custom text", jsonl.find("\"answer\":\"Use both\"") != std::string::npos}});
}

void test_app_rpc_grant_clear_and_foreign_identity_isolation()
{
  ava::app::rpc::PendingResolverState current;
  ava::app::rpc::PendingResolverState other;
  auto grant = [](std::string id, std::string session) {
    return ava::app::rpc::PermissionSessionGrant{.grant_id = std::move(id),
                                                 .permission_request_id = {},
                                                 .session_id = std::move(session),
                                                 .operation = ava::permissions::Operation::ReadFile,
                                                 .mode = ava::agent::Mode::Build,
                                                 .tool_name = "read_file",
                                                 .target_path = {},
                                                 .command = {},
                                                 .command_recipe_key = {},
                                                 .command_recipe_display = {},
                                                 .reason = {},
                                                 .risk = ava::permissions::PermissionRisk::Low};
  };
  current.permission_session_grants.push_back(grant("grant_a", "session_a"));
  current.permission_session_grants.push_back(grant("grant_b", "session_a"));
  other.permission_session_grants.push_back(grant("grant_other", "session_other"));
  auto foreign = ava::app::rpc::permission_session_grant_revoke_result_json(current, "grant_other");
  expect_all("RPC foreign grant identity cannot mutate another connection's grant set",
             {{"rejected", !foreign && foreign.error().category() == ava::core::ErrorCategory::InvalidArgument},
              {"current retained", current.permission_session_grants.size() == 2},
              {"other retained", other.permission_session_grants.size() == 1}});
  auto const cleared = ava::app::rpc::permission_session_grants_clear_result_json(current);
  expect_all("RPC clearing grants removes every current grant without affecting another connection",
             {{"reported count", ava::core::json::integer_field(cleared, "cleared") == 2},
              {"current empty", current.permission_session_grants.empty()},
              {"serialized empty", ava::app::rpc::permission_session_grants_result_json(current) == "{\"grants\":[]}"},
              {"other retained", other.permission_session_grants.size() == 1}});
  auto const cleared_again = ava::app::rpc::permission_session_grants_clear_result_json(current);
  expect_eq(ava::core::json::integer_field(cleared_again, "cleared").value_or(-1), 0LL, "RPC clearing an empty grant set is idempotent");
}

void test_app_rpc_grants_clear_requires_permission_again()
{
  auto root = ScopedTestDirectory::create_unique(temp_root(), "rpc-clear-grants-");
  auto const workspace = root / "workspace";
  auto const outside = root / "outside.txt";
  write_test_file(outside, "GRANT_CLEAR_PRIVATE_CONTENT");
  auto opened = open_runtime_session_for_test(root.path(), workspace);
  if (!require(opened.has_value(), "RPC grant clearing fixture opens a runtime session"))
    return;
  auto& session = *opened;
  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = app_test_paths(root.path());
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside.generic_string(), "before_clear")), sse_response(final_text_sse("before clear")),
                                       sse_response(read_file_call_sse(outside.generic_string(), "after_clear")), sse_response(final_text_sse("after clear"))});
  ava::app::runtime::RunOptions options;
  options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream input(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream output(&output_buffer);
  ava::core::VoidResult result;
  auto worker = ava::core::JoinThread::create("rpc_clear_grants", [&] {
    result = ava::app::run_rpc_loop(session, context, provider, transport, options, input, output, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"first\",\"type\":\"prompt\",\"message\":\"read outside\"}\n");
  bool const first_requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  auto const first_id = extract_last_json_string_field(output_buffer.str(), "resolver_request_id");
  input_buffer.push("{\"id\":\"allow\",\"type\":\"permission_reply\",\"request_id\":\"" + first_id +
                    "\",\"correlation_id\":\"first\",\"decision\":\"allow_session\"}\n");
  bool const first_completed = wait_for_rpc_response(output_buffer, "first", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"clear\",\"type\":\"permission_grants_clear\"}\n");
  input_buffer.push("{\"id\":\"listed\",\"type\":\"permission_grants\"}\n");
  bool const listed = wait_for_rpc_response(output_buffer, "listed", std::chrono::seconds(2));
  // Distinguish a new read from content legitimately retained from the first run.
  write_test_file(outside, "GRANT_CLEAR_AFTER_CLEAR_CONTENT");
  input_buffer.push("{\"id\":\"second\",\"type\":\"prompt\",\"message\":\"read outside after clear\"}\n");
  bool const requested_again = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  auto const second_id = extract_last_json_string_field(output_buffer.str(), "resolver_request_id");
  input_buffer.push("{\"id\":\"deny\",\"type\":\"permission_reply\",\"request_id\":\"" + second_id +
                    "\",\"correlation_id\":\"second\",\"decision\":\"deny\"}\n");
  bool const second_completed = wait_for_rpc_response(output_buffer, "second", std::chrono::seconds(2));
  input_buffer.close();
  worker.join();
  auto const jsonl = output_buffer.str();
  auto const cleared = rpc_response_line(jsonl, "clear").value_or("");
  auto const grants = rpc_response_line(jsonl, "listed").value_or("");
  auto const second = rpc_response_line(jsonl, "second").value_or("");
  expect_all("RPC permission_grants_clear removes a real grant and requires a fresh decision for the same file",
             {{"loop", result.has_value()},
              {"first permission", first_requested},
              {"first prompt", first_completed},
              {"cleared one", cleared.find("\"cleared\":1") != std::string_view::npos},
              {"empty list", listed && grants.find("\"grants\":[]") != std::string_view::npos},
              {"fresh permission", requested_again && !second_id.empty() && second_id != first_id},
              {"second prompt", second_completed && second.find("\"success\":true") != std::string_view::npos},
              {"two permission events", count_substrings(jsonl, "\"name\":\"permission_requested\"") == 2},
              {"provider requests", transport.requests().size() == 4}});
  if (transport.requests().size() == 4)
    expect_all("RPC grant clearing preserves the allowed read but blocks newly denied file content",
               {{"allowed read reached provider", transport.requests()[1].body.find("GRANT_CLEAR_PRIVATE_CONTENT") != std::string::npos},
                {"denied new content absent", transport.requests()[3].body.find("GRANT_CLEAR_AFTER_CLEAR_CONTENT") == std::string::npos}});
}

}  // namespace

void run_app_rpc_resolver_tests()
{
  ava::tests::HermeticTestEnvironment environment("ava-rpc-resolver-");
  test_app_rpc_resolver_payload_builders_preserve_wire_shapes();
  test_app_rpc_resolver_output_failure_callback_preserves_lock_order();
  test_app_rpc_resolver_publication_linearizes_permission_and_question_cancellation();
  test_app_rpc_resolver_exact_request_identity_gates_publication_and_cleanup();
  test_app_rpc_permission_policy_auto_allows_before_resolver_event();
  test_app_rpc_permission_reply_allow_and_deny_flows();
  test_app_rpc_permission_reply_session_grant_flow();
  test_app_rpc_grant_clear_and_foreign_identity_isolation();
  test_app_rpc_grants_clear_requires_permission_again();
  test_app_rpc_session_grants_are_exact_session_scoped_and_cannot_override_deny();
  test_app_rpc_command_one_shot_blocks_reusable_grants();
  test_app_rpc_permission_request_includes_mutation_diff_preview();
  test_app_rpc_persistent_permission_rule_lifecycle();
  test_app_rpc_question_reply_flow();
  test_app_rpc_question_reply_selected_option_flow();
  test_app_rpc_question_reply_selected_options_flow();
}
