#include "sys.h"
#include "tests/acp_test_declarations.h"
#include "tests/support/acp_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/command/command.h"
#include "ava/app/acp/codec.h"
#include "ava/app/acp/peer.h"
#include "ava/app/acp/service.h"
#include "ava/agent/mode.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/tool_permission.h"
#include "ava/permissions/permission.h"
#include "ava/core/error.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;
using ava::app::acp::JsonRpcId;
using namespace acp_test;
namespace runtime = ava::app::runtime;

namespace {

class RecordingShutdownEscalation final : public ava::app::acp::ShutdownEscalation
{
 public:
  explicit RecordingShutdownEscalation(std::atomic_int& calls) : calls_(calls) { }

  [[noreturn]] void escalate() noexcept override
  {
    calls_.fetch_add(1, std::memory_order_relaxed);
    std::_Exit(ava::app::acp::kShutdownEscalationExitCode);
  }

 private:
  std::atomic_int& calls_;
};

}  // namespace

void test_acp_peer_claimed_outbound_abort_and_delivery_races()
{
  using namespace ava::app::acp;

  auto timeout_state = std::make_shared<MemoryTransportState>();
  timeout_state->block_writes = true;
  JsonRpcPeer timeout_peer(std::make_unique<MemoryTransport>(timeout_state),
                           [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult timeout_run;
  ava::core::JoinThread timeout_thread = ava::core::JoinThread::create("timeout_thread", [&] { timeout_run = timeout_peer.run(); });
  wait_reader(timeout_state);
  auto timed = timeout_peer.send_request("client/claimed-timeout", std::string("{}"), 250ms);
  wait_writer(timeout_state);
  bool const timed_ready = timed && timed->completion.wait_for(2s) == std::future_status::ready;
  auto timed_result = timed_ready ? std::optional<CallResult>(timed->completion.get()) : std::nullopt;
  timeout_thread.join();
  {
    std::lock_guard lock(timeout_state->mutex);
    timeout_state->block_writes = false;
    timeout_state->cv.notify_all();
  }
  expect(timed_result && !*timed_result && timed_result->error().code == -32800 && timeout_run.has_value() && timeout_state->canceled &&
             timeout_state->cancel_calls > 0 && timeout_state->write_attempts == 1 && !take_output(timeout_state, 80ms),
         "a claimed request timeout aborts the transport before bounded completion and never resumes the record");

  auto eof_state = std::make_shared<MemoryTransportState>();
  eof_state->block_writes = true;
  JsonRpcPeer eof_peer(std::make_unique<MemoryTransport>(eof_state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult eof_run;
  ava::core::JoinThread eof_thread = ava::core::JoinThread::create("eof_thread", [&] { eof_run = eof_peer.run(); });
  wait_reader(eof_state);
  auto eof_call = eof_peer.send_request("client/claimed-eof", std::string("{}"), 5s);
  wait_writer(eof_state);
  close_input(eof_state);
  bool const eof_ready = eof_call && eof_call->completion.wait_for(2s) == std::future_status::ready;
  auto eof_result = eof_ready ? std::optional<CallResult>(eof_call->completion.get()) : std::nullopt;
  eof_thread.join();
  expect(eof_result && !*eof_result && eof_result->error().code == -32800 && eof_run.has_value() && eof_state->canceled && eof_state->cancel_calls > 0 &&
             eof_state->write_attempts == 1 && !take_output(eof_state, 80ms),
         "EOF aborts a request that is itself the claimed stalled record and completes peer and future without draining it");

  auto cancel_state = std::make_shared<MemoryTransportState>();
  cancel_state->block_writes = true;
  JsonRpcPeer cancel_peer(std::make_unique<MemoryTransport>(cancel_state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult cancel_run;
  ava::core::JoinThread cancel_thread = ava::core::JoinThread::create("cancel_thread", [&] { cancel_run = cancel_peer.run(); });
  wait_reader(cancel_state);
  auto canceled = cancel_peer.send_request("client/claimed-cancel", std::string("{}"), 5s);
  wait_writer(cancel_state);
  if (canceled)
    cancel_peer.cancel_pending_call(canceled->id, "prompt canceled during claimed permission write");
  bool const cancel_ready = canceled && canceled->completion.wait_for(2s) == std::future_status::ready;
  auto cancel_result = cancel_ready ? std::optional<CallResult>(canceled->completion.get()) : std::nullopt;
  cancel_thread.join();
  expect(cancel_result && !*cancel_result && cancel_result->error().code == -32800 && cancel_run.has_value() && cancel_state->canceled &&
             cancel_state->cancel_calls > 0 && cancel_state->write_attempts == 1 && !take_output(cancel_state, 80ms),
         "cancel_pending_call aborts a claimed request before bounded completion and does not resume it");

  auto staged_state = std::make_shared<MemoryTransportState>();
  staged_state->block_writes = true;
  JsonRpcPeer staged_peer(std::make_unique<MemoryTransport>(staged_state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::JoinThread staged_thread = ava::core::JoinThread::create("staged_thread", [&] { static_cast<void>(staged_peer.run()); });
  wait_reader(staged_state);
  auto staged = staged_peer.send_request("session/request_permission", std::string("{}"), 2s);
  wait_writer(staged_state);
  bool randomized_id = false;
  if (staged)
  {
    auto const* text_id = std::get_if<std::string>(&staged->id);
    randomized_id = text_id && *text_id != "ava-acp-1" && text_id->starts_with("ava-acp-connection_");
    auto guessed = encode_success(staged->id, R"({"outcome":{"outcome":"selected","optionId":"allow_always"}})");
    feed(staged_state, *guessed);
  }
  std::this_thread::sleep_for(20ms);
  bool const fulfilled_before_visible = staged && staged->completion.wait_for(0ms) == std::future_status::ready;
  bool output_before_release = false;
  {
    std::lock_guard lock(staged_state->mutex);
    output_before_release = !staged_state->output.empty();
    staged_state->block_writes = false;
    staged_state->cv.notify_all();
  }
  auto staged_record = take_output(staged_state);
  bool const staged_ready = staged && staged->completion.wait_for(2s) == std::future_status::ready;
  auto staged_result = staged_ready ? std::optional<CallResult>(staged->completion.get()) : std::nullopt;
  close_input(staged_state);
  staged_thread.join();
  expect(randomized_id && !fulfilled_before_visible && !output_before_release && staged_record && staged_result && staged_result->has_value(),
         "a guessed response while the permission request is writer-claimed is staged until delivery acknowledgement and randomized ids are non-predictable");

  auto delivered_state = std::make_shared<MemoryTransportState>();
  delivered_state->block_writes = true;
  JsonRpcPeer delivered_peer(std::make_unique<MemoryTransport>(delivered_state),
                             [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::JoinThread delivered_thread = ava::core::JoinThread::create("delivered_thread", [&] { static_cast<void>(delivered_peer.run()); });
  wait_reader(delivered_state);
  auto delivered = delivered_peer.send_request("client/delivery-wins", std::string("{}"), 2s);
  wait_writer(delivered_state);
  {
    std::lock_guard lock(delivered_state->mutex);
    delivered_state->block_writes = false;
    delivered_state->cv.notify_all();
  }
  auto delivered_record = take_output(delivered_state);
  if (delivered)
  {
    auto response = encode_success(delivered->id, R"({"winner":"delivery"})");
    feed(delivered_state, *response);
  }
  bool const delivered_ready = delivered && delivered->completion.wait_for(2s) == std::future_status::ready;
  auto delivered_result = delivered_ready ? std::optional<CallResult>(delivered->completion.get()) : std::nullopt;
  bool canceled_before_close = false;
  {
    std::lock_guard lock(delivered_state->mutex);
    canceled_before_close = delivered_state->canceled;
  }
  close_input(delivered_state);
  delivered_thread.join();
  expect(delivered_record && delivered_result && delivered_result->has_value() && !canceled_before_close,
         "writer delivery acknowledgment and response win the mutex race with normal pending-call semantics");

  auto ambiguous_state = std::make_shared<MemoryTransportState>();
  ambiguous_state->block_writes = true;
  ambiguous_state->publish_before_stall = true;
  JsonRpcPeer ambiguous_peer(std::make_unique<MemoryTransport>(ambiguous_state),
                             [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult ambiguous_run;
  ava::core::JoinThread ambiguous_thread = ava::core::JoinThread::create("ambiguous_thread", [&] { ambiguous_run = ambiguous_peer.run(); });
  wait_reader(ambiguous_state);
  auto ambiguous = ambiguous_peer.send_request("client/ambiguous-race", std::string("{}"), 250ms);
  wait_writer(ambiguous_state);
  bool const ambiguous_ready = ambiguous && ambiguous->completion.wait_for(2s) == std::future_status::ready;
  auto ambiguous_result = ambiguous_ready ? std::optional<CallResult>(ambiguous->completion.get()) : std::nullopt;
  ambiguous_thread.join();
  auto physically_delivered = take_output(ambiguous_state);
  expect(ambiguous_result && !*ambiguous_result && ambiguous_result->error().code == -32603 &&
             ambiguous_result->error().message.find("outcome is unknown") != std::string::npos && ambiguous_run.has_value() && ambiguous_state->canceled &&
             physically_delivered && !take_output(ambiguous_state, 80ms),
         "a full physical delivery reported after cancellation yields one ambiguous-delivery winner and no resumed duplicate record");
}

void test_acp_prompt_admission_rollback_and_control_cancel_saturation()
{
  using namespace ava::app::acp;

  auto run_case = [](bool cancel_with_json_rpc) {
    auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-admission-order");
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    configure_acp_test_model(root);
    std::string body;
    AgentServiceOptions options;
    options.agent_version = "1";
    options.launch_root = ava::core::normalized_absolute_path(workspace);
    options.paths = ava::tests::app_test_paths(root);
    options.provider_bundle_factory = recording_bundle_factory(&body);
    AgentService service(options);
    static_cast<void>(service.handle_request(initialize_request(), {}));
    auto created = service.handle_request(
        Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
    auto session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
    expect(session_id.has_value(), "ACP admission-order test creates a session");
    if (!session_id)
      return;

    auto state = std::make_shared<MemoryTransportState>();
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    std::size_t blockers_entered = 0;
    bool release = false;
    std::atomic_int control_cancels = 0;
    JsonRpcPeer peer(
        std::make_unique<MemoryTransport>(state), [&service](Request const& request, std::stop_token token) { return service.handle_request(request, token); },
        [&gate_mutex, &gate_cv, &blockers_entered, &release](Notification const&, std::stop_token) {
          std::unique_lock lock(gate_mutex);
          ++blockers_entered;
          gate_cv.notify_all();
          gate_cv.wait(lock, [&release] { return release; });
        });
    peer.set_request_pre_admission_hook([&service](Request const& request) { return service.pre_admit_request(request); });
    peer.set_control_notification_handler([&service, &control_cancels](Notification const& notification) {
      control_cancels.fetch_add(1, std::memory_order_relaxed);
      service.handle_control_notification(notification);
    });
    service.bind_update_sender([&peer](std::string_view id, std::string_view update) -> ava::core::VoidResult {
      return peer.send_notification("session/update",
                                    std::string("{\"sessionId\":\"") + ava::core::json::escape(id) + "\",\"update\":" + std::string(update) + "}");
    });

    ava::core::VoidResult run_result;
    ava::core::JoinThread peer_thread = ava::core::JoinThread::create("peer_thread", [&] { run_result = peer.run(); });
    wait_reader(state);
    for (std::size_t index = 0; index < kWorkerCount; ++index) feed(state, R"({"jsonrpc":"2.0","method":"test/block","params":{}})");
    {
      std::unique_lock lock(gate_mutex);
      static_cast<void>(gate_cv.wait_for(lock, 2s, [&] { return blockers_entered == kWorkerCount; }));
    }

    auto prompt_record = std::string("{\"jsonrpc\":\"2.0\",\"id\":\"queued\",\"method\":\"session/prompt\",\"params\":{\"sessionId\":\"") + *session_id +
                         "\",\"prompt\":[{\"type\":\"text\",\"text\":\"queued\"}]}}";
    feed(state, prompt_record);
    std::this_thread::sleep_for(20ms);
    if (cancel_with_json_rpc)
    {
      feed(state, R"({"jsonrpc":"2.0","method":"$/cancel_request","params":{"requestId":"queued"}})");
      while (peer.stats().canceled_inbound_requests == 0) std::this_thread::sleep_for(1ms);
    }
    else
    {
      for (std::size_t index = 1; index < kMaxWorkerQueue; ++index) feed(state, R"({"jsonrpc":"2.0","method":"test/queued","params":{}})");
      for (int index = 0; index < 8; ++index) feed(state, R"({"jsonrpc":"2.0","method":"test/dropped","params":{}})");
      feed(state, std::string("{\"jsonrpc\":\"2.0\",\"method\":\"session/cancel\",\"params\":{\"sessionId\":\"") + *session_id + "\"}}");
      while (control_cancels.load(std::memory_order_acquire) == 0) std::this_thread::sleep_for(1ms);
    }
    {
      std::lock_guard lock(gate_mutex);
      release = true;
    }
    gate_cv.notify_all();

    auto canceled = take_output(state);
    bool const cancellation_ok =
        cancel_with_json_rpc ? output_has_code(canceled, -32800) : canceled && canceled->find("\"stopReason\":\"cancelled\"") != std::string::npos;
    expect(cancellation_ok, cancel_with_json_rpc ? "queued prompt $/cancel_request rolls back pre-admission before handler consumption"
                                                 : "reader-ordered session/cancel remains non-droppable under a saturated generic notification queue");
    if (!cancel_with_json_rpc)
      expect(peer.stats().dropped_notifications > 0 && control_cancels.load(std::memory_order_relaxed) == 1,
             "generic notifications saturate and drop while the bounded session/cancel control path executes exactly once");

    feed(state, std::string("{\"jsonrpc\":\"2.0\",\"id\":\"later\",\"method\":\"session/prompt\",\"params\":{\"sessionId\":\"") + *session_id +
                    "\",\"prompt\":[{\"type\":\"text\",\"text\":\"later succeeds\"}]}}");
    std::optional<std::string> later;
    for (int index = 0; index < 4 && !later; ++index)
    {
      auto record = take_output(state);
      if (record && record->find("\"id\":\"later\"") != std::string::npos)
        later = std::move(record);
    }
    expect(later && later->find("\"stopReason\":\"end_turn\"") != std::string::npos, "a later prompt succeeds after canceled queued admission rollback");

    feed(state, std::string("{\"jsonrpc\":\"2.0\",\"id\":\"close\",\"method\":\"session/close\",\"params\":{\"sessionId\":\"") + *session_id + "\"}}");
    auto closed = take_output(state);
    expect(closed && closed->find("\"id\":\"close\"") != std::string::npos && closed->find("\"result\":{}") != std::string::npos,
           "session close succeeds without a leaked prompt reservation");

    close_input(state);
    peer_thread.join();
    service.unbind_update_sender();
    service.shutdown();
    expect(run_result.has_value(), "admission and control cancellation test shuts down cleanly");
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
  };

  run_case(true);
  run_case(false);
}

void test_acp_peer_shutdown_abandons_queued_unstarted_request()
{
  using namespace ava::app::acp;

  auto state = std::make_shared<MemoryTransportState>();
  std::mutex blockers_mutex;
  std::condition_variable blockers_cv;
  std::size_t blockers_entered = 0;
  std::size_t blockers_stopped = 0;
  std::atomic_int target_executions = 0;
  std::atomic_int admissions = 0;
  std::atomic_int rollbacks = 0;
  std::atomic_int escalation_calls = 0;
  JsonRpcPeer peer(
      std::make_unique<MemoryTransport>(state),
      [&target_executions](Request const&, std::stop_token) -> RequestResult {
        target_executions.fetch_add(1, std::memory_order_relaxed);
        return std::string("{}");
      },
      [&blockers_mutex, &blockers_cv, &blockers_entered, &blockers_stopped](Notification const&, std::stop_token token) {
        std::unique_lock lock(blockers_mutex);
        ++blockers_entered;
        blockers_cv.notify_all();
        std::stop_callback notify_stop(token, [&blockers_cv] { blockers_cv.notify_all(); });
        blockers_cv.wait(lock, [&token] { return token.stop_requested(); });
        ++blockers_stopped;
        blockers_cv.notify_all();
      },
      {}, std::make_unique<RecordingShutdownEscalation>(escalation_calls));
  peer.set_request_pre_admission_hook([&admissions, &rollbacks, &blockers_cv](Request const&) -> std::expected<std::function<void()>, JsonRpcError> {
    admissions.fetch_add(1, std::memory_order_release);
    blockers_cv.notify_all();
    return std::function<void()>([&rollbacks] { rollbacks.fetch_add(1, std::memory_order_relaxed); });
  });

  ava::core::VoidResult run_result;
  ava::core::JoinThread peer_thread = ava::core::JoinThread::create("peer_thread", [&] { run_result = peer.run(); });
  wait_reader(state);
  for (std::size_t index = 0; index < kWorkerCount; ++index) feed(state, R"({"jsonrpc":"2.0","method":"test/block-worker","params":{}})");

  bool all_blockers_entered = false;
  {
    std::unique_lock lock(blockers_mutex);
    all_blockers_entered = blockers_cv.wait_for(lock, 2s, [&] { return blockers_entered == kWorkerCount; });
  }
  expect(all_blockers_entered, "ACP queued-abandonment test occupies every peer worker before admitting its target request");
  if (!all_blockers_entered)
  {
    close_input(state);
    peer_thread.join();
    return;
  }

  feed(state, R"({"jsonrpc":"2.0","id":1,"method":"test/queued-target","params":{}})");
  bool target_admitted = false;
  {
    std::unique_lock lock(blockers_mutex);
    target_admitted = blockers_cv.wait_for(lock, 2s, [&] { return admissions.load(std::memory_order_acquire) == 1; });
  }
  expect(target_admitted, "ACP queued-abandonment test observes target pre-admission before EOF");
  close_input(state);
  peer_thread.join();

  expect(target_admitted && target_executions.load(std::memory_order_relaxed) == 0 && rollbacks.load(std::memory_order_relaxed) == 1,
         "EOF abandons an admitted but unstarted request and rolls back its reservation exactly once");
  expect(blockers_stopped == kWorkerCount, "EOF stops every cooperative worker blocker");
  expect(run_result.has_value() && escalation_calls.load(std::memory_order_relaxed) == 0,
         "queued-unstarted abandonment exits the peer cleanly without shutdown escalation");
}

void test_acp_peer_started_non_cooperative_shutdown_escalates()
{
  using namespace ava::app::acp;

  int entry_pipe[2] = {-1, -1};
  bool const pipe_created = pipe(entry_pipe) == 0;
  expect(pipe_created, "ACP escalation regression creates a handler-entry signal pipe");
  if (!pipe_created)
    return;

  pid_t const child = fork();
  expect(child >= 0, "ACP escalation regression forks a child process");
  if (child == 0)
  {
    static_cast<void>(close(entry_pipe[0]));
    auto state = std::make_shared<MemoryTransportState>();
    JsonRpcPeer peer(
        std::make_unique<MemoryTransport>(state),
        [state, entry_fd = entry_pipe[1]](Request const&, std::stop_token) -> RequestResult {
          char const entered = '1';
          if (write(entry_fd, &entered, 1) != 1)
            std::_Exit(98);
          static_cast<void>(close(entry_fd));
          close_input(state);
          while (true) std::this_thread::sleep_for(10ms);
        },
        {}, {}, make_process_shutdown_escalation(), 100ms);
    ava::core::JoinThread request_feeder = ava::core::JoinThread::create("request_feeder", [state] {
      wait_reader(state);
      feed(state, R"({"jsonrpc":"2.0","id":1,"method":"never","params":{}})");
    });
    static_cast<void>(peer.run());
    std::_Exit(99);
  }

  static_cast<void>(close(entry_pipe[1]));
  if (child < 0)
  {
    static_cast<void>(close(entry_pipe[0]));
    return;
  }

  int status = 0;
  bool exited = false;
  auto const deadline = ava::tests::now_plus_seconds(5);
  while (std::chrono::steady_clock::now() < deadline)
  {
    auto const waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
    {
      exited = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  if (!exited)
  {
    static_cast<void>(kill(child, SIGKILL));
    static_cast<void>(waitpid(child, &status, 0));
  }

  char entry_signal = '\0';
  auto const signal_bytes = read(entry_pipe[0], &entry_signal, 1);
  static_cast<void>(close(entry_pipe[0]));
  expect(signal_bytes == 1 && entry_signal == '1', "the non-cooperative request handler starts before causing transport EOF");
  expect(exited && WIFEXITED(status) && WEXITSTATUS(status) == kShutdownEscalationExitCode,
         "a started non-cooperative request causes bounded normal shutdown escalation exit 70");
}

void test_acp_redacted_command_audit_omits_recipe_arguments()
{
  constexpr std::string_view secret = "acp-unique-secret-like-argument-7f3e";
  ava::tools::PermissionAuditEvent event;
  event.permission_request_id = "permreq_acp_redacted";
  event.operation = ava::permissions::Operation::RunCommand;
  event.mode = ava::agent::Mode::Build;
  event.tool_name = "bash";
  event.action = ava::permissions::PermissionAction::Ask;
  event.reason = "command requires approval";
  event.risk = ava::permissions::PermissionRisk::Critical;
  event.command = "curl --token " + std::string(secret);
  event.resolution_source = "policy";
  event.command_arguments_redacted = true;
  ava::permissions::CommandPermissionMetadata metadata;
  metadata.level = ava::command::CommandLevel::Sensitive;
  metadata.recipe_display = "sensitive-network: --header " + std::string(secret);
  metadata.global_recipe_key = "sha256:ava-command-recipe-v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  metadata.workspace_recipe_key = "sha256:ava-command-workspace-recipe-v1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  event.command_metadata = std::move(metadata);
  auto const json = ava::tools::permission_audit_data_json(event);

  expect(json.find(secret) == std::string::npos && ava::core::json::string_field(json, "command") == "[redacted]" &&
             json.find("\"recipe_display\"") == std::string::npos,
         "ACP-style redacted command audits retain a redacted command and never serialize argument-derived recipe display values");
}

void test_acp_peer_write_failure_wakes_reader_and_shutdown_race()
{
  using namespace ava::app::acp;
  auto state = std::make_shared<MemoryTransportState>();
  state->fail_writes = true;
  JsonRpcPeer peer(std::make_unique<MemoryTransport>(state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
  wait_reader(state);
  feed(state, R"({"jsonrpc":"2.0","id":1,"method":"x","params":{}})");
  thread.join();
  expect(!run_result, "ACP write failure wakes the reader and terminates the connection");

  auto blocked_state = std::make_shared<MemoryTransportState>();
  std::atomic_bool entered = false;
  JsonRpcPeer blocked(std::make_unique<MemoryTransport>(blocked_state), [&entered](Request const&, std::stop_token token) -> RequestResult {
    entered.store(true, std::memory_order_release);
    while (!token.stop_requested()) std::this_thread::sleep_for(1ms);
    return std::string("{}");
  });
  ava::core::VoidResult blocked_result;
  ava::core::JoinThread blocked_thread = ava::core::JoinThread::create("blocked_thread", [&] { blocked_result = blocked.run(); });
  wait_reader(blocked_state);
  feed(blocked_state, R"({"jsonrpc":"2.0","id":1,"method":"blocked","params":{}})");
  while (!entered.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
  blocked.shutdown();
  blocked.shutdown();
  blocked_thread.join();
  expect(blocked_result.has_value(), "ACP idempotent shutdown cancels and joins a cooperatively blocked request handler");

  auto notification_state = std::make_shared<MemoryTransportState>();
  std::atomic_bool notification_entered = false;
  std::atomic_bool notification_stopped = false;
  JsonRpcPeer notifications(
      std::make_unique<MemoryTransport>(notification_state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); },
      [&notification_entered, &notification_stopped](Notification const&, std::stop_token token) {
        notification_entered.store(true, std::memory_order_release);
        while (!token.stop_requested()) std::this_thread::sleep_for(1ms);
        notification_stopped.store(true, std::memory_order_release);
      });
  ava::core::JoinThread notification_thread = ava::core::JoinThread::create("notification_thread", [&] { static_cast<void>(notifications.run()); });
  wait_reader(notification_state);
  feed(notification_state, R"({"jsonrpc":"2.0","method":"blocked-notification","params":{}})");
  while (!notification_entered.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
  notifications.shutdown();
  notification_thread.join();
  expect(notification_stopped.load(std::memory_order_acquire), "ACP notification handlers receive connection shutdown cancellation");
}
