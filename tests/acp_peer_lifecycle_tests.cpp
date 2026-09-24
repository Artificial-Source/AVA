#include "sys.h"
#include "tests/acp_test_declarations.h"
#include "tests/support/acp_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/app/acp/codec.h"
#include "ava/app/acp/peer.h"
#include "ava/app/acp/service.h"
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
#include <deque>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using ava::app::acp::JsonRpcId;
using namespace acp_test;
namespace runtime = ava::app::runtime;

void test_acp_peer_shutdown_cancels_blocked_transport()
{
  using namespace ava::app::acp;
  auto state = std::make_shared<MemoryTransportState>();
  JsonRpcPeer peer(std::make_unique<MemoryTransport>(state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("shutdown_reader", [&] { run_result = peer.run(); });
  wait_reader(state);

  peer.shutdown();
  thread.join();

  std::lock_guard lock(state->mutex);
  expect(state->canceled && state->cancel_calls > 0 && run_result.has_value(), "ACP shutdown cancels transport and wakes a peer blocked in read_record");
}

void test_acp_peer_lifecycle_notifications_and_duplicate_ids()
{
  using namespace ava::app::acp;
  auto state = std::make_shared<MemoryTransportState>();
  AgentService service("1.0.0");
  JsonRpcPeer peer(
      std::make_unique<MemoryTransport>(state), [&service](Request const& request, std::stop_token token) { return service.handle_request(request, token); },
      [&service](Notification const& notification, std::stop_token token) { service.handle_notification(notification, token); });
  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
  wait_reader(state);

  feed(state, R"({"jsonrpc":"2.0","id":1,"method":"session/new","params":{}})");
  expect(output_has_code(take_output(state), -32600), "ACP peer enforces pre-init gating");
  feed(state, R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":1}})");
  auto init = take_output(state);
  expect(init && init->find("\"result\"") != std::string::npos && init->find("\"protocolVersion\":1") != std::string::npos,
         "ACP peer successfully negotiates the M4 v1 baseline");
  feed(state, R"({"jsonrpc":"2.0","method":"unknown","params":{}})");
  expect(!take_output(state, 80ms), "ACP notification failures produce no response");
  feed(state, R"({"jsonrpc":"2.0","id":3,"method":"unknown","params":{}})");
  expect(output_has_code(take_output(state), -32601), "ACP peer routes unknown requests after initialization");
  feed(state, R"({"jsonrpc":"1.0","id":9,"result":{}})");
  feed(state, R"({"jsonrpc":"2.0","id":9,"error":{"code":"bad","message":1}})");
  feed(state, R"({"jsonrpc":"2.0","id":10})");
  feed(state, R"({"jsonrpc":"2.0","id":null})");
  feed(state, R"({"jsonrpc":"2.0","id":11,"result":{},"error":{"code":1,"message":"bad"}})");
  expect(!take_output(state, 80ms) && peer.stats().unknown_or_late_responses == 5,
         "all malformed response-intent envelopes are diagnosed and ignored without response loops");

  std::string deep_value(kMaxNestingDepth + 1, '[');
  deep_value += '0';
  deep_value.append(kMaxNestingDepth + 1, ']');
  feed(state, R"({"jsonrpc":"2.0","id":20,"result":)" + deep_value + '}');
  feed(state, R"({"jsonrpc":"2.0","result":)" + deep_value + R"(,"id":null})");
  feed_limit_error(state, EnvelopeIntent::Response);
  feed_limit_error(state, EnvelopeIntent::Response);
  feed_limit_error(state, EnvelopeIntent::Notification);
  expect(!take_output(state, 80ms) && peer.stats().unknown_or_late_responses == 9,
         "over-depth and oversized response hints, plus oversized notifications, remain response-free");
  feed_limit_error(state, EnvelopeIntent::Request);
  expect(output_has_code(take_output(state), -32700), "an oversized request hint still produces the bounded parse error");

  feed(state, R"({"jsonrpc":"2.0","id":12,"method":"unknown","params":{}})");
  expect(output_has_code(take_output(state), -32601), "ACP peer continues handling valid input after depth and size intent errors");

  close_input(state);
  thread.join();
  expect(run_result.has_value(), "ACP peer exits cleanly on EOF");
}

void test_acp_peer_bidirectional_out_of_order_deadline_and_late_response()
{
  using namespace ava::app::acp;
  auto state = std::make_shared<MemoryTransportState>();
  JsonRpcPeer peer(std::make_unique<MemoryTransport>(state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
  wait_reader(state);

  auto first = peer.send_request("client/one", std::string("{}"), 2s);
  auto second = peer.send_request("client/two", std::string("{}"), 2s);
  expect(first && second, "ACP peer admits bounded outbound calls");
  static_cast<void>(take_output(state));
  static_cast<void>(take_output(state));
  if (first && second)
  {
    auto second_response = encode_success(second->id, R"({"order":2})");
    auto first_response = encode_success(first->id, R"({"order":1})");
    feed(state, *second_response);
    feed(state, *first_response);
    auto second_done = second->completion.get();
    auto first_done = first->completion.get();
    expect(second_done && second_done->find("2") != std::string::npos && first_done && first_done->find("1") != std::string::npos,
           "ACP peer correlates out-of-order responses by typed id");
  }

  auto canceled = peer.send_request("client/cancel", std::string("{}"), 2s);
  expect(canceled.has_value(), "ACP peer admits an outbound call that the active prompt may cancel");
  auto canceled_record = take_output(state);
  auto cancellation_barrier_sent = peer.send_notification("test/cancellation_delivery_barrier", std::string("{}"));
  auto cancellation_barrier_record = take_output(state);
  expect(canceled_record && cancellation_barrier_sent && cancellation_barrier_record,
         "a subsequent outbound barrier observes the normal request after writer-acknowledged delivery");
  if (canceled)
  {
    peer.cancel_pending_call(canceled->id, "prompt cancelled");
    auto cancellation = canceled->completion.get();
    expect(!cancellation && cancellation.error().code == -32800, "prompt cancellation releases one outbound client request without closing the connection");
    auto late_canceled = encode_success(canceled->id, R"({"outcome":{"outcome":"selected","optionId":"allow_always"}})");
    feed(state, *late_canceled);
    std::this_thread::sleep_for(20ms);
    expect(peer.stats().unknown_or_late_responses == 1, "a response arriving after prompt cancellation is ignored and cannot apply a permission grant");
  }

  auto timeout = peer.send_request("client/slow", std::string("{}"), 30ms);
  expect(timeout.has_value(), "ACP peer admits a finite-deadline call");
  static_cast<void>(take_output(state));
  if (timeout)
  {
    auto timed_out = timeout->completion.get();
    expect(!timed_out && timed_out.error().code == -32603 && timed_out.error().message.find("outcome is unknown") != std::string::npos,
           "a deadline after writer delivery reports an ambiguous response outcome rather than local rollback");
    auto late = encode_success(timeout->id, "{}");
    feed(state, *late);
    std::this_thread::sleep_for(20ms);
    expect(peer.stats().unknown_or_late_responses == 2, "ACP peer safely ignores and counts a late response");
  }
  feed(state, R"({"jsonrpc":"2.0","id":null,"result":{"unknown":true}})");
  feed(state, R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"unknown"}})");
  std::this_thread::sleep_for(20ms);
  expect(peer.stats().unknown_or_late_responses == 4, "null success and error responses preserve identity and follow bounded unknown-response handling");

  bool races_completed = true;
  for (int index = 0; index < 20; ++index)
  {
    auto call = peer.send_request("client/race", std::string("{}"), 3ms);
    if (!call)
    {
      races_completed = false;
      break;
    }
    static_cast<void>(take_output(state));
    auto response_record = encode_success(call->id, "{}");
    ava::core::JoinThread responder = ava::core::JoinThread::create("responder", [state, response_record, index] {
      std::this_thread::sleep_for(index % 2 == 0 ? 1ms : 3ms);
      feed(state, *response_record);
    });
    auto completion = call->completion.get();
    races_completed = races_completed && (completion.has_value() ||
                                          (completion.error().code == -32603 && completion.error().message.find("outcome is unknown") != std::string::npos));
  }
  expect(races_completed, "ACP response-versus-deadline races have exactly one pending-call completion owner");

  close_input(state);
  thread.join();
  expect(run_result.has_value(), "ACP bidirectional peer shuts down after interleaving");
}

void test_acp_peer_cancel_duplicate_inflight_and_saturation()
{
  using namespace ava::app::acp;
  auto state = std::make_shared<MemoryTransportState>();
  std::atomic_int started = 0;
  JsonRpcPeer peer(std::make_unique<MemoryTransport>(state), [&started](Request const&, std::stop_token token) -> RequestResult {
    started.fetch_add(1, std::memory_order_release);
    while (!token.stop_requested())
      std::this_thread::sleep_for(1ms);
    return std::unexpected(JsonRpcError{.code = -32800, .message = "cancelled", .data_json = std::nullopt, .id = std::nullopt, .suppress_response = false});
  });
  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
  wait_reader(state);

  feed(state, R"({"jsonrpc":"2.0","id":null,"method":"slow","params":{}})");
  while (started.load(std::memory_order_acquire) == 0)
    std::this_thread::sleep_for(1ms);
  feed(state, R"({"jsonrpc":"2.0","id":null,"method":"slow","params":{}})");
  expect(output_has_code(take_output(state), -32600), "ACP peer reserves explicit null ids and rejects a colliding in-flight request");
  feed(state, R"({"jsonrpc":"2.0","method":"$/cancel_request","params":{"requestId":null}})");
  auto null_cancellation = take_output(state);
  expect(output_has_code(null_cancellation, -32800) && null_cancellation->find("\"id\":null") != std::string::npos,
         "$/cancel_request correlates null and preserves it on the cancellation response");
  auto null_release_barrier_sent = peer.send_notification("test/null_release_barrier", std::string("{}"));
  auto null_release_barrier = take_output(state);
  expect(null_release_barrier_sent && null_release_barrier && null_release_barrier->find("test/null_release_barrier") != std::string::npos,
         "a writer barrier observes null cancellation after its in-flight slot is released");
  expect(peer.stats().duplicate_inbound_ids == 1 && peer.stats().canceled_inbound_requests == 1,
         "ACP peer records duplicate and cancellation routing outcomes");

  for (int index = 0; index <= static_cast<int>(kMaxInflightRequests); ++index)
    feed(state, "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(index) + ",\"method\":\"slow\",\"params\":{}}");
  expect(output_has_code(take_output(state), -32603), "ACP peer enforces the concurrent inbound request limit");
  for (int index = 0; index < static_cast<int>(kMaxInflightRequests); ++index)
    feed(state, "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancel_request\",\"params\":{\"requestId\":" + std::to_string(index) + "}}");
  bool inflight_released = true;
  for (std::size_t index = 0; index < kMaxInflightRequests; ++index)
    inflight_released = inflight_released && output_has_code(take_output(state), -32800);
  expect(inflight_released, "ACP cancellation releases every saturated in-flight slot");

  std::deque<PendingCall> pending;
  for (std::size_t index = 0; index < kMaxPendingCalls; ++index)
  {
    auto call = peer.send_request("client/pending", std::string("{}"), 5s);
    if (call)
      pending.push_back(std::move(*call));
  }
  auto saturated = peer.send_request("client/overflow", std::string("{}"), 5s);
  expect(pending.size() == kMaxPendingCalls && !saturated, "ACP peer enforces the pending outbound call limit");

  close_input(state);
  thread.join();
  bool all_released = true;
  for (auto& call : pending)
  {
    auto result = call.completion.get();
    all_released = all_released && !result &&
                   (result.error().code == -32800 || (result.error().code == -32603 && result.error().message.find("outcome is unknown") != std::string::npos));
  }
  expect(all_released && run_result.has_value(), "ACP EOF releases every pending outbound call with delivery-aware terminal errors");
}

void test_acp_peer_lifecycle_request_commit_linearization()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-peer-lifecycle-commit");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_test_model(root);

  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = ava::tests::app_test_paths(root);
  AgentService service(options);
  auto state = std::make_shared<MemoryTransportState>();
  JsonRpcPeer peer(std::make_unique<MemoryTransport>(state),
                   [&service](Request const& request, std::stop_token token) { return service.handle_request(request, token); });
  std::mutex barrier_mutex;
  std::condition_variable barrier_cv;
  bool committed = false;
  bool release = false;
  service.bind_request_terminal_committer([&](JsonRpcId const& id) {
    bool const accepted = peer.commit_inbound_request(id);
    auto const* text = std::get_if<std::string>(&id);
    if (text == nullptr || *text != "new-late" || !accepted)
      return accepted;
    std::unique_lock lock(barrier_mutex);
    committed = true;
    barrier_cv.notify_all();
    barrier_cv.wait(lock, [&] { return release; });
    return true;
  });

  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
  wait_reader(state);
  feed(state, R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":1}})");
  auto initialized = take_output(state);
  expect(initialized && initialized->find("\"result\"") != std::string::npos, "ACP peer lifecycle fixture commits initialize successfully");

  feed(state, std::string("{\"jsonrpc\":\"2.0\",\"id\":\"new-late\",\"method\":\"session/new\",\"params\":{\"cwd\":\"") + workspace.string() +
                  "\",\"mcpServers\":[]}}");
  {
    std::unique_lock lock(barrier_mutex);
    static_cast<void>(barrier_cv.wait_for(lock, 2s, [&] { return committed; }));
  }
  feed(state, R"({"jsonrpc":"2.0","method":"$/cancel_request","params":{"requestId":"new-late"}})");
  std::this_thread::sleep_for(10ms);
  {
    std::lock_guard lock(barrier_mutex);
    release = true;
  }
  barrier_cv.notify_all();
  auto created = take_output(state);
  auto created_result = created ? ava::core::json::object_field(*created, "result") : std::nullopt;
  auto session_id = created_result ? ava::core::json::string_field(*created_result, "sessionId") : std::nullopt;
  expect(session_id && peer.stats().canceled_inbound_requests == 0,
         "late $/cancel_request loses after session/new commits and the actual successful mutation response is retained");

  close_input(state);
  thread.join();
  service.unbind_request_terminal_committer();
  service.shutdown();
  expect(run_result.has_value(), "lifecycle request commit linearization test shuts down cleanly");
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_peer_outbound_queue_saturation()
{
  using namespace ava::app::acp;
  auto state = std::make_shared<MemoryTransportState>();
  state->block_writes = true;
  JsonRpcPeer peer(std::make_unique<MemoryTransport>(state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
  wait_reader(state);

  expect(peer.send_notification("test/block", std::string("{}")).has_value(), "ACP saturation regression claims one blocked writer record");
  wait_writer(state);
  std::size_t queued = 0;
  for (std::size_t index = 0; index <= kMaxOutboundRecords; ++index)
  {
    auto sent = peer.send_notification("test/queued", std::string("{}"));
    if (!sent)
      break;
    ++queued;
  }
  expect(queued == kMaxOutboundRecords, "ACP peer deterministically fills the bounded outbound FIFO behind a blocked writer");

  auto const old_deadline = ava::tests::now_plus_seconds(5);
  std::size_t failed_requests = 0;
  for (std::size_t index = 0; index < kMaxPendingCalls; ++index)
  {
    auto failed = peer.send_request("client/saturated", std::string("{}"), 5s);
    if (!failed)
      ++failed_requests;
  }
  expect(failed_requests == kMaxPendingCalls, "every saturated request enqueue fails without consuming a pending-call slot");

  {
    std::lock_guard lock(state->mutex);
    state->block_writes = false;
    state->cv.notify_all();
  }
  bool drained = true;
  for (std::size_t index = 0; index < queued + 1; ++index)
    drained = drained && take_output(state).has_value();

  auto recovered = peer.send_request("client/recovered", std::string("{}"), 2s);
  auto recovered_record = take_output(state);
  if (recovered)
  {
    auto response = encode_success(recovered->id, R"({"recovered":true})");
    feed(state, *response);
  }
  bool const recovered_ready = recovered && recovered->completion.wait_for(2s) == std::future_status::ready;
  auto recovered_result = recovered_ready ? std::optional<CallResult>(recovered->completion.get()) : std::nullopt;
  expect(drained && recovered_record && recovered_result && recovered_result->has_value() && std::chrono::steady_clock::now() < old_deadline,
         "failed enqueue rollback leaves no dangling pending entries or futures and a later request completes before stale deadlines");

  close_input(state);
  thread.join();
  expect(run_result.has_value(), "ACP saturation rollback regression shuts down cleanly");
}

void test_acp_peer_delivered_fail_stop_cancellation()
{
  using namespace ava::app::acp;
  auto state = std::make_shared<MemoryTransportState>();
  JsonRpcPeer peer(std::make_unique<MemoryTransport>(state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
  wait_reader(state);

  auto write = peer.send_request("fs/write_text_file", std::string("{}"), 2s, OutboundCallPolicy::AbortConnectionIfDelivered);
  auto write_record = take_output(state);
  auto barrier_sent = peer.send_notification("test/delivery_barrier", std::string("{}"));
  auto barrier_record = take_output(state);
  bool const canceled = write && peer.cancel_pending_call(write->id, "prompt canceled after file write delivery");
  bool const ready = write && write->completion.wait_for(2s) == std::future_status::ready;
  auto result = ready ? std::optional<CallResult>(write->completion.get()) : std::nullopt;
  auto rejected = peer.send_request("fs/write_text_file", std::string("{}"), 2s, OutboundCallPolicy::AbortConnectionIfDelivered);

  thread.join();
  expect(write_record && barrier_sent && barrier_record && canceled && result && !*result && result->error().code == -32603 &&
             result->error().message.find("outcome is unknown") != std::string::npos && state->canceled && state->cancel_calls > 0 && !rejected &&
             run_result.has_value(),
         "canceling a delivered fail-stop call returns ambiguous delivery, poisons the connection, and rejects later mutations");

  auto deadline_state = std::make_shared<MemoryTransportState>();
  JsonRpcPeer deadline_peer(std::make_unique<MemoryTransport>(deadline_state),
                            [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::VoidResult deadline_run;
  ava::core::JoinThread deadline_thread = ava::core::JoinThread::create("deadline_thread", [&] { deadline_run = deadline_peer.run(); });
  wait_reader(deadline_state);
  auto deadline_write = deadline_peer.send_request("fs/write_text_file", std::string("{}"), 50ms, OutboundCallPolicy::AbortConnectionIfDelivered);
  auto deadline_write_record = take_output(deadline_state);
  auto deadline_barrier_sent = deadline_peer.send_notification("test/deadline_delivery_barrier", std::string("{}"));
  auto deadline_barrier_record = take_output(deadline_state);
  bool const deadline_ready = deadline_write && deadline_write->completion.wait_for(2s) == std::future_status::ready;
  auto deadline_result = deadline_ready ? std::optional<CallResult>(deadline_write->completion.get()) : std::nullopt;
  auto deadline_rejected = deadline_peer.send_request("fs/write_text_file", std::string("{}"), 2s, OutboundCallPolicy::AbortConnectionIfDelivered);
  deadline_thread.join();
  expect(deadline_write_record && deadline_barrier_sent && deadline_barrier_record && deadline_result && !*deadline_result &&
             deadline_result->error().code == -32603 && deadline_result->error().message.find("outcome is unknown") != std::string::npos &&
             deadline_state->canceled && !deadline_rejected && deadline_run.has_value(),
         "a delivered fail-stop deadline reports ambiguity, aborts the connection, and rejects later mutations");
}

void test_acp_peer_fail_stop_poison_arbitrates_all_pending_calls()
{
  using namespace ava::app::acp;

  auto run_case = [](bool deadline_triggered) {
    auto state = std::make_shared<MemoryTransportState>();
    std::mutex reader_barrier_mutex;
    std::condition_variable reader_barrier_cv;
    bool reader_barrier_reached = false;
    JsonRpcPeer peer(std::make_unique<MemoryTransport>(state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
    peer.set_control_notification_handler([&](Notification const&) {
      {
        std::lock_guard lock(reader_barrier_mutex);
        reader_barrier_reached = true;
      }
      reader_barrier_cv.notify_all();
    });
    ava::core::VoidResult run_result;
    ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
    wait_reader(state);

    auto trigger = peer.send_request("fs/write_text_file", std::string("{}"), deadline_triggered ? 1s : 5s, OutboundCallPolicy::AbortConnectionIfDelivered);
    auto trigger_record = take_output(state);
    auto delivery_barrier_sent = peer.send_notification("test/fail_stop_delivery_barrier", std::string("{}"));
    auto delivery_barrier_record = take_output(state);

    std::size_t claimed_write_attempt = 0;
    {
      std::lock_guard lock(state->mutex);
      state->block_writes = true;
      claimed_write_attempt = state->write_attempts + 1;
    }
    auto collateral = peer.send_request("fs/write_text_file", std::string("{}"), 5s, OutboundCallPolicy::AbortConnectionIfDelivered);
    bool const writer_claimed = wait_for_write_attempts(state, claimed_write_attempt);
    if (collateral)
    {
      auto staged_response = encode_success(collateral->id, R"({})");
      if (staged_response)
        feed(state, std::move(*staged_response));
    }
    feed(state, R"({"jsonrpc":"2.0","method":"session/cancel","params":{}})");
    bool response_processed = false;
    {
      std::unique_lock lock(reader_barrier_mutex);
      response_processed = reader_barrier_cv.wait_for(lock, 2s, [&] { return reader_barrier_reached; });
    }
    bool const response_staged = collateral && collateral->completion.wait_for(0ms) != std::future_status::ready;

    bool trigger_started = deadline_triggered;
    if (!deadline_triggered && trigger)
      trigger_started = peer.cancel_pending_call(trigger->id, "prompt canceled after fail-stop delivery");

    bool const trigger_ready = trigger && trigger->completion.wait_for(3s) == std::future_status::ready;
    auto trigger_result = trigger_ready ? std::optional<CallResult>(trigger->completion.get()) : std::nullopt;
    bool const collateral_ready = collateral && collateral->completion.wait_for(3s) == std::future_status::ready;
    auto collateral_result = collateral_ready ? std::optional<CallResult>(collateral->completion.get()) : std::nullopt;
    auto rejected_mutation = peer.send_request("fs/write_text_file", std::string("{}"), 2s, OutboundCallPolicy::AbortConnectionIfDelivered);

    peer.shutdown();
    thread.join();
    bool const trigger_is_ambiguous = trigger_result && !trigger_result->has_value() && trigger_result->error().code == -32603 &&
                                      trigger_result->error().message.find("outcome is unknown") != std::string::npos;
    bool const collateral_failed = collateral_result && !collateral_result->has_value() && collateral_result->error().code == -32800 &&
                                   collateral_result->error().message.find("connection closed") != std::string::npos;
    expect(trigger_record && delivery_barrier_sent && delivery_barrier_record && writer_claimed && response_processed && response_staged && trigger_started &&
               trigger_is_ambiguous && collateral_failed && !rejected_mutation && state->canceled && state->cancel_calls > 0 && run_result.has_value(),
           deadline_triggered
               ? "a delivered fail-stop deadline atomically cancels a claimed call with a reader-staged success and rejects later mutations"
               : "canceling a delivered fail-stop call atomically cancels a claimed call with a reader-staged success and rejects later mutations");
  };

  run_case(false);
  run_case(true);
}

void test_acp_peer_writer_acknowledged_lifecycle()
{
  using namespace ava::app::acp;
  auto state = std::make_shared<MemoryTransportState>();
  state->block_writes = true;
  std::atomic_int side_effects = 0;
  JsonRpcPeer* handler_peer = nullptr;
  std::promise<bool> handler_commit;
  auto handler_committed = handler_commit.get_future();
  JsonRpcPeer peer(std::make_unique<MemoryTransport>(state),
                   [&side_effects, &handler_peer, &handler_commit](Request const& request, std::stop_token) -> RequestResult {
                     side_effects.fetch_add(1, std::memory_order_relaxed);
                     handler_commit.set_value(handler_peer->commit_inbound_request(request.id));
                     return std::string(R"({"ok":true})");
                   });
  handler_peer = &peer;
  ava::core::VoidResult run_result;
  ava::core::JoinThread thread = ava::core::JoinThread::create("thread", [&] { run_result = peer.run(); });
  wait_reader(state);

  expect(peer.send_notification("test/block", std::string("{}")).has_value(), "ACP lifecycle test claims a leading stalled write");
  wait_writer(state);
  auto timed = peer.send_request("client/timeout-before-claim", std::string("{}"), 100ms);
  expect(timed.has_value(), "ACP lifecycle test queues an outbound call behind the stalled writer");
  if (timed)
  {
    auto premature = encode_success(timed->id, R"({"outcome":{"outcome":"selected","optionId":"allow_always"}})");
    feed(state, *premature);
    std::this_thread::sleep_for(20ms);
    bool const premature_fulfilled = timed->completion.wait_for(0ms) == std::future_status::ready;
    auto result = timed->completion.get();
    expect(!premature_fulfilled && !result && result.error().code == -32800,
           "a response for a queued outbound id is ignored and cannot grant permission before request delivery");
  }

  feed(state, R"({"jsonrpc":"2.0","id":"held","method":"work","params":{}})");
  bool const handler_commit_ready = handler_committed.wait_for(2s) == std::future_status::ready;
  bool const handler_commit_succeeded = handler_commit_ready && handler_committed.get();
  feed(state, R"({"jsonrpc":"2.0","id":"held","method":"work","params":{}})");
  feed(state, R"({"jsonrpc":"2.0","method":"$/cancel_request","params":{"requestId":"held"}})");
  while (peer.stats().duplicate_inbound_ids == 0)
    std::this_thread::sleep_for(1ms);

  {
    std::lock_guard lock(state->mutex);
    state->block_writes = false;
    state->cv.notify_all();
  }
  auto blocker = take_output(state);
  auto terminal_response_one = take_output(state);
  auto terminal_response_two = take_output(state);
  std::string const terminal_responses = terminal_response_one.value_or("") + terminal_response_two.value_or("");
  expect(blocker && blocker->find("test/block") != std::string::npos && terminal_response_one && terminal_response_two &&
             terminal_responses.find("\"result\"") != std::string::npos && terminal_responses.find("\"code\":-32600") != std::string::npos &&
             terminal_responses.find("Duplicate in-flight request id") != std::string::npos && handler_commit_succeeded && side_effects.load() == 1 &&
             peer.stats().canceled_inbound_requests == 0,
         "late cancellation loses to the atomically committed handler result while retaining the inbound id and preventing duplicate side effects");
  expect(!take_output(state, 80ms), "each admitted inbound envelope produces exactly one terminal response under a stalled writer");
  std::string const delivered = blocker.value_or("") + terminal_responses;
  expect(!timed || delivered.find(id_debug_string(timed->id)) == std::string::npos,
         "a timed-out outbound call is skipped rather than sent after the writer resumes");

  close_input(state);
  thread.join();
  expect(run_result.has_value(), "writer acknowledgement lifecycle shuts down cleanly");

  auto eof_state = std::make_shared<MemoryTransportState>();
  eof_state->block_writes = true;
  JsonRpcPeer eof_peer(std::make_unique<MemoryTransport>(eof_state), [](Request const&, std::stop_token) -> RequestResult { return std::string("{}"); });
  ava::core::JoinThread eof_thread = ava::core::JoinThread::create("eof_thread", [&] { static_cast<void>(eof_peer.run()); });
  wait_reader(eof_state);
  static_cast<void>(eof_peer.send_notification("test/eof-block", std::string("{}")));
  wait_writer(eof_state);
  auto eof_call = eof_peer.send_request("client/eof-before-claim", std::string("{}"), 5s);
  close_input(eof_state);
  expect(eof_call.has_value() && !eof_call->completion.get(), "EOF releases an outbound call queued behind a claimed write");
  eof_thread.join();
  {
    std::lock_guard lock(eof_state->mutex);
    eof_state->block_writes = false;
    eof_state->cv.notify_all();
  }
  expect(eof_state->canceled && !take_output(eof_state, 80ms), "EOF aborts the claimed transport write and skips queued outbound records");
}
