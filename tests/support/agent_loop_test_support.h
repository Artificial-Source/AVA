#pragma once
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/observability/run_observer.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/model_invocation_options.h"
#include "ava/agent/subagent_coordinator.h"
#include "ava/tools/tool_io.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/provider.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agent_loop_test {

class TraceCollector final : public ava::observability::RunObserver
{
 public:
  void on_event(ava::observability::TraceEvent const& event) override;

  std::mutex mutex;
  std::vector<ava::observability::TraceEvent> events;
};

ava::http::HttpResponse sse_response(std::string const& body);
std::string tool_call_sse(std::string_view id, std::string_view name, std::string_view arguments_json);
ava::agent::ModelInvocationOptions model_invocation_options(std::string system_prompt = "system prompt", std::string provider_id = "openai",
                                                            std::string model_id = "gpt-5.5");

// Owns a unique root, workspace, persistent store and offline provider. Keep the
// fixture alive until every loop using its options and every worker has stopped.
struct AgentLoopFixture
{
  explicit AgentLoopFixture(std::string_view name, ava::agent::Mode mode = ava::agent::Mode::Build);
  AgentLoopFixture(AgentLoopFixture const&) = delete;
  AgentLoopFixture& operator=(AgentLoopFixture const&) = delete;
  AgentLoopFixture(AgentLoopFixture&&) = delete;
  AgentLoopFixture& operator=(AgentLoopFixture&&) = delete;

  // Returns token-authenticated options with exact append/read authority; callers
  // may override policy or callbacks on the returned value before constructing a loop.
  [[nodiscard]] ava::agent::AgentLoopOptions options(ava::agent::ModelInvocationOptions model = model_invocation_options()) const;

  ScopedTestDirectory root;
  std::filesystem::path workspace;
  ava::session::SessionStore store;
  ava::provider::OpenAIProvider provider{"https://api.example.test"};
  ava::agent::Mode mode;
};

// Returns committed v4 function call ids in persisted turn/item order.
[[nodiscard]] std::vector<std::string> committed_function_call_ids(std::vector<ava::session::SessionEntry> const& entries);
// Returns bound ToolResult call ids in persistence order, retaining empty/missing
// ids so malformed entries cannot disappear from an equality assertion.
[[nodiscard]] std::vector<std::string> bound_tool_result_call_ids(std::vector<ava::session::SessionEntry> const& entries);

// Polls public coordinator publication with an adjustable watchdog and no busy
// spin. Returns the last snapshot on timeout; callers must assert the expected
// count and Running state. Provider-request readiness alone is not publication.
[[nodiscard]] std::vector<ava::agent::SubagentCoordinatorJobSnapshot> wait_for_published_running_job(
    ava::agent::SubagentCoordinator const& coordinator, std::string const& parent_session_id, std::size_t expected_total_count,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Trace callbacks and exact reads share one mutex/counter so event ordering never
// depends on elapsed time. Snapshot the witness after the observed work settles.
struct ChildSchedulingEvent
{
  enum class Kind
  {
    RunStart,
    ToolStart,
    FileRead
  };

  Kind kind = Kind::RunStart;
  std::string session_id = {};
  std::string parent_session_id = {};
  std::string tool_name = {};
  std::string file_name = {};
  std::thread::id thread = {};
  std::uint64_t order = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class ChildSchedulingWitness
{
 public:
  // Records only run-start/tool-start trace events, preserving correlation.
  void record_trace(ava::observability::TraceEvent const& event);
  // Records the file's basename and the actual executor thread.
  void record_file_read(std::filesystem::path const& absolute_path);
  // Returns an owned, mutex-consistent observation snapshot.
  [[nodiscard]] std::vector<ChildSchedulingEvent> snapshot() const;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  mutable std::mutex mutex_;
  std::vector<ChildSchedulingEvent> events_;
  std::uint64_t next_order_ = 0;
};

// Shares the witness with exact-file adapters while observer callbacks are live.
class ChildSchedulingObserver final : public ava::observability::RunObserver
{
 public:
  explicit ChildSchedulingObserver(std::shared_ptr<ChildSchedulingWitness> witness);
  void on_event(ava::observability::TraceEvent const& event) override;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::shared_ptr<ChildSchedulingWitness> witness_;
};

// Read-only real-file adapter that shares its witness with the trace observer.
// It records scheduling, not maximum concurrent reads or controlled admission.
class ChildSchedulingExactFileAccess final : public ava::tools::ExactFileAccess
{
 public:
  explicit ChildSchedulingExactFileAccess(std::shared_ptr<ChildSchedulingWitness> witness);
  [[nodiscard]] bool supports_read_text_file() const noexcept override { return true; }
  [[nodiscard]] bool supports_write_text_file() const noexcept override { return false; }
  [[nodiscard]] ava::core::Result<std::string> read_text_file(std::filesystem::path const& absolute_path,
                                                              ava::tools::ToolIoCancelCallback cancel_requested) const override;
  [[nodiscard]] ava::core::VoidResult write_text_file(std::filesystem::path const&, std::string_view,
                                                      ava::tools::ToolIoCancelCallback cancel_requested) const override;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::shared_ptr<ChildSchedulingWitness> witness_;
};

class SharedFakeTransport final : public ava::http::Transport
{
 public:
  SharedFakeTransport(std::shared_ptr<std::vector<ava::http::HttpResponse>> responses, std::shared_ptr<std::vector<ava::http::HttpRequest>> requests,
                      std::shared_ptr<std::mutex> mutex);

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override;

 private:
  std::shared_ptr<std::vector<ava::http::HttpResponse>> responses_;
  std::shared_ptr<std::vector<ava::http::HttpRequest>> requests_;
  std::shared_ptr<std::mutex> mutex_;
};

class BlockingSequenceTransport final : public ava::http::Transport
{
 public:
  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t blocked_request_index = 0;
    bool release = false;
    bool cancel_observed = false;
    std::vector<ava::http::HttpRequest> requests;

    void release_success();
    // Readiness watchdogs honor AVA_DEBUG_NO_TIMEOUT and its seconds override.
    [[nodiscard]] bool wait_for_requests(std::size_t count, std::chrono::milliseconds timeout);
    [[nodiscard]] bool wait_for_cancel(std::chrono::milliseconds timeout);
    [[nodiscard]] std::vector<ava::http::HttpRequest> requests_snapshot();
  };

  BlockingSequenceTransport(std::shared_ptr<State> state, std::vector<ava::http::HttpResponse> responses);

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override;
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request, CancelCallback cancel_requested) override;

 private:
  std::shared_ptr<State> state_;
  std::vector<ava::http::HttpResponse> responses_;
};

class BlockingBackgroundTransport final : public ava::http::Transport
{
 public:
  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool request_seen = false;
    bool release = false;
    bool cancel_observed = false;
    std::vector<ava::http::HttpRequest> requests;

    void release_success();
    void notify();
    // Readiness watchdogs honor AVA_DEBUG_NO_TIMEOUT and its seconds override.
    [[nodiscard]] bool wait_for_request(std::chrono::milliseconds timeout);
    [[nodiscard]] bool wait_for_cancel(std::chrono::milliseconds timeout);
    [[nodiscard]] std::vector<ava::http::HttpRequest> requests_snapshot();
  };

  BlockingBackgroundTransport(std::shared_ptr<State> state, ava::http::HttpResponse response);

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override;
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request, CancelCallback cancel_requested) override;

 private:
  std::shared_ptr<State> state_;
  ava::http::HttpResponse response_;
};

}  // namespace agent_loop_test
