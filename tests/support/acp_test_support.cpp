#include "sys.h"
#include "tests/support/acp_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_timeout.h"
#include "ava/http/transport.h"
#include "ava/app/acp/codec.h"
#include "ava/provider/registry.h"
#include "ava/core/error.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/result.h"
#include "ava/app/runtime/Session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace acp_test {

MemoryTransport::MemoryTransport(std::shared_ptr<MemoryTransportState> state) : state_(std::move(state))
{
}

ava::app::acp::ReadRecord MemoryTransport::read_record()
{
  std::unique_lock lock(state_->mutex);
  state_->read_waiting = true;
  state_->cv.notify_all();
  state_->cv.wait(lock, [&] { return state_->canceled || state_->eof || !state_->input.empty(); });
  if (state_->canceled)
  {
    state_->read_canceled = true;
    state_->cv.notify_all();
    return {.status = ava::app::acp::ReadRecordStatus::Canceled, .record = {}, .diagnostic = {}};
  }
  if (!state_->input.empty())
  {
    auto record = std::move(state_->input.front());
    state_->input.pop_front();
    return record;
  }
  return {.status = ava::app::acp::ReadRecordStatus::EndOfFile, .record = {}, .diagnostic = {}};
}

ava::core::VoidResult MemoryTransport::write_record(std::string const& record)
{
  std::unique_lock lock(state_->mutex);
  state_->write_waiting = true;
  ++state_->write_attempts;
  bool const published_before_stall = state_->publish_before_stall;
  if (published_before_stall)
    state_->output.push_back(record);
  state_->cv.notify_all();
  state_->cv.wait(lock, [&] { return !state_->block_writes || state_->canceled; });
  if (state_->fail_writes || (state_->canceled && !published_before_stall))
    return std::unexpected(ava::app::acp::protocol_error("simulated write failure"));
  if (!published_before_stall)
    state_->output.push_back(record);
  state_->cv.notify_all();
  return {};
}

void MemoryTransport::cancel() noexcept
{
  std::lock_guard lock(state_->mutex);
  ++state_->cancel_calls;
  state_->canceled = true;
  state_->cv.notify_all();
}

void feed(std::shared_ptr<MemoryTransportState> const& state, std::string record)
{
  std::lock_guard lock(state->mutex);
  state->input.push_back(
      {.status = ava::app::acp::ReadRecordStatus::Record, .record = std::move(record), .diagnostic = {}, .intent = ava::app::acp::EnvelopeIntent::Unknown});
  state->cv.notify_all();
}

void feed_limit_error(std::shared_ptr<MemoryTransportState> const& state, ava::app::acp::EnvelopeIntent intent)
{
  std::lock_guard lock(state->mutex);
  state->input.push_back({.status = ava::app::acp::ReadRecordStatus::RecoverableError, .record = {}, .diagnostic = "record too large", .intent = intent});
  state->cv.notify_all();
}

std::optional<std::string> take_output(std::shared_ptr<MemoryTransportState> const& state, std::chrono::milliseconds timeout)
{
  std::unique_lock lock(state->mutex);
  if (!state->cv.wait_for(lock, timeout, [&] { return !state->output.empty(); }))
    return std::nullopt;
  auto output = std::move(state->output.front());
  state->output.pop_front();
  return output;
}

void close_input(std::shared_ptr<MemoryTransportState> const& state)
{
  std::lock_guard lock(state->mutex);
  state->eof = true;
  state->cv.notify_all();
}

void wait_reader(std::shared_ptr<MemoryTransportState> const& state)
{
  std::unique_lock lock(state->mutex);
  bool const ready = state->cv.wait_until(lock, ava::tests::now_plus_seconds(2), [&] { return state->read_waiting; });
  lock.unlock();
  expect(ready, "ACP memory transport reader reaches read_record before the readiness watchdog");
}

void wait_writer(std::shared_ptr<MemoryTransportState> const& state)
{
  std::unique_lock lock(state->mutex);
  bool const ready = state->cv.wait_until(lock, ava::tests::now_plus_seconds(2), [&] { return state->write_waiting; });
  lock.unlock();
  expect(ready, "ACP memory transport writer reaches write_record before the readiness watchdog");
}

bool wait_for_write_attempts(std::shared_ptr<MemoryTransportState> const& state, std::size_t expected)
{
  std::unique_lock lock(state->mutex);
  return state->cv.wait_for(lock, 2s, [&] { return state->write_attempts >= expected; });
}

bool output_has_code(std::optional<std::string> const& output, int code)
{
  if (!output)
    return false;
  auto decoded = ava::app::acp::decode_message(*output);
  if (!decoded)
    return false;
  auto const* error = std::get_if<ava::app::acp::ErrorResponse>(&*decoded);
  return error != nullptr && error->error.code == code;
}
struct ReaderBarrier::State
{
  std::mutex mutex;
  std::condition_variable cv;
  std::string token = ava::core::make_id("reader-barrier");
  std::int64_t submitted = 0;
  std::int64_t acknowledged = 0;
};

ReaderBarrier::ReaderBarrier(ava::app::acp::JsonRpcPeer& peer, ava::app::acp::ControlNotificationHandler handler) : state_(std::make_shared<State>())
{
  peer.set_control_notification_handler([state = state_, handler = std::move(handler)](ava::app::acp::Notification const& notification) {
    if (notification.params_json && ava::core::json::string_field(*notification.params_json, "test_reader_barrier") == state->token)
    {
      auto const sequence = ava::core::json::integer_field(*notification.params_json, "sequence");
      if (sequence)
      {
        std::lock_guard lock(state->mutex);
        state->acknowledged = *sequence;
        state->cv.notify_all();
      }
      return;
    }
    if (handler)
      handler(notification);
  });
}

bool ReaderBarrier::synchronize(std::shared_ptr<MemoryTransportState> const& state, std::chrono::milliseconds timeout)
{
  std::unique_lock lock(state_->mutex);
  auto const sequence = ++state_->submitted;
  feed(state, "{\"jsonrpc\":\"2.0\",\"method\":\"session/cancel\",\"params\":{\"test_reader_barrier\":\"" + ava::core::json::escape(state_->token) +
                  "\",\"sequence\":" + std::to_string(sequence) + "}}");
  return state_->cv.wait_until(lock, ava::tests::now_plus_seconds(timeout), [&] { return state_->acknowledged >= sequence; });
}

bool synchronize_writer(ava::app::acp::JsonRpcPeer& peer, std::shared_ptr<MemoryTransportState> const& state, std::chrono::milliseconds timeout)
{
  auto const method = ava::core::make_id("test/writer-barrier");
  if (!peer.send_notification(method, std::string("{}")))
    return false;
  std::unique_lock lock(state->mutex);
  return state->cv.wait_until(lock, ava::tests::now_plus_seconds(timeout), [&] {
    auto const probe =
        std::ranges::find_if(state->output, [&](std::string const& record) { return ava::core::json::string_field(record, "method") == method; });
    if (probe == state->output.end())
      return false;
    state->output.erase(probe);
    return true;
  });
}

namespace {

ava::app::acp::AgentServiceOptions fixture_options(std::filesystem::path const& root, std::filesystem::path const& workspace,
                                                   ava::app::acp::AgentServiceOptions options, bool with_tools)
{
  std::filesystem::create_directories(workspace);
  if (with_tools)
    configure_acp_tool_test_model(root);
  else
    configure_acp_test_model(root);
  options.paths = ava::tests::app_test_paths(root);
  options.launch_root = workspace;
  if (options.agent_version.empty())
    options.agent_version = "1";
  return options;
}

}  // namespace

ScopedAgentService::ScopedAgentService(std::string_view name, ava::app::acp::AgentServiceOptions initial_options, bool with_tools)
    : root(ScopedTestDirectory::create_unique(temp_root(), name)),
      workspace(root / "workspace"),
      options(fixture_options(root.path(), workspace, std::move(initial_options), with_tools)),
      service(options)
{
}

ScopedAgentService::ScopedAgentService(ScopedTestDirectory owned_root, ava::app::acp::AgentServiceOptions initial_options)
    : root(std::move(owned_root)), workspace(initial_options.launch_root), options(std::move(initial_options)), service(options)
{
}

ScopedAgentService::~ScopedAgentService()
{
  service.shutdown();
}

ava::app::acp::RequestResult ScopedAgentService::initialize(std::int64_t id, std::optional<std::string> capabilities_json)
{
  return service.handle_request(capabilities_json ? initialize_request_with_capabilities(std::move(*capabilities_json), id) : initialize_request(id), {});
}

void configure_acp_test_model(std::filesystem::path const& root)
{
  auto paths = ava::tests::app_test_paths(root);
  std::filesystem::create_directories(paths.ava_config_dir);
  ava::tests::write_app_test_file(
      paths.models_file,
      R"({"default_provider":"moonshot","default_model":"acp-test","models":[{"provider":"moonshot","id":"acp-test","name":"ACP Test","family":"fake","supports_tools":false,"supports_streaming":false,"input_modalities":["text"],"output_modalities":["text"]}]})");
}

ava::app::acp::Request initialize_request(std::int64_t id)
{
  return ava::app::acp::Request{.id = id, .method = "initialize", .params_json = std::string(R"({"protocolVersion":1})")};
}

ava::app::acp::Request initialize_request_with_capabilities(std::string capabilities_json, std::int64_t id)
{
  return ava::app::acp::Request{
      .id = id, .method = "initialize", .params_json = std::string("{\"protocolVersion\":1,\"clientCapabilities\":") + capabilities_json + "}"};
}

namespace {

class RecordingTransport final : public ava::http::Transport
{
 public:
  RecordingTransport(std::string* body, std::atomic_bool* entered = nullptr, std::atomic_bool* release = nullptr)
      : body_(body), entered_(entered), release_(release)
  {
  }

  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    if (body_)
      *body_ = request.body;
    if (entered_)
      entered_->store(true, std::memory_order_release);
    while (release_ && !release_->load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = R"({"choices":[{"message":{"content":"recorded"},"finish_reason":"stop"}]})"};
  }

 private:
  std::string* body_ = nullptr;
  std::atomic_bool* entered_ = nullptr;
  std::atomic_bool* release_ = nullptr;
};

}  // namespace

ava::app::RuntimeProviderRunBundleFactory recording_bundle_factory(std::string* body, std::atomic_bool* entered, std::atomic_bool* release)
{
  return [body, entered, release](ava::app::runtime::session_ts const&, ava::app::runtime::RunOptions options,
                                  std::string_view) -> ava::core::Result<ava::app::RuntimeProviderRunBundle> {
    auto provider = ava::provider::builtin_provider_registry().create("moonshot");
    if (!provider)
      return std::unexpected(std::move(provider.error()));
    options.access_token = "test";
    options.stream = false;
    std::unique_ptr<ava::http::Transport> transport = std::make_unique<RecordingTransport>(body, entered, release);
    std::unique_ptr<ava::http::Transport> auth = std::make_unique<ava::tests::FakeTransport>(std::vector<ava::http::HttpResponse>{});
    return ava::app::RuntimeProviderRunBundle{
        .provider = std::move(*provider), .transport = std::move(transport), .auth_transport = std::move(auth), .options = std::move(options)};
  };
}

CapturingSequenceTransport::CapturingSequenceTransport(std::shared_ptr<CapturingSequenceState> state, std::vector<ava::http::HttpResponse> responses)
    : state_(std::move(state)), responses_(std::move(responses))
{
}

ava::core::Result<ava::http::HttpResponse> CapturingSequenceTransport::send(ava::http::HttpRequest const& request)
{
  {
    std::lock_guard lock(state_->mutex);
    state_->request_bodies.push_back(request.body);
  }
  if (responses_.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "capturing transport has no response"));
  auto response = std::move(responses_.front());
  responses_.erase(responses_.begin());
  return response;
}

ava::app::RuntimeProviderRunBundleFactory sequence_bundle_factory(std::shared_ptr<CapturingSequenceState> state, std::vector<ava::http::HttpResponse> responses)
{
  return [state = std::move(state), responses = std::move(responses)](ava::app::runtime::session_ts const&, ava::app::runtime::RunOptions options,
                                                                      std::string_view) -> ava::core::Result<ava::app::RuntimeProviderRunBundle> {
    auto provider = ava::provider::builtin_provider_registry().create("moonshot");
    if (!provider)
      return std::unexpected(std::move(provider.error()));
    options.access_token = "test";
    options.stream = false;
    std::unique_ptr<ava::http::Transport> transport = std::make_unique<CapturingSequenceTransport>(state, responses);
    std::unique_ptr<ava::http::Transport> auth = std::make_unique<ava::tests::FakeTransport>(std::vector<ava::http::HttpResponse>{});
    return ava::app::RuntimeProviderRunBundle{
        .provider = std::move(*provider), .transport = std::move(transport), .auth_transport = std::move(auth), .options = std::move(options)};
  };
}

ava::core::VoidResult RunPhaseBarrier::observe(ava::app::RunPhase phase)
{
  if (phase != target)
    return {};
  std::unique_lock lock(mutex);
  reached = true;
  cv.notify_all();
  cv.wait(lock, [&] { return released; });
  return {};
}

bool RunPhaseBarrier::wait_until_reached()
{
  std::unique_lock lock(mutex);
  return cv.wait_for(lock, 2s, [&] { return reached; });
}

void RunPhaseBarrier::release()
{
  {
    std::lock_guard lock(mutex);
    released = true;
  }
  cv.notify_all();
}

ava::http::HttpResponse acp_text_response(std::string_view text)
{
  return ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = std::string(R"({"choices":[{"message":{"content":")") + ava::core::json::escape(text) + R"("},"finish_reason":"stop"}]})"};
}

std::string read_acp_test_file(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  std::string text;
  std::getline(file, text);
  return text;
}

void configure_acp_tool_test_model(std::filesystem::path const& root)
{
  auto paths = ava::tests::app_test_paths(root);
  std::filesystem::create_directories(paths.ava_config_dir);
  ava::tests::write_app_test_file(
      paths.models_file,
      R"({"default_provider":"moonshot","default_model":"acp-tools","models":[{"provider":"moonshot","id":"acp-tools","name":"ACP Tools","family":"fake","supports_tools":true,"supports_streaming":false,"input_modalities":["text"],"output_modalities":["text"]}]})");
}

}  // namespace acp_test
