#pragma once
#include "ava/http/transport.h"
#include "ava/observability/run_observer.h"
#include "ava/agent/model_invocation_options.h"
#include "ava/provider/provider.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
