#include "sys.h"
#include "tests/acp_test_declarations.h"
#include "tests/support/acp_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/app/acp/client_tools.h"
#include "ava/app/acp/peer.h"
#include "ava/app/acp/service.h"
#include "ava/app/session_run_controller.h"
#include "ava/agent/mode.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/agent/tool_result.h"
#include "ava/agent/tool_summaries.h"
#include "ava/tools/bash_tool.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/secure_workspace.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/core/error.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <sys/stat.h>

using namespace std::chrono_literals;
using ava::app::acp::JsonRpcId;
using namespace acp_test;
namespace runtime = ava::app::runtime;

namespace {

ava::app::acp::PendingCall ready_call(std::string id, ava::app::acp::CallResult result)
{
  std::promise<ava::app::acp::CallResult> promise;
  promise.set_value(std::move(result));
  return ava::app::acp::PendingCall{.id = std::move(id), .completion = promise.get_future()};
}

}  // namespace

void test_acp_peer_prompt_terminal_commit_arbitration()
{
  using namespace ava::app::acp;

  auto run_case = [](ava::app::RunPhase barrier_phase, bool cancel_wins) {
    auto root_owner = ScopedTestDirectory::create_unique(temp_root(), cancel_wins ? "acp-peer-cancel-before-" : "acp-peer-cancel-late-");
    auto const root = root_owner.path();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    configure_acp_test_model(root);
    auto provider_state = std::make_shared<CapturingSequenceState>();
    auto barrier = std::make_shared<RunPhaseBarrier>();
    barrier->target = barrier_phase;

    AgentServiceOptions options;
    options.agent_version = "1";
    options.launch_root = ava::core::normalized_absolute_path(workspace);
    options.paths = ava::tests::app_test_paths(root);
    options.run_options.on_phase = [barrier](ava::app::RunPhase phase) { return barrier->observe(phase); };
    options.provider_bundle_factory = sequence_bundle_factory(provider_state, {acp_text_response("peer terminal success")});
    ScopedAgentService fixture(std::move(root_owner), options);
    auto& service = fixture.service;
    auto state = std::make_shared<MemoryTransportState>();
    JsonRpcPeer peer(
        std::make_unique<MemoryTransport>(state), [&service](Request const& request, std::stop_token token) { return service.handle_request(request, token); },
        [&service](Notification const& notification, std::stop_token token) { service.handle_notification(notification, token); });
    ReaderBarrier reader(peer, [&service](Notification const& notification) { service.handle_control_notification(notification); });
    peer.set_request_pre_admission_hook([&service](Request const& request) { return service.pre_admit_request(request); });
    service.bind_request_terminal_committer([&peer](JsonRpcId const& id) { return peer.commit_inbound_request(id); });
    service.bind_update_sender([&peer](std::string_view session_id, std::string_view update) -> ava::core::VoidResult {
      return peer.send_notification("session/update",
                                    std::string("{\"sessionId\":\"") + ava::core::json::escape(session_id) + "\",\"update\":" + std::string(update) + "}");
    });

    ava::core::VoidResult run_result;
    ava::core::JoinThread peer_thread = ava::core::JoinThread::create("peer_thread", [&] { run_result = peer.run(); });
    wait_reader(state);
    feed(state, R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":1}})");
    static_cast<void>(take_output(state));
    feed(state,
         std::string("{\"jsonrpc\":\"2.0\",\"id\":\"new\",\"method\":\"session/new\",\"params\":{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}}");
    auto created_record = take_output(state);
    auto created_result = created_record ? ava::core::json::object_field(*created_record, "result") : std::nullopt;
    auto session_id = created_result ? ava::core::json::string_field(*created_result, "sessionId") : std::nullopt;
    if (!ava::tests::require(session_id.has_value(), "ACP peer terminal arbitration creates a live session"))
    {
      close_input(state);
      peer_thread.join();
      return;
    }

    feed(state, std::string("{\"jsonrpc\":\"2.0\",\"id\":\"prompt\",\"method\":\"session/prompt\",\"params\":{\"sessionId\":\"") + *session_id +
                    "\",\"prompt\":[{\"type\":\"text\",\"text\":\"race\"}]}}");
    bool const reached = barrier->wait_until_reached();
    expect(reached, cancel_wins ? "ACP peer reaches a pre-terminal-commit boundary" : "ACP peer reaches the committed Completing boundary");
    feed(state, R"({"jsonrpc":"2.0","method":"$/cancel_request","params":{"requestId":"prompt"}})");
    auto const probe_deadline = ava::tests::now_plus_seconds(2);
    static_cast<void>(probe_deadline);
    expect(reader.synchronize(state), "reader processes terminal cancellation while the runtime phase remains held");
    barrier->release();

    std::optional<std::string> prompt_terminal;
    for (int index = 0; index < 4 && !prompt_terminal; ++index)
    {
      auto record = take_output(state);
      if (record && record->find(R"("id":"prompt")") != std::string::npos)
        prompt_terminal = std::move(record);
    }
    close_input(state);
    peer_thread.join();
    service.unbind_request_terminal_committer();
    service.unbind_update_sender();
    service.shutdown();

    std::size_t provider_requests = 0;
    {
      std::lock_guard lock(provider_state->mutex);
      provider_requests = provider_state->request_bodies.size();
    }
    auto store = ava::session::SessionStore::open(workspace, *session_id, options.paths.sessions_dir);
    auto entries = store ? store->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(store.error()));
    auto const assistant_count =
        entries ? std::count_if(entries->begin(), entries->end(), [](auto const& entry) { return entry.type == ava::session::EntryType::AssistantTurnCommit; })
                : 0;
    if (cancel_wins)
    {
      expect(output_has_code(prompt_terminal, -32800) && peer.stats().canceled_inbound_requests == 1 && provider_requests == 0 && assistant_count == 0,
             "$/cancel_request before terminal commit wins with one canceled response and no durable/provider completion");
    }
    else
    {
      expect(prompt_terminal && prompt_terminal->find(R"("stopReason":"end_turn")") != std::string::npos && peer.stats().canceled_inbound_requests == 0 &&
                 provider_requests == 1 && assistant_count == 1,
             "$/cancel_request after runtime Completing loses and preserves the one successful PromptResponse and durable assistant");
    }
    expect(run_result.has_value(), "ACP peer terminal arbitration shuts down cleanly");
  };

  run_case(ava::app::RunPhase::AwaitingProvider, true);
  run_case(ava::app::RunPhase::Completing, false);

  auto root_owner = ScopedTestDirectory::create_unique(temp_root(), "acp-terminal-refusal-outcome-");
  auto const root = root_owner.path();
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_test_model(root);
  auto provider_state = std::make_shared<CapturingSequenceState>();
  auto updates = std::make_shared<SessionUpdateGateway>();
  updates->bind([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  auto requests = std::make_shared<ClientRequestGateway>();
  AcpSessionOptions options;
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = ava::tests::app_test_paths(root);
  options.provider_bundle_factory = sequence_bundle_factory(provider_state, {acp_text_response("terminal refusal")});
  options.client_capabilities = std::make_shared<ClientCapabilities const>();
  options.updates = updates;
  options.client_requests = requests;
  AcpSessionRegistry registry(std::move(options));
  auto host = registry.create(ava::core::normalized_absolute_path(workspace), std::make_shared<ava::mcp::McpConfig const>());
  auto reservation = host ? (*host)->reserve_prompt() : ava::core::Result<std::uint64_t>(std::unexpected(host.error()));
  std::shared_ptr<ava::app::SessionRunController> controller;
  std::string run_id;
  if (reservation)
  {
    auto session_r = (*host)->session_r();
    controller = session_r->run_controller();
    run_id = controller ? controller->snapshot().run_id : std::string{};
  }
  auto refused = reservation ? (*host)->prompt(AcpPromptContent{.text = "refuse terminal commit", .images = {}}, {}, *reservation, [] { return false; })
                             : RequestResult(std::unexpected(JsonRpcError{}));
  auto outcome = controller && !run_id.empty()
                     ? controller->wait_outcome(run_id)
                     : ava::core::Result<ava::app::RunOutcome>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing ACP run outcome")));
  expect(host && reservation && !refused && outcome && outcome->reason == ava::app::StopReason::UserCanceled && outcome->error &&
             outcome->error->code() == ava::core::ErrorCode::Canceled && outcome->error->format().find("boundary: before_terminal_commit") != std::string::npos,
         "ACP terminal-commit refusal completes the admitted run as UserCanceled, not ProviderError");
  registry.shutdown();
}

void test_acp_client_tool_dtos_lifecycle_and_cancellation()
{
  using namespace ava::app::acp;

  auto gateway = std::make_shared<ClientRequestGateway>();
  std::vector<std::string> methods;
  std::vector<std::string> params;
  std::vector<OutboundCallPolicy> policies;
  gateway->bind(
      [&](std::string method, std::optional<std::string> value, std::chrono::milliseconds, OutboundCallPolicy policy) -> ava::core::Result<PendingCall> {
        methods.push_back(method);
        params.push_back(value.value_or(""));
        policies.push_back(policy);
        if (method == "fs/read_text_file")
          return ready_call("read", CallResult(std::string(R"({"content":"remote text","_meta":[]})")));
        if (method == "fs/write_text_file")
          return ready_call("write", CallResult(std::string(R"({"_meta":7})")));
        if (method == "terminal/create")
          return ready_call("create", CallResult(std::string(R"({"terminalId":"terminal-1","_meta":[]})")));
        if (method == "terminal/wait_for_exit")
          return ready_call("wait", CallResult(std::string(R"({"exitCode":0,"signal":null,"_meta":"ignored"})")));
        if (method == "terminal/output")
          return ready_call(
              "output",
              CallResult(std::string(R"({"output":"one\ntwo\n","truncated":false,"exitStatus":{"exitCode":0,"signal":null,"_meta":7},"_meta":false})")));
        return ready_call("release", CallResult(std::string("{}")));
      },
      [](JsonRpcId const&, std::string) { return true; });

  auto files = make_client_exact_file_access("session-a", gateway);
  auto read = files->read_text_file(std::filesystem::path("/workspace/note.txt"), nullptr);
  auto written = files->write_text_file(std::filesystem::path("/workspace/note.txt"), "updated", nullptr);
  auto windowed = files->read_text_file_window(std::filesystem::path("/workspace/note.txt"), {.line = 9, .limit = 21}, nullptr);
  ava::tests::expect_all(
      "ACP exact-file adapter emits exact full/windowed DTOs and field-local optional defaults",
      {{"full read content", read && *read == "remote text"},
       {"write accepted", written.has_value()},
       {"window read content", windowed && *windowed == "remote text"},
       {"method order", methods == std::vector<std::string>({"fs/read_text_file", "fs/write_text_file", "fs/read_text_file"})},
       {"full-read session id", params.size() == 3 && ava::core::json::string_field(params[0], "sessionId") == "session-a"},
       {"full-read path", params.size() == 3 && ava::core::json::string_field(params[0], "path") == "/workspace/note.txt"},
       {"full-read omits line", params.size() == 3 && params[0].find(R"("line")") == std::string::npos},
       {"write content", params.size() == 3 && ava::core::json::string_field(params[1], "content") == "updated"},
       {"window line", params.size() == 3 && ava::core::json::integer_field(params[2], "line") == 9},
       {"window limit", params.size() == 3 && ava::core::json::integer_field(params[2], "limit") == 21},
       {"delivery policies", policies == std::vector<OutboundCallPolicy>(
                                             {OutboundCallPolicy::Normal, OutboundCallPolicy::AbortConnectionIfDelivered, OutboundCallPolicy::Normal})}});

  std::string const invalid_utf8(1, static_cast<char>(0xff));
  auto const calls_before_invalid_utf8 = methods.size();
  auto invalid_content = files->write_text_file(std::filesystem::path("/workspace/note.txt"), invalid_utf8, nullptr);
  auto invalid_path = files->read_text_file(std::filesystem::path("/workspace/" + invalid_utf8), nullptr);
  auto invalid_session_files = make_client_exact_file_access(invalid_utf8, gateway);
  auto invalid_session = invalid_session_files->read_text_file(std::filesystem::path("/workspace/note.txt"), nullptr);
  expect(!invalid_content && !invalid_path && !invalid_session && methods.size() == calls_before_invalid_utf8,
         "ACP exact-file serialization rejects invalid UTF-8 without throwing, changing bytes, or sending a client request");

  auto delegated_cancel_root_owner = ScopedTestDirectory::create_unique(temp_root(), "acp-delegated-read-cancel-");
  auto const delegated_cancel_root = delegated_cancel_root_owner.path();
  auto const delegated_cancel_workspace = delegated_cancel_root / "workspace";
  std::filesystem::create_directories(delegated_cancel_workspace);
  ava::tests::write_test_file(delegated_cancel_workspace / "note.txt", "local placeholder");
  auto delegated_cancel_secure = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(delegated_cancel_workspace));
  auto delegated_cancel_gateway = std::make_shared<ClientRequestGateway>();
  auto delegated_cancel_promise = std::make_shared<std::promise<CallResult>>();
  bool cancellation_observable = false;
  std::string canceled_method;
  delegated_cancel_gateway->bind(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        canceled_method = method;
        cancellation_observable = true;
        return PendingCall{.id = std::string("delegated-read"), .completion = delegated_cancel_promise->get_future()};
      },
      [](JsonRpcId const&, std::string) { return true; });
  ava::tools::ToolContext delegated_cancel_context{.workspace_dir = ava::core::normalized_absolute_path(delegated_cancel_workspace),
                                                   .mode = ava::agent::Mode::Build,
                                                   .cancel_requested = [&] { return cancellation_observable; },
                                                   .secure_workspace = delegated_cancel_secure ? *delegated_cancel_secure : nullptr,
                                                   .exact_file_access = make_client_exact_file_access("session-delegated-cancel", delegated_cancel_gateway)};
  auto delegated_cancel_dispatcher_result = ava::agent::ToolDispatcher::create_strict(delegated_cancel_context);
  if (!ava::tests::require(delegated_cancel_dispatcher_result.has_value(),
                           delegated_cancel_dispatcher_result ? "dispatcher fixture composes" : delegated_cancel_dispatcher_result.error().format()))
    return;
  auto& delegated_cancel_dispatcher = *delegated_cancel_dispatcher_result;
  auto delegated_cancel = delegated_cancel_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_delegated_cancel", .name = "read_file", .arguments_json = R"({"path":"note.txt"})"});
  expect(delegated_cancel_secure && delegated_cancel && !delegated_cancel->success &&
             delegated_cancel->payload.status == ava::agent::ToolResultStatus::Canceled && canceled_method == "fs/read_text_file" &&
             delegated_cancel->result_text.find("ACP client tool request canceled") != std::string::npos &&
             delegated_cancel->result_text.find("method: fs/read_text_file") != std::string::npos,
         "delegated ACP fs/read_text_file cancellation reaches tool dispatch as Canceled with unchanged wire diagnostics");

  auto ambiguous_write_gateway = std::make_shared<ClientRequestGateway>();
  auto ambiguous_write_promise = std::make_shared<std::promise<CallResult>>();
  ambiguous_write_gateway->bind(
      [ambiguous_write_promise](std::string method, std::optional<std::string>, std::chrono::milliseconds,
                                OutboundCallPolicy policy) -> ava::core::Result<PendingCall> {
        expect(method == "fs/write_text_file" && policy == OutboundCallPolicy::AbortConnectionIfDelivered,
               "cancelable ACP file writes retain fail-stop policy at the gateway");
        return PendingCall{.id = std::string("ambiguous-write"), .completion = ambiguous_write_promise->get_future()};
      },
      [ambiguous_write_promise](JsonRpcId const&, std::string) {
        ambiguous_write_promise->set_value(std::unexpected(JsonRpcError{.code = -32603,
                                                                        .message = "ACP outbound request was delivered, but the response outcome is unknown",
                                                                        .data_json = std::nullopt,
                                                                        .id = std::nullopt,
                                                                        .intent = EnvelopeIntent::Response,
                                                                        .suppress_response = true}));
        return true;
      });
  auto ambiguous_write_files = make_client_exact_file_access("session-ambiguous-write", ambiguous_write_gateway);
  int ambiguous_write_cancel_checks = 0;
  auto ambiguous_write = ambiguous_write_files->write_text_file("/workspace/file.txt", "changed", [&] { return ++ambiguous_write_cancel_checks > 1; });
  expect(!ambiguous_write && ambiguous_write.error().format().find("outcome is unknown") != std::string::npos &&
             ambiguous_write.error().format().find("canceled: true") == std::string::npos,
         "ACP file-write cancellation boundedly surfaces the peer's ambiguous-delivery completion");

  auto ambiguous_create_gateway = std::make_shared<ClientRequestGateway>();
  auto ambiguous_create_promise = std::make_shared<std::promise<CallResult>>();
  ambiguous_create_gateway->bind(
      [ambiguous_create_promise](std::string method, std::optional<std::string>, std::chrono::milliseconds,
                                 OutboundCallPolicy policy) -> ava::core::Result<PendingCall> {
        expect(method == "terminal/create" && policy == OutboundCallPolicy::AbortConnectionIfDelivered,
               "terminal/create uses fail-stop delivery before acquiring its cleanup identity");
        return PendingCall{.id = std::string("ambiguous-create"), .completion = ambiguous_create_promise->get_future()};
      },
      [ambiguous_create_promise](JsonRpcId const&, std::string) {
        ambiguous_create_promise->set_value(std::unexpected(JsonRpcError{.code = -32603,
                                                                         .message = "ACP outbound request was delivered, but the response outcome is unknown",
                                                                         .data_json = std::nullopt,
                                                                         .id = std::nullopt,
                                                                         .intent = EnvelopeIntent::Response,
                                                                         .suppress_response = true}));
        return true;
      });
  auto ambiguous_create_commands = make_client_command_executor("session-ambiguous-create", ambiguous_create_gateway);
  int ambiguous_create_cancel_checks = 0;
  auto ambiguous_create = ambiguous_create_commands->execute(
      ava::tools::CommandExecutionRequest{.argv = {"sleep", "30"}, .cwd = "/workspace", .timeout = 1s, .output_byte_limit = 1024, .cancel_requested = [&] {
                                            return ++ambiguous_create_cancel_checks > 1;
                                          }});
  expect(!ambiguous_create && ambiguous_create.error().format().find("outcome is unknown") != std::string::npos,
         "ACP terminal/create cancellation surfaces delivered ambiguity through the fail-stop request policy");

  methods.clear();
  params.clear();
  policies.clear();
  auto commands = make_client_command_executor("session-a", gateway);
  auto executed = commands->execute(
      ava::tools::CommandExecutionRequest{.argv = {"printf", "hello world"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 4096});
  expect(executed && executed->exit_code == 0 && executed->output == "one\ntwo\n" &&
             methods == std::vector<std::string>({"terminal/create", "terminal/wait_for_exit", "terminal/output", "terminal/release"}) &&
             params[0].find(R"("command":"printf")") != std::string::npos && params[0].find(R"("args":["hello world"])") != std::string::npos &&
             params[0].find(R"("env":[])") != std::string::npos && params[0].find(R"("cwd":"/workspace")") != std::string::npos &&
             std::all_of(std::next(params.begin()), params.end(), [](std::string const& value) { return value.find("terminal-1") != std::string::npos; }) &&
             policies == std::vector<OutboundCallPolicy>({OutboundCallPolicy::AbortConnectionIfDelivered, OutboundCallPolicy::Normal,
                                                          OutboundCallPolicy::Normal, OutboundCallPolicy::Normal}),
         "ACP command adapter fail-stops ambiguous create ownership and otherwise uses exact create-wait-output-release ordering");

  auto const truncated_tail_fixture = read_acp_test_file(std::filesystem::path(AVA_ACP_V1_FIXTURE_DIR) / "terminal-output-truncated-tail.json");
  auto truncated_tail_gateway = std::make_shared<ClientRequestGateway>();
  truncated_tail_gateway->bind(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "terminal/create")
          return ready_call("truncated-create", CallResult(std::string(R"({"terminalId":"terminal-truncated"})")));
        if (method == "terminal/wait_for_exit")
          return ready_call("truncated-wait", CallResult(std::string(R"({"exitCode":0,"signal":null})")));
        if (method == "terminal/output")
          return ready_call("truncated-output", CallResult(truncated_tail_fixture));
        return ready_call("truncated-release", CallResult(std::string("{}")));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto truncated_tail_commands = make_client_command_executor("session-truncated-tail", truncated_tail_gateway);
  auto truncated_root_owner = ScopedTestDirectory::create_unique(temp_root(), "acp-terminal-truncated-tail-");
  auto const truncated_root = truncated_root_owner.path();
  auto const truncated_workspace = truncated_root / "workspace";
  std::filesystem::create_directories(truncated_workspace);
  expect(::chmod(truncated_root.c_str(), S_IRWXU) == 0 && ::chmod(truncated_workspace.c_str(), S_IRWXU) == 0,
         "ACP terminal truncated fixture workspace is owner-only for sealed command planning");
  std::vector<ava::tools::ToolProgressEvent> truncated_progress;
  auto const truncated_spill = truncated_root / "spill";
  ava::tools::ToolContext truncated_context{
      .workspace_dir = truncated_workspace,
      .spill_dir = truncated_spill,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .progress_sink = [&truncated_progress](ava::tools::ToolProgressEvent const& event) -> ava::core::VoidResult {
        truncated_progress.push_back(event);
        return {};
      },
      .anchor_set = command_anchors_for_test(truncated_workspace, truncated_spill),
      .command_executor = truncated_tail_commands};
  auto truncated_bash =
      ava::tools::run_bash(truncated_context, "printf retained-tail", ava::tools::BashOptions{.timeout = 100ms, .max_bytes = 4096, .max_lines = 200});
  auto const retained_tail = std::string("retained-tail-two\nretained-tail-three\n");
  expect(truncated_bash && truncated_bash->exit_code == 0 && !truncated_bash->timed_out && !truncated_bash->canceled && truncated_bash->truncated &&
             truncated_bash->byte_limited && !truncated_bash->line_limited && !truncated_bash->totals_known && truncated_bash->output == retained_tail &&
             truncated_bash->output_bytes == retained_tail.size() && truncated_bash->output_lines == 2 && truncated_bash->total_bytes == 0 &&
             truncated_bash->total_lines == 0 && truncated_bash->omitted_lines == 0 &&
             std::ranges::any_of(truncated_progress,
                                 [](ava::tools::ToolProgressEvent const& event) {
                                   return event.text.find("retained output bytes; original total unknown") != std::string::npos;
                                 }),
         "ACP terminal/output truncated-tail fixture preserves retained counts and status while clearing unavailable original totals");

  auto truncated_dispatcher_result = ava::agent::ToolDispatcher::create_strict(truncated_context);
  if (!ava::tests::require(truncated_dispatcher_result.has_value(),
                           truncated_dispatcher_result ? "dispatcher fixture composes" : truncated_dispatcher_result.error().format()))
    return;
  auto& truncated_dispatcher = *truncated_dispatcher_result;
  auto truncated_dispatch = truncated_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_truncated_terminal", .name = "bash", .arguments_json = R"({"command":"printf retained-tail","timeout_ms":100})"});
  auto const truncated_structured = truncated_dispatch ? ava::agent::serialize_tool_result_payload_json(*truncated_dispatch) : std::string{};
  auto const truncated_summary = truncated_dispatch ? ava::agent::summarize_tool_result(*truncated_dispatch) : std::string{};
  expect(truncated_dispatch && truncated_dispatch->success && ava::core::json::string_field(truncated_dispatch->result_text, "output") == retained_tail &&
             ava::core::json::integer_field(truncated_dispatch->result_text, "output_bytes") == static_cast<long long>(retained_tail.size()) &&
             ava::core::json::integer_field(truncated_dispatch->result_text, "output_lines") == 2 &&
             !ava::core::json::integer_field(truncated_dispatch->result_text, "total_bytes") &&
             !ava::core::json::integer_field(truncated_dispatch->result_text, "total_lines") &&
             !ava::core::json::integer_field(truncated_dispatch->result_text, "omitted_lines") &&
             truncated_summary.find("retained lines; original total unknown") != std::string::npos &&
             truncated_structured.find("\"total_bytes\"") == std::string::npos && truncated_structured.find("\"total_lines\"") == std::string::npos &&
             truncated_structured.find("\"omitted_lines\"") == std::string::npos,
         std::string(
             "ACP truncated terminal tool dispatch omits unknown totals from provider and structured serialization without losing retained counts: result=") +
             (truncated_dispatch ? truncated_dispatch->result_text : truncated_dispatch.error().format()) + " structured=" + truncated_structured +
             " summary=" + truncated_summary);

  int response_cancel_checks = 0;
  auto response_wins = commands->execute(ava::tools::CommandExecutionRequest{
      .argv = {"true"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 1024, .cancel_requested = [&response_cancel_checks] {
        return ++response_cancel_checks > 1;
      }});
  ava::tests::expect_all("a ready terminal response wins before cancellation retires its pending call",
                         {{"terminal response", response_wins && response_wins->exit_code == 0},
                          {"not canceled", response_wins && !response_wins->canceled},
                          {"cancellation observed", response_cancel_checks >= 1}});

  auto cancel_gateway = std::make_shared<ClientRequestGateway>();
  std::vector<std::string> cancellation_order;
  std::vector<std::shared_ptr<std::promise<CallResult>>> held;
  cancel_gateway->bind(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        cancellation_order.push_back(method);
        if (method == "terminal/wait_for_exit")
        {
          auto promise = std::make_shared<std::promise<CallResult>>();
          auto future = promise->get_future();
          held.push_back(std::move(promise));
          return PendingCall{.id = std::string("active-wait"), .completion = std::move(future)};
        }
        if (method == "terminal/create")
          return ready_call("create-cancel", CallResult(std::string(R"({"terminalId":"terminal-cancel"})")));
        if (method == "terminal/output")
          return ready_call("output-cancel", CallResult(std::string(R"({"output":"final","truncated":false})")));
        return ready_call(method, CallResult(std::string("{}")));
      },
      [&](JsonRpcId const& id, std::string) {
        expect(std::get<std::string>(id) == "active-wait", "ACP terminal cancellation targets the active wait request ID");
        cancellation_order.push_back("$/cancel_request");
        return true;
      });
  auto cancel_commands = make_client_command_executor("session-cancel", cancel_gateway);
  int cancel_checks = 0;
  auto canceled = cancel_commands->execute(ava::tools::CommandExecutionRequest{
      .argv = {"sleep", "30"}, .cwd = "/workspace", .timeout = 1s, .output_byte_limit = 1024, .cancel_requested = [&cancel_checks] {
        return ++cancel_checks >= 2;
      }});
  expect(canceled && canceled->canceled && canceled->exit_code == -1 &&
             cancellation_order == std::vector<std::string>({"terminal/create", "terminal/wait_for_exit", "$/cancel_request", "terminal/kill",
                                                             "terminal/output", "terminal/release"}),
         "ACP terminal cancellation sends $/cancel_request before kill, then fetches output and releases exactly once");

  auto release_gateway = std::make_shared<ClientRequestGateway>();
  release_gateway->bind(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "terminal/create")
          return ready_call("create-release", CallResult(std::string(R"({"terminalId":"terminal-release"})")));
        if (method == "terminal/wait_for_exit")
          return ready_call("wait-release", CallResult(std::string(R"({"exitCode":0,"signal":null})")));
        if (method == "terminal/output")
          return ready_call("output-release", CallResult(std::string(R"({"output":"ok","truncated":false})")));
        return ready_call("release-error", std::unexpected(JsonRpcError{.code = -32603,
                                                                        .message = "release failed",
                                                                        .data_json = std::nullopt,
                                                                        .id = std::nullopt,
                                                                        .intent = EnvelopeIntent::Response,
                                                                        .suppress_response = true}));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto release_commands = make_client_command_executor("session-release", release_gateway);
  auto unconfirmed_release =
      release_commands->execute(ava::tools::CommandExecutionRequest{.argv = {"true"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 1024});
  expect(!unconfirmed_release && unconfirmed_release.error().format().find("release failed") != std::string::npos &&
             unconfirmed_release.error().format().find("terminal_phase: release") != std::string::npos,
         "ACP terminal execution never reports clean success when release is unconfirmed");
}

void test_acp_client_file_dtos_validation_and_windows()
{
  using namespace ava::app::acp;

  auto malformed_gateway = std::make_shared<ClientRequestGateway>();
  malformed_gateway->bind(
      [&](std::string, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        return ready_call("malformed", CallResult(std::string(R"({"content":7})")));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto malformed_files = make_client_exact_file_access("session-malformed", malformed_gateway);
  auto malformed = malformed_files->read_text_file("/workspace/file.txt", nullptr);
  expect(!malformed && malformed.error().message().find("requires string content") != std::string::npos,
         "ACP client file adapter rejects malformed required response fields while allowing additive members");

  for (auto const& raw : {
           std::string(R"({"content":"first","content":"second"})"),
           std::string(kMaxRecordBytes, ' ') + R"({"content":"x"})",
           std::string(R"({"content":"x","_meta":)") + std::string(kMaxNestingDepth, '[') + "0" + std::string(kMaxNestingDepth, ']') + "}",
       })
  {
    auto bounded_gateway = std::make_shared<ClientRequestGateway>();
    bounded_gateway->bind([&raw](std::string, std::optional<std::string>, std::chrono::milliseconds,
                                 OutboundCallPolicy) -> ava::core::Result<PendingCall> { return ready_call("bounded", CallResult(raw)); },
                          [](JsonRpcId const&, std::string) { return true; });
    auto bounded_files = make_client_exact_file_access("session-bounded", bounded_gateway);
    expect(!bounded_files->read_text_file("/workspace/file.txt", nullptr), "direct client-tool response decoding enforces ACP bytes, depth and duplicate keys");
  }

  auto oversized_gateway = std::make_shared<ClientRequestGateway>();
  oversized_gateway->bind(
      [&](std::string, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        return ready_call("oversized", CallResult(std::string("{\"content\":\"") + std::string(kMaxStringBytes + 1, 'x') + "\"}"));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto oversized_files = make_client_exact_file_access("session-oversized", oversized_gateway);
  auto oversized = oversized_files->read_text_file("/workspace/file.txt", nullptr);
  expect(!oversized && oversized.error().message().find("bounded valid JSON") != std::string::npos,
         "ACP client file adapter rejects oversized content even when a test gateway bypasses peer record validation");

  std::string large_client_file;
  for (std::size_t line = 1; line <= 4'000; ++line) large_client_file += "line-" + std::to_string(line) + '-' + std::string(80, 'x') + '\n';
  auto window_root_owner = ScopedTestDirectory::create_unique(temp_root(), "acp-windowed-read-");
  auto const window_root = window_root_owner.path();
  auto const window_workspace = window_root / "workspace";
  std::filesystem::create_directories(window_workspace);
  auto window_secure = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(window_workspace));
  auto window_gateway = std::make_shared<ClientRequestGateway>();
  std::vector<std::pair<std::size_t, std::size_t>> observed_windows;
  window_gateway->bind(
      [&](std::string method, std::optional<std::string> value, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        expect(method == "fs/read_text_file" && value.has_value(), "bounded tool reads use fs/read_text_file");
        auto const observed_line = static_cast<std::size_t>(ava::core::json::integer_field(value.value_or(""), "line").value_or(0));
        auto const observed_limit = static_cast<std::size_t>(ava::core::json::integer_field(value.value_or(""), "limit").value_or(0));
        observed_windows.emplace_back(observed_line, observed_limit);
        std::size_t begin = 0;
        for (std::size_t current = 1; current < observed_line && begin < large_client_file.size(); ++current)
        {
          auto const newline = large_client_file.find('\n', begin);
          begin = newline == std::string::npos ? large_client_file.size() : newline + 1;
        }
        auto end = begin;
        for (std::size_t count = 0; count < observed_limit && end < large_client_file.size(); ++count)
        {
          auto const newline = large_client_file.find('\n', end);
          end = newline == std::string::npos ? large_client_file.size() : newline + 1;
        }
        auto result = std::string("{\"content\":\"") + ava::core::json::escape(large_client_file.substr(begin, end - begin)) + "\"}";
        return ready_call("window", CallResult(std::move(result)));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto window_files = make_client_exact_file_access("session-window", window_gateway, true, false);
  ava::tools::ToolContext window_context{.workspace_dir = ava::core::normalized_absolute_path(window_workspace),
                                         .mode = ava::agent::Mode::Build,
                                         .secure_workspace = window_secure ? *window_secure : nullptr,
                                         .exact_file_access = window_files};
  auto bounded_window = ava::tools::read_file(window_context, window_workspace / "large.txt", {.max_bytes = 64 * 1024, .offset_line = 300, .max_lines = 200});
  auto window_dispatcher_result = ava::agent::ToolDispatcher::create_strict(window_context);
  if (!ava::tests::require(window_dispatcher_result.has_value(),
                           window_dispatcher_result ? "dispatcher fixture composes" : window_dispatcher_result.error().format()))
    return;
  auto& window_dispatcher = *window_dispatcher_result;
  auto dispatched_window = window_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_window", .name = "read_file", .arguments_json = R"({"path":"large.txt","offset":300,"limit":200,"max_bytes":65536})"});
  auto beyond_eof = ava::tools::read_file(window_context, window_workspace / "large.txt", {.max_bytes = 64 * 1024, .offset_line = 5'000, .max_lines = 200});
  auto dispatched_beyond = window_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_beyond_eof", .name = "read_file", .arguments_json = R"({"path":"large.txt","offset":5000,"limit":200,"max_bytes":65536})"});
  auto overflow_window = window_files->read_text_file_window(
      window_workspace / "large.txt", {.line = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1, .limit = 1}, nullptr);
  auto const dispatcher_window_totals_absent = dispatched_window && !ava::core::json::integer_field(dispatched_window->result_text, "total_bytes") &&
                                               !ava::core::json::integer_field(dispatched_window->result_text, "total_lines") &&
                                               ava::core::json::integer_field(dispatched_window->result_text, "output_lines") == 200 &&
                                               ava::core::json::integer_field(dispatched_window->result_text, "next_offset_line") == 500;
  auto const dispatcher_beyond_totals_absent = dispatched_beyond && !ava::core::json::integer_field(dispatched_beyond->result_text, "total_bytes") &&
                                               !ava::core::json::integer_field(dispatched_beyond->result_text, "total_lines") &&
                                               ava::core::json::integer_field(dispatched_beyond->result_text, "output_bytes") == 0 &&
                                               ava::core::json::integer_field(dispatched_beyond->result_text, "output_lines") == 0 &&
                                               ava::core::json::integer_field(dispatched_beyond->result_text, "start_line") == 5'000 &&
                                               ava::core::json::integer_field(dispatched_beyond->result_text, "end_line") == 0;
  ava::tests::expect_all("ACP bounded read_file leaves full-file totals unknown and uses a sentinel for continuation",
                         {{"fixture exceeds whole-string limit", large_client_file.size() > kMaxStringBytes},
                          {"secure workspace", window_secure.has_value()},
                          {"window returned", bounded_window.has_value()},
                          {"window totals unknown", bounded_window && !bounded_window->totals_known},
                          {"no fabricated total bytes", bounded_window && bounded_window->total_bytes == 0},
                          {"no fabricated total lines", bounded_window && bounded_window->total_lines == 0},
                          {"requested start content", bounded_window && bounded_window->content.starts_with("line-300-")},
                          {"requested line count", bounded_window && bounded_window->output_lines == 200},
                          {"continuation limit", bounded_window && bounded_window->line_limited},
                          {"next offset", bounded_window && bounded_window->next_offset_line == 500},
                          {"dispatcher omits unknown totals", dispatcher_window_totals_absent}});
  ava::tests::expect_all("ACP beyond-EOF read_file returns empty content without fabricating totals or continuation",
                         {{"beyond-EOF read returned", beyond_eof.has_value()},
                          {"totals unknown", beyond_eof && !beyond_eof->totals_known},
                          {"content empty", beyond_eof && beyond_eof->content.empty()},
                          {"zero output bytes", beyond_eof && beyond_eof->output_bytes == 0},
                          {"zero output lines", beyond_eof && beyond_eof->output_lines == 0},
                          {"requested start line", beyond_eof && beyond_eof->start_line == 5'000},
                          {"no end line", beyond_eof && beyond_eof->end_line == 0},
                          {"no total bytes", beyond_eof && beyond_eof->total_bytes == 0},
                          {"no total lines", beyond_eof && beyond_eof->total_lines == 0},
                          {"not line limited", beyond_eof && !beyond_eof->line_limited},
                          {"no next offset", beyond_eof && beyond_eof->next_offset_line == 0},
                          {"dispatcher omits unknown totals", dispatcher_beyond_totals_absent}});
  ava::tests::expect_all(
      "ACP windowed reads request exactly one sentinel line and reject integer overflow",
      {{"wire windows", observed_windows == std::vector<std::pair<std::size_t, std::size_t>>({{300, 201}, {300, 201}, {5'000, 201}, {5'000, 201}})},
       {"overflow rejected", !overflow_window}});

  auto full_read_root_owner = ScopedTestDirectory::create_unique(temp_root(), "acp-full-edit-read-");
  auto const full_read_root = full_read_root_owner.path();
  auto const full_read_workspace = full_read_root / "workspace";
  std::filesystem::create_directories(full_read_workspace);
  auto full_read_secure = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(full_read_workspace));
  auto full_read_gateway = std::make_shared<ClientRequestGateway>();
  std::string full_read_content = "alpha old stale\n";
  std::vector<std::string> full_read_params;
  full_read_gateway->bind(
      [&](std::string method, std::optional<std::string> value, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "fs/read_text_file")
        {
          full_read_params.push_back(value.value_or(""));
          return ready_call("full-read", CallResult(std::string("{\"content\":\"") + ava::core::json::escape(full_read_content) + "\"}"));
        }
        if (method == "fs/write_text_file")
        {
          full_read_content = ava::core::json::string_field(value.value_or(""), "content").value_or("");
          return ready_call("full-write", CallResult(std::string("{}")));
        }
        return std::unexpected(ava::app::acp::protocol_error("unexpected full-read test method"));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto full_read_files = make_client_exact_file_access("session-full-read", full_read_gateway, true, true);
  ava::tools::ToolContext full_read_context{.workspace_dir = ava::core::normalized_absolute_path(full_read_workspace),
                                            .mode = ava::agent::Mode::Build,
                                            .secure_workspace = full_read_secure ? *full_read_secure : nullptr,
                                            .exact_file_access = full_read_files};
  auto full_edit = ava::tools::edit_file(full_read_context, full_read_workspace / "remote.txt", "old", "new");
  auto full_patch_dispatcher_result = ava::agent::ToolDispatcher::create_strict(full_read_context);
  if (!ava::tests::require(full_patch_dispatcher_result.has_value(),
                           full_patch_dispatcher_result ? "dispatcher fixture composes" : full_patch_dispatcher_result.error().format()))
    return;
  auto& full_patch_dispatcher = *full_patch_dispatcher_result;
  auto full_patch = full_patch_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "full-patch", .name = "apply_patch", .arguments_json = R"({"edits":[{"path":"remote.txt","old_text":"stale","new_text":"fresh"}]})"});
  expect(full_read_secure && full_edit && full_patch && full_patch->success && full_read_content == "alpha new fresh\n" && full_read_params.size() == 2 &&
             std::ranges::all_of(
                 full_read_params,
                 [](std::string const& value) { return !ava::core::json::integer_field(value, "line") && !ava::core::json::integer_field(value, "limit"); }),
         "ACP edit_file and apply_patch use full unwindowed fs/read_text_file DTOs");
}

void test_acp_client_terminal_status_defaults_and_cleanup()
{
  using namespace ava::app::acp;

  auto range_gateway = std::make_shared<ClientRequestGateway>();
  int release_calls = 0;
  range_gateway->bind(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "terminal/create")
          return ready_call("range-create", CallResult(std::string(R"({"terminalId":"terminal-range"})")));
        if (method == "terminal/wait_for_exit")
          return ready_call("range-wait", CallResult(std::string(R"({"exitCode":4294967295,"signal":null})")));
        if (method == "terminal/output")
          return ready_call("range-output", CallResult(std::string(R"({"output":"","truncated":false})")));
        if (method == "terminal/release")
          ++release_calls;
        return ready_call(method, CallResult(std::string("{}")));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto range_commands = make_client_command_executor("session-range", range_gateway);
  auto range_result =
      range_commands->execute(ava::tools::CommandExecutionRequest{.argv = {"true"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 1024});
  expect(range_result && range_result->exit_code == 4'294'967'295LL && release_calls == 1,
         "ACP terminal DTO and lifecycle accept UINT32_MAX and release the acquired terminal exactly once");

  auto run_defaulted_status_case = [&](std::string_view name, std::string wait_json, std::string output_json, std::int64_t expected_exit,
                                       std::string_view expected_error = {}) {
    auto status_gateway = std::make_shared<ClientRequestGateway>();
    int status_releases = 0;
    status_gateway->bind(
        [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
          if (method == "terminal/create")
            return ready_call(std::string(name) + "-create", CallResult(std::string(R"({"terminalId":"terminal-defaulted","_meta":[]})")));
          if (method == "terminal/wait_for_exit")
            return ready_call(std::string(name) + "-wait", CallResult(wait_json));
          if (method == "terminal/output")
            return ready_call(std::string(name) + "-output", CallResult(output_json));
          if (method == "terminal/release")
            ++status_releases;
          return ready_call(std::string(name) + "-cleanup", CallResult(std::string(R"({"_meta":false})")));
        },
        [](JsonRpcId const&, std::string) { return true; });
    auto status_commands = make_client_command_executor(std::string("session-") + std::string(name), status_gateway);
    auto status_result =
        status_commands->execute(ava::tools::CommandExecutionRequest{.argv = {"true"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 1024});
    auto const matches = expected_error.empty() ? status_result && status_result->exit_code == expected_exit
                                                : !status_result && status_result.error().format().find(expected_error) != std::string::npos;
    expect(matches && status_releases == 1, std::string("ACP terminal field-local default case: ") + std::string(name));
  };

  run_defaulted_status_case("wait-valid-code-sibling", R"({"exitCode":7,"signal":9,"_meta":[]})",
                            R"({"output":"ok","truncated":false,"exitStatus":[],"_meta":7})", 7);
  run_defaulted_status_case("output-valid-code-sibling", R"({"exitCode":"bad","signal":7})",
                            R"({"output":"ok","truncated":false,"exitStatus":{"exitCode":5,"signal":{},"_meta":[]}})", 5);
  run_defaulted_status_case("unknown-status", R"({"exitCode":-1,"signal":7})", R"({"output":"ok","truncated":false,"exitStatus":"bad"})", -1);
  run_defaulted_status_case("required-output-fields", R"({"exitCode":0})", R"({"output":"missing truncated","_meta":[]})", -1,
                            "requires string output and boolean truncated");

  auto malformed_create_gateway = std::make_shared<ClientRequestGateway>();
  int malformed_create_calls = 0;
  bool malformed_create_aborted = false;
  malformed_create_gateway->bind(
      [&](std::string, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        ++malformed_create_calls;
        return ready_call("malformed-create", CallResult(std::string(R"({"terminalId":7,"_meta":[]})")));
      },
      [](JsonRpcId const&, std::string) { return true; }, [&](std::string) { malformed_create_aborted = true; });
  auto malformed_create_commands = make_client_command_executor("session-malformed-create", malformed_create_gateway);
  auto malformed_create = malformed_create_commands->execute(
      ava::tools::CommandExecutionRequest{.argv = {"true"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 1024});
  expect(
      !malformed_create && malformed_create.error().format().find("terminalId") != std::string::npos && malformed_create_calls == 1 && malformed_create_aborted,
      "ACP terminal/create keeps its required terminalId strict and aborts when delivered ownership cannot be recovered");

  auto malformed_status_gateway = std::make_shared<ClientRequestGateway>();
  int malformed_status_releases = 0;
  malformed_status_gateway->bind(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "terminal/create")
          return ready_call("malformed-status-create", CallResult(std::string(R"({"terminalId":"terminal-malformed-status"})")));
        if (method == "terminal/wait_for_exit")
          return ready_call("malformed-status-wait", CallResult(std::string(R"({"exitCode":0,"signal":"SIGKILL"})")));
        if (method == "terminal/output")
          return ready_call("malformed-status-output", CallResult(std::string(R"({"output":"","truncated":false})")));
        if (method == "terminal/release")
          ++malformed_status_releases;
        return ready_call(method, CallResult(std::string("{}")));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto malformed_status_commands = make_client_command_executor("session-malformed-status", malformed_status_gateway);
  auto malformed_status = malformed_status_commands->execute(
      ava::tools::CommandExecutionRequest{.argv = {"true"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 1024});
  expect(!malformed_status && malformed_status.error().format().find("both a valid exitCode and a valid signal") != std::string::npos &&
             malformed_status_releases == 1,
         "ACP terminal adapter rejects one status containing both exit kinds and releases exactly once");

  auto conflicting_status_gateway = std::make_shared<ClientRequestGateway>();
  int conflicting_status_releases = 0;
  conflicting_status_gateway->bind(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "terminal/create")
          return ready_call("conflicting-status-create", CallResult(std::string(R"({"terminalId":"terminal-conflicting-status"})")));
        if (method == "terminal/wait_for_exit")
          return ready_call("conflicting-status-wait", CallResult(std::string(R"({"exitCode":0,"signal":null})")));
        if (method == "terminal/output")
          return ready_call("conflicting-status-output",
                            CallResult(std::string(R"({"output":"","truncated":false,"exitStatus":{"exitCode":null,"signal":"SIGKILL"}})")));
        if (method == "terminal/release")
          ++conflicting_status_releases;
        return ready_call(method, CallResult(std::string("{}")));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto conflicting_status_commands = make_client_command_executor("session-conflicting-status", conflicting_status_gateway);
  auto conflicting_status = conflicting_status_commands->execute(
      ava::tools::CommandExecutionRequest{.argv = {"true"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 1024});
  expect(!conflicting_status && conflicting_status.error().format().find("exitStatus conflicts") != std::string::npos && conflicting_status_releases == 1,
         "ACP terminal adapter rejects cross-response exit-kind disagreement and releases exactly once");

  auto signal_status_gateway = std::make_shared<ClientRequestGateway>();
  int signal_status_releases = 0;
  signal_status_gateway->bind(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "terminal/create")
          return ready_call("signal-status-create", CallResult(std::string(R"({"terminalId":"terminal-signal-status"})")));
        if (method == "terminal/wait_for_exit")
          return ready_call("signal-status-wait", CallResult(std::string(R"({"exitCode":"bad","signal":"SIGTERM"})")));
        if (method == "terminal/output")
          return ready_call("signal-status-output",
                            CallResult(std::string(R"({"output":"","truncated":false,"exitStatus":{"exitCode":{},"signal":"SIGTERM"}})")));
        if (method == "terminal/release")
          ++signal_status_releases;
        return ready_call(method, CallResult(std::string("{}")));
      },
      [](JsonRpcId const&, std::string) { return true; });
  auto signal_status_commands = make_client_command_executor("session-signal-status", signal_status_gateway);
  auto signal_status =
      signal_status_commands->execute(ava::tools::CommandExecutionRequest{.argv = {"true"}, .cwd = "/workspace", .timeout = 100ms, .output_byte_limit = 1024});
  expect(signal_status && signal_status->exit_code == -1 && signal_status_releases == 1,
         "ACP terminal adapter preserves valid signal siblings when malformed exitCode fields default absent and releases exactly once");
}
