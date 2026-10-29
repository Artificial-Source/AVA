#pragma once
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/acp/peer.h"
#include "ava/app/acp/protocol.h"
#include "ava/app/acp/service.h"
#include "ava/app/acp/transport.h"
#include "ava/app/runtime_credentials.h"
#include "ava/app/session_run_controller.h"
#include "ava/provider/provider.h"
#include "ava/core/result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acp_test {

using namespace std::chrono_literals;

struct MemoryTransportState
{
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<ava::app::acp::ReadRecord> input;
  std::deque<std::string> output;
  bool eof = false;
  bool canceled = false;
  bool read_waiting = false;
  // Reader-side acknowledgement, distinct from cancel() having been requested.
  // Observe under mutex/cv; set immediately before read_record returns Canceled.
  bool read_canceled = false;
  bool fail_writes = false;
  bool block_writes = false;
  bool write_waiting = false;
  bool publish_before_stall = false;
  std::size_t cancel_calls = 0;
  std::size_t write_attempts = 0;
};

class MemoryTransport final : public ava::app::acp::RecordTransport
{
 public:
  explicit MemoryTransport(std::shared_ptr<MemoryTransportState> state);

  ava::app::acp::ReadRecord read_record() override;
  ava::core::VoidResult write_record(std::string const& record) override;
  void cancel() noexcept override;

 private:
  std::shared_ptr<MemoryTransportState> state_;
};

void feed(std::shared_ptr<MemoryTransportState> const& state, std::string record);
void feed_limit_error(std::shared_ptr<MemoryTransportState> const& state, ava::app::acp::EnvelopeIntent intent);
std::optional<std::string> take_output(std::shared_ptr<MemoryTransportState> const& state, std::chrono::milliseconds timeout = 2s);
void close_input(std::shared_ptr<MemoryTransportState> const& state);
// Suite-thread setup waits: record a named expect() failure on watchdog expiry.
// These acknowledge first transport entry, not completion of a particular record;
// use ReaderBarrier/synchronize_writer for processing/delivery assertions.
void wait_reader(std::shared_ptr<MemoryTransportState> const& state);
void wait_writer(std::shared_ptr<MemoryTransportState> const& state);
bool wait_for_write_attempts(std::shared_ptr<MemoryTransportState> const& state, std::size_t expected);
bool output_has_code(std::optional<std::string> const& output, int code);

// Install before peer.run(). Its private session/cancel probes acknowledge all
// earlier reader records without canceling application work. The optional handler
// receives every non-probe control notification. Shared acknowledgement state
// outlives this wrapper if the peer still holds its callback.
class ReaderBarrier
{
 public:
  explicit ReaderBarrier(ava::app::acp::JsonRpcPeer& peer, ava::app::acp::ControlNotificationHandler handler = {});
  // Serialize calls on one barrier. False means the reader did not acknowledge
  // before timeout; callers must still release their writer/worker gates.
  [[nodiscard]] bool synchronize(std::shared_ptr<MemoryTransportState> const& state, std::chrono::milliseconds timeout = 2s);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

// Queues a private outbound notification, then waits until it reaches the
// transport: the sole writer has acknowledged every earlier write by then.
// Consumes only its own probe; unrelated output remains available to take_output.
[[nodiscard]] bool synchronize_writer(ava::app::acp::JsonRpcPeer& peer, std::shared_ptr<MemoryTransportState> const& state,
                                      std::chrono::milliseconds timeout = 2s);

// Owns a unique root, workspace, isolated paths/model and service. Destruction
// shuts down the service before removing files, including on early return.
// The name-based constructor creates paths/model files before service creation.
// Initialization is explicit so negotiation tests can vary it.
struct ScopedAgentService
{
  explicit ScopedAgentService(std::string_view name, ava::app::acp::AgentServiceOptions options = {}, bool with_tools = false);
  // Adopts an existing owned root without rewriting options, launch_root, paths
  // or configured files. workspace mirrors the supplied launch_root.
  ScopedAgentService(ScopedTestDirectory owned_root, ava::app::acp::AgentServiceOptions options);
  ~ScopedAgentService();
  // Returns the real initialize result; the caller must assert success.
  [[nodiscard]] ava::app::acp::RequestResult initialize(std::int64_t id = 1, std::optional<std::string> capabilities_json = std::nullopt);

  ScopedTestDirectory root;
  std::filesystem::path workspace;
  ava::app::acp::AgentServiceOptions options;
  ava::app::acp::AgentService service;
};

void configure_acp_test_model(std::filesystem::path const& root);
ava::app::acp::Request initialize_request(std::int64_t id = 1);
ava::app::acp::Request initialize_request_with_capabilities(std::string capabilities_json, std::int64_t id = 1);

ava::app::RuntimeProviderRunBundleFactory recording_bundle_factory(std::string* body, std::atomic_bool* entered = nullptr, std::atomic_bool* release = nullptr);

struct CapturingSequenceState
{
  std::mutex mutex;
  std::vector<std::string> request_bodies;
};

class CapturingSequenceTransport final : public ava::http::Transport
{
 public:
  CapturingSequenceTransport(std::shared_ptr<CapturingSequenceState> state, std::vector<ava::http::HttpResponse> responses);

  using ava::http::Transport::send;
  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override;

 private:
  std::shared_ptr<CapturingSequenceState> state_;
  std::vector<ava::http::HttpResponse> responses_;
};

ava::app::RuntimeProviderRunBundleFactory sequence_bundle_factory(std::shared_ptr<CapturingSequenceState> state,
                                                                  std::vector<ava::http::HttpResponse> responses);

struct RunPhaseBarrier
{
  std::mutex mutex;
  std::condition_variable cv;
  ava::app::RunPhase target = ava::app::RunPhase::AwaitingProvider;
  bool reached = false;
  bool released = false;

  ava::core::VoidResult observe(ava::app::RunPhase phase);
  bool wait_until_reached();
  void release();
};

ava::http::HttpResponse acp_text_response(std::string_view text = "recorded");
std::string read_acp_test_file(std::filesystem::path const& path);
void configure_acp_tool_test_model(std::filesystem::path const& root);

}  // namespace acp_test
