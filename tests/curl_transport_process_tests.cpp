#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/http/curl_transport.h"
#include "ava/http/curl_transport_test_support.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/process/supervisor_test_support.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AVA_FAKE_CURL_CHILD_PATH
#define AVA_FAKE_CURL_CHILD_PATH ""
#endif

namespace {

using namespace std::chrono_literals;

ava::process::ProcessScopeV1 require_application_scope(std::shared_ptr<ava::process::Supervisor> const& supervisor)
{
  auto scope = ava::process::ProcessScopeV1::application(supervisor);
  if (!scope)
    throw std::runtime_error(scope.error().format());
  return *scope;
}

class CurlFixture final
{
 public:
  CurlFixture() : supervisor(std::make_shared<ava::process::Supervisor>()), scope(require_application_scope(supervisor)), transport(scope)
  {
    auto installed = ava::http::testing::CurlTransportTestAccess::set_executable(transport, AVA_FAKE_CURL_CHILD_PATH);
    if (!installed)
      throw std::runtime_error(installed.error().format());
  }

  [[nodiscard]] ava::process::ProcessSnapshotRecordV1 const* only_record() const
  {
    auto const& records = snapshot.records;
    if (records.size() != 1)
      return nullptr;
    return &records.front();
  }

  void capture_snapshot() { snapshot = supervisor->snapshot(); }

  std::shared_ptr<ava::process::Supervisor> supervisor;
  ava::process::ProcessScopeV1 scope;
  ava::http::CurlCliTransport transport;
  ava::process::ProcessSnapshotV1 snapshot;
};

class SequencedCurlTransport final : public ava::http::Transport
{
 public:
  SequencedCurlTransport(ava::process::ProcessScopeV1 scope, std::vector<std::string> paths) : transport_(std::move(scope)), paths_(paths.begin(), paths.end())
  {
    auto installed = ava::http::testing::CurlTransportTestAccess::set_executable(transport_, AVA_FAKE_CURL_CHILD_PATH);
    if (!installed)
      throw std::runtime_error(installed.error().format());
  }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override { return transport_.send(request); }

  [[nodiscard]] bool supports_streaming() const noexcept override { return true; }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send_streaming(ava::http::HttpRequest const& request, BodyChunkSink on_body_chunk,
                                                                          CancelCallback cancel_requested) override
  {
    if (paths_.empty())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing fake curl response"));
    auto sequenced = request;
    sequenced.url = "https://curl.test" + paths_.front();
    paths_.pop_front();
    ++attempts_;
    return transport_.send_streaming(sequenced, std::move(on_body_chunk), std::move(cancel_requested));
  }

  [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }

 private:
  ava::http::CurlCliTransport transport_;
  std::deque<std::string> paths_;
  std::size_t attempts_ = 0;
};

class EnvironmentRestore final
{
 public:
  explicit EnvironmentRestore(std::vector<std::string> names)
  {
    for (auto& name : names)
    {
      auto const* value = std::getenv(name.c_str());
      values_.push_back({std::move(name), value == nullptr ? std::nullopt : std::optional<std::string>(value)});
    }
  }
  ~EnvironmentRestore()
  {
    for (auto const& [name, value] : values_)
    {
      if (value)
        static_cast<void>(::setenv(name.c_str(), value->c_str(), 1));
      else
        static_cast<void>(::unsetenv(name.c_str()));
    }
  }

 private:
  std::vector<std::pair<std::string, std::optional<std::string>>> values_;
};

ava::http::HttpRequest request_for(std::string_view path, int timeout_ms = 3000)
{
  return {.method = "GET",
          .url = "https://curl.test" + std::string(path),
          .headers = {},
          .body = {},
          .timeout_ms = timeout_ms,
          .follow_redirects = false,
          .include_response_headers = false,
          .resolve_hosts = {}};
}

bool settled_record(ava::process::ProcessSnapshotRecordV1 const* record, ava::process::TerminationReasonV1 reason)
{
  return record != nullptr && record->role == ava::process::ProcessRoleV1::Curl && record->state == ava::process::ProcessStateV1::Finished &&
         record->reason == reason && record->cleanup == ava::process::CleanupStateV1::Complete && record->settlement_count == 1;
}

bool no_waitable_immediate_child()
{
  errno = 0;
  int status = 0;
  auto const child = ::waitpid(-1, &status, WNOHANG);
  return child == -1 && errno == ECHILD;
}

void test_exact_argv_config_body_and_success()
{
  CurlFixture fixture;
  auto request = request_for("/success");
  request.method = "PUT";
  request.headers = {{"Accept", "application/json"}, {"X-Test", "header-value"}};
  request.body = "fixed request body";
  request.follow_redirects = true;
  request.include_response_headers = true;
  request.resolve_hosts = {"curl.test:443:127.0.0.1"};
  auto response = fixture.transport.send(request);
  fixture.capture_snapshot();
  auto const* record = fixture.only_record();
  expect(response && response->status_code == 200 && response->headers["X-Fake"] == "yes" && response->body.find("argv=fixed") != std::string::npos &&
             response->body.find("streaming=false") != std::string::npos && response->body.find("request = \"PUT\"") != std::string::npos &&
             response->body.find("header = \"X-Test: header-value\"") != std::string::npos &&
             response->body.find("resolve = \"curl.test:443:127.0.0.1\"") != std::string::npos &&
             response->body.find("body=fixed request body") != std::string::npos,
         "supervised curl preserves fixed argv and parent-generated config/body/status parsing");
  expect(settled_record(record, ava::process::TerminationReasonV1::NaturalExit) && record->stdout_bytes > response->body.size() && !record->stdout_truncated &&
             !record->stderr_truncated && fixture.snapshot.live_records == 0,
         "successful curl request has one exactly-once finished managed-group record with raw output accounting");
}

void test_exact_environment_capture()
{
  EnvironmentRestore restore(
      {"HTTPS_PROXY", "CURL_CA_BUNDLE", "OPENAI_API_KEY", "AWS_SECRET_ACCESS_KEY", "LD_PRELOAD", "GIT_ASKPASS", "SSH_AUTH_SOCK", "AVA_CURL_FORBIDDEN_CANARY"});
  static_cast<void>(::setenv("HTTPS_PROXY", "https://captured-proxy.invalid", 1));
  static_cast<void>(::setenv("CURL_CA_BUNDLE", "/captured/ca.pem", 1));
  static_cast<void>(::setenv("OPENAI_API_KEY", "PROVIDER_SECRET_CANARY", 1));
  static_cast<void>(::setenv("AWS_SECRET_ACCESS_KEY", "CLOUD_SECRET_CANARY", 1));
  static_cast<void>(::setenv("LD_PRELOAD", "/loader/canary.so", 1));
  static_cast<void>(::setenv("GIT_ASKPASS", "/askpass/canary", 1));
  static_cast<void>(::setenv("SSH_AUTH_SOCK", "/agent/canary", 1));
  static_cast<void>(::setenv("AVA_CURL_FORBIDDEN_CANARY", "ARBITRARY_AVA_CANARY", 1));

  CurlFixture fixture;
  static_cast<void>(::setenv("HTTPS_PROXY", "https://mutated-proxy.invalid", 1));
  static_cast<void>(::setenv("CURL_CA_BUNDLE", "/mutated/ca.pem", 1));
  auto response = fixture.transport.send(request_for("/environment"));
  fixture.capture_snapshot();
  expect(response && response->body.find("HTTPS_PROXY=https://captured-proxy.invalid") != std::string::npos &&
             response->body.find("CURL_CA_BUNDLE=/captured/ca.pem") != std::string::npos && response->body.find("mutated-proxy") == std::string::npos &&
             response->body.find("/mutated/ca.pem") == std::string::npos && response->body.find("OPENAI_API_KEY") == std::string::npos &&
             response->body.find("AWS_SECRET_ACCESS_KEY") == std::string::npos && response->body.find("LD_PRELOAD") == std::string::npos &&
             response->body.find("GIT_ASKPASS") == std::string::npos && response->body.find("SSH_AUTH_SOCK") == std::string::npos &&
             response->body.find("AVA_CURL_FORBIDDEN_CANARY") == std::string::npos,
         "ava-curl-v1 inherits only captured proxy/CA inputs and excludes provider, cloud, loader, askpass, agent, and arbitrary AVA values");
  expect(settled_record(fixture.only_record(), ava::process::TerminationReasonV1::NaturalExit),
         "environment inspection request remains supervised and exactly once settled");
}

void test_stream_separation_hup_and_callbacks()
{
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/separate"));
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    expect(response && response->status_code == 201 && response->body == "fake stdout body" && response->body.find("STDERR") == std::string::npos,
           "nonstream curl parses stdout independently and never contaminates the body with stderr");
    expect(settled_record(record, ava::process::TerminationReasonV1::NaturalExit) && record->stderr_bytes == std::string_view("FAKE_CURL_STDERR_CANARY").size(),
           "separate stderr is fully drained and accounted without exposing content");
  }
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/buffered-hup"));
    fixture.capture_snapshot();
    expect(response && response->body == std::string(32U * 1024U, 'h') && settled_record(fixture.only_record(), ava::process::TerminationReasonV1::NaturalExit),
           "buffered stdout is drained through HUP/EOF before successful settlement");
  }
  {
    CurlFixture fixture;
    std::vector<std::string> chunks;
    auto response = fixture.transport.send_streaming(request_for("/stream"), [&](std::string_view chunk) -> ava::core::VoidResult {
      chunks.emplace_back(chunk);
      return {};
    });
    fixture.capture_snapshot();
    std::string delivered;
    for (auto const& chunk : chunks)
      delivered += chunk;
    expect(response && response->status_code == 206 && response->body == "chunk-one|chunk-two" && delivered == response->body &&
               delivered.find("AVA_HTTP_STATUS") == std::string::npos,
           "streaming curl preserves callback order while withholding a split status marker");
    expect(settled_record(fixture.only_record(), ava::process::TerminationReasonV1::NaturalExit),
           "streaming completion waits for both output streams and exact process settlement");
  }
}

void test_streaming_retry_with_nonempty_error_body()
{
  for (auto const status : {429, 503})
  {
    auto supervisor = std::make_shared<ava::process::Supervisor>();
    SequencedCurlTransport inner(require_application_scope(supervisor), {status == 429 ? "/retry-429" : "/retry-503", "/retry-success"});
    std::vector<ava::http::RetryOptions::Event> retry_events;
    ava::http::RetryTransport retry(inner, ava::http::RetryOptions{.max_attempts = 2,
                                                                   .base_delay_ms = status == 429 ? 50 : 0,
                                                                   .on_retry =
                                                                       [&retry_events](ava::http::RetryOptions::Event const& event) {
                                                                         retry_events.push_back(event);
                                                                         return ava::core::VoidResult{};
                                                                       },
                                                                   .response_retry_decision =
                                                                       [](ava::http::HttpResponse const& response) {
                                                                         return response.status_code == 429   ? ava::http::ResponseRetryDecision::RateLimited
                                                                                : response.status_code == 503 ? ava::http::ResponseRetryDecision::Transient
                                                                                                              : ava::http::ResponseRetryDecision::NoRetry;
                                                                       }});
    std::string accepted;
    auto response = retry.send_streaming(request_for("/unused"), [&accepted](std::string_view chunk) -> ava::core::VoidResult {
      accepted.append(chunk);
      return {};
    });
    expect(response && response->status_code == 200 && response->body == "accepted response" && inner.attempts() == 2 && accepted == "accepted response" &&
               retry_events.size() == 1 && (status != 429 || retry_events.front().delay_ms == 0),
           "streaming curl withholds nonempty retryable errors, retains final Retry-After headers, and publishes only accepted response bytes");
  }
}

void test_streaming_error_bodies_and_retry_exhaustion()
{
  for (auto const& path : {"/retry-quota", "/retry-auth"})
  {
    auto supervisor = std::make_shared<ava::process::Supervisor>();
    SequencedCurlTransport inner(require_application_scope(supervisor), {path, "/retry-success"});
    ava::http::RetryTransport retry(
        inner, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0, .response_retry_decision = [](ava::http::HttpResponse const& response) {
                                         if (response.status_code == 429 && response.body.find("insufficient_quota") == std::string::npos)
                                           return ava::http::ResponseRetryDecision::RateLimited;
                                         return ava::http::ResponseRetryDecision::NoRetry;
                                       }});
    std::string accepted;
    auto response = retry.send_streaming(request_for("/unused"), [&accepted](std::string_view chunk) -> ava::core::VoidResult {
      accepted.append(chunk);
      return {};
    });
    expect(response && inner.attempts() == 1 && accepted.empty() &&
               ((response->status_code == 429 && response->body == "insufficient_quota: billing hard limit") ||
                (response->status_code == 401 && response->body == "authentication failed")),
           "streaming curl retains quota and authentication error bodies for classification without publishing them");
  }

  auto supervisor = std::make_shared<ava::process::Supervisor>();
  SequencedCurlTransport inner(require_application_scope(supervisor), {"/retry-429", "/retry-503"});
  ava::http::RetryTransport retry(
      inner, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0, .response_retry_decision = [](ava::http::HttpResponse const& response) {
                                       return response.status_code == 429 ? ava::http::ResponseRetryDecision::RateLimited
                                                                          : ava::http::ResponseRetryDecision::Transient;
                                     }});
  std::string accepted;
  auto response = retry.send_streaming(request_for("/unused"), [&accepted](std::string_view chunk) -> ava::core::VoidResult {
    accepted.append(chunk);
    return {};
  });
  expect(response && response->status_code == 503 && response->body == "transient response" && inner.attempts() == 2 && accepted.empty(),
         "exhausted streaming retries return the final error body without sink publication");

  auto cancel_supervisor = std::make_shared<ava::process::Supervisor>();
  SequencedCurlTransport cancel_inner(require_application_scope(cancel_supervisor), {"/retry-429", "/retry-success"});
  bool cancel_backoff = false;
  ava::http::RetryTransport cancel_retry(
      cancel_inner,
      ava::http::RetryOptions{.max_attempts = 2,
                              .base_delay_ms = 10,
                              .on_retry =
                                  [&cancel_backoff](ava::http::RetryOptions::Event const&) {
                                    cancel_backoff = true;
                                    return ava::core::VoidResult{};
                                  },
                              .cancel_requested = [&cancel_backoff] { return cancel_backoff; },
                              .response_retry_decision = [](ava::http::HttpResponse const&) { return ava::http::ResponseRetryDecision::RateLimited; }});
  auto canceled = cancel_retry.send_streaming(request_for("/unused"), [](std::string_view) -> ava::core::VoidResult { return {}; });
  expect(!canceled && canceled.error().message() == "transport retry canceled" && cancel_inner.attempts() == 1,
         "streaming retry cancellation interrupts backoff after a withheld nonempty error body");
}

void test_streaming_sink_failure_stops_retry_and_cleans_up()
{
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  SequencedCurlTransport inner(require_application_scope(supervisor), {"/stream", "/retry-success"});
  ava::http::RetryTransport retry(inner, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0});
  auto response = retry.send_streaming(request_for("/unused", 60000), [](std::string_view) -> ava::core::VoidResult {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "sink IO rejection"));
  });
  auto const snapshot = supervisor->snapshot();
  auto const* record = snapshot.records.size() == 1 ? &snapshot.records.front() : nullptr;
  expect(!response && response.error().message() == "sink IO rejection" && inner.attempts() == 1 &&
             settled_record(record, ava::process::TerminationReasonV1::ProtocolFailure),
         "streaming sink errors prevent retry and preserve supervised child cleanup");
}

void test_streaming_response_framing()
{
  {
    CurlFixture fixture;
    std::string accepted;
    auto response = fixture.transport.send_streaming(request_for("/stream-http-trailers"), [&accepted](std::string_view chunk) -> ava::core::VoidResult {
      accepted.append(chunk);
      return {};
    });
    expect(response && response->status_code == 200 && response->headers["Trailer"] == "X-Trace, data" && response->body == "expected body" &&
               accepted == "expected body" && response->body.find("malicious trailer event") == std::string::npos,
           "streaming curl never publishes HTTP chunked trailers emitted by curl as response metadata");
  }
  for (auto const& path : {"/stream-stderr-eof-first", "/stream-stdout-eof-first"})
  {
    CurlFixture fixture;
    std::string accepted;
    auto response = fixture.transport.send_streaming(request_for(path), [&accepted](std::string_view chunk) -> ava::core::VoidResult {
      accepted.append(chunk);
      return {};
    });
    auto const expected_body = path == std::string_view("/stream-stderr-eof-first") ? "headers-first body" : "body-pipe-first body";
    expect(response && response->status_code == 200 && response->body == expected_body && accepted == expected_body,
           "streaming curl gates body bytes until metadata across either stdout/stderr EOF ordering");
  }
  {
    CurlFixture fixture;
    std::string accepted;
    auto response = fixture.transport.send_streaming(request_for("/stream-missing-metadata"), [&accepted](std::string_view chunk) -> ava::core::VoidResult {
      accepted.append(chunk);
      return {};
    });
    expect(!response && response.error().message().find("final HTTP response head") != std::string::npos && accepted.empty(),
           "streaming curl fails closed without response metadata and never publishes pending body bytes");
  }
  {
    CurlFixture fixture;
    std::string accepted;
    auto response =
        fixture.transport.send_streaming(request_for("/stream-prefix-diagnostic-failure"), [&accepted](std::string_view chunk) -> ava::core::VoidResult {
          accepted.append(chunk);
          return {};
        });
    expect(!response && response.error().message() == "curl transport failed" && accepted.empty(),
           "streaming curl does not search diagnostics for a later HTTP prefix and preserves curl process failure classification");
  }
  {
    CurlFixture fixture;
    auto request = request_for("/stream-head-chain");
    request.follow_redirects = true;
    std::string accepted;
    auto response = fixture.transport.send_streaming(request, [&accepted](std::string_view chunk) -> ava::core::VoidResult {
      accepted.append(chunk);
      return {};
    });
    expect(response && response->status_code == 200 && response->headers["X-Final"] == "yes" && response->body == "chain body" && accepted == "chain body",
           "streaming curl skips split 100, 103, and followed arbitrary 3xx heads before the final response");
  }
  for (auto const& path : {"/stream-upgrade", "/stream-final-redirect", "/stream-disabled-redirect"})
  {
    CurlFixture fixture;
    auto request = request_for(path);
    request.follow_redirects = path == std::string_view("/stream-final-redirect");
    std::string accepted;
    auto response = fixture.transport.send_streaming(request, [&accepted](std::string_view chunk) -> ava::core::VoidResult {
      accepted.append(chunk);
      return {};
    });
    expect(response && (response->status_code == 101 || response->status_code == 399) && response->body.find("HTTP/1.1 200 Fake") != std::string::npos &&
               accepted.empty(),
           "101, final no-location, and redirect-disabled 3xx bodies are retained without fake HTTP body reparsing or sink publication");
  }
  for (auto const& path : {"/stream-missing-trailer", "/stream-malformed-trailer", "/stream-mismatched-trailer"})
  {
    CurlFixture fixture;
    auto response = fixture.transport.send_streaming(request_for(path), [](std::string_view) -> ava::core::VoidResult { return {}; });
    expect(!response && response.error().message().find("status") != std::string::npos,
           "streaming curl fails closed on missing, malformed, and mismatched status trailers");
  }
}

void test_streaming_incremental_gate_limits_and_cancellation()
{
  {
    CurlFixture fixture;
    auto const token = std::to_string(::getpid());
    auto const gate = "/tmp/ava-curl-stream-gate-" + token;
    static_cast<void>(::unlink(gate.c_str()));
    bool opened_gate = false;
    auto response = fixture.transport.send_streaming(request_for("/stream-gate-" + token, 5000), [&](std::string_view) -> ava::core::VoidResult {
      int const descriptor = ::open(gate.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (descriptor >= 0)
      {
        static_cast<void>(::close(descriptor));
        opened_gate = true;
      }
      return {};
    });
    static_cast<void>(::unlink(gate.c_str()));
    expect(response && opened_gate && response->body == "incremental accepted body before gate|after gate",
           "streaming curl publishes accepted bytes before the child can pass its deterministic completion gate");
  }
  for (auto const& path : {"/stream-header-limit", "/stream-output-limit"})
  {
    CurlFixture fixture;
    auto response = fixture.transport.send_streaming(request_for(path, 60000), [](std::string_view) -> ava::core::VoidResult { return {}; });
    fixture.capture_snapshot();
    expect(!response && response.error().message().find("exceeded byte limit") != std::string::npos &&
               settled_record(fixture.only_record(), path == std::string_view("/stream-output-limit") ? ava::process::TerminationReasonV1::OutputLimit
                                                                                                      : ava::process::TerminationReasonV1::ProtocolFailure),
           "streaming curl independently enforces bounded cumulative headers and the existing body cap");
  }
  {
    CurlFixture fixture;
    auto const started = std::chrono::steady_clock::now();
    std::size_t sink_bytes = 0;
    auto response = fixture.transport.send_streaming(
        request_for("/stream-error-cancel", 60000),
        [&sink_bytes](std::string_view chunk) -> ava::core::VoidResult {
          sink_bytes += chunk.size();
          return {};
        },
        [started] { return std::chrono::steady_clock::now() - started > 40ms; });
    fixture.capture_snapshot();
    expect(!response && response.error().message() == "transport request canceled" && sink_bytes == 0 &&
               settled_record(fixture.only_record(), ava::process::TerminationReasonV1::Canceled),
           "cancellation during a withheld error body performs supervised cleanup without sink publication");
  }
}

void test_prelaunch_checkpoints_and_empty_body()
{
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/cancel", 60000), [] { return true; });
    fixture.capture_snapshot();
    expect(!response && response.error().message() == "transport request canceled" && fixture.snapshot.records.empty() && fixture.snapshot.live_records == 0 &&
               !fixture.snapshot.monitor_started,
           "immediate curl cancellation returns before operation reservation and starts no process monitor");
  }
  {
    CurlFixture fixture;
    unsigned callback_calls = 0;
    auto response = fixture.transport.send(request_for("/cancel", 60000), [&callback_calls] { return ++callback_calls == 3; });
    fixture.capture_snapshot();
    expect(!response && response.error().message() == "transport request canceled" && callback_calls == 3 && fixture.snapshot.records.empty() &&
               fixture.snapshot.live_records == 0 && !fixture.snapshot.monitor_started,
           "curl cancellation immediately before spawn abandons its reservation without a process record or monitor");
  }
  {
    CurlFixture fixture;
    std::optional<ava::core::Result<ava::http::HttpResponse>> response;
    bool callback_unwound = false;
    try
    {
      response.emplace(fixture.transport.send(request_for("/cancel", 60000), []() -> bool { throw std::runtime_error("PRELAUNCH_CANCEL_CALLBACK_SECRET"); }));
    }
    catch (...)
    {
      callback_unwound = true;
    }
    fixture.capture_snapshot();
    auto const formatted = response && !*response ? response->error().format() : std::string{};
    expect(!callback_unwound && response && !*response && response->error().message() == "curl cancellation callback failed" &&
               formatted.find("PRELAUNCH_CANCEL_CALLBACK_SECRET") == std::string::npos && fixture.snapshot.records.empty() &&
               fixture.snapshot.live_records == 0 && !fixture.snapshot.monitor_started,
           "prelaunch cancellation callback exceptions become content-free errors without process authority use");
  }
  {
    CurlFixture fixture;
    auto request = request_for("/success", 1);
    request.body.assign(16U * 1024U * 1024U, 'd');
    auto response = fixture.transport.send(request);
    fixture.capture_snapshot();
    expect(!response && response.error().message() == "curl transport deadline expired" && fixture.snapshot.records.empty() &&
               fixture.snapshot.live_records == 0 && !fixture.snapshot.monitor_started,
           "deadline expiry during parent-side body preparation returns before reservation and spawn");
  }
  {
    EnvironmentRestore restore({"TMPDIR"});
    auto const missing_temp = "/tmp/ava-curl-missing-temp-" + std::to_string(::getpid());
    std::error_code remove_error;
    std::filesystem::remove_all(missing_temp, remove_error);
    static_cast<void>(::setenv("TMPDIR", missing_temp.c_str(), 1));
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/success"));
    fixture.capture_snapshot();
    expect(
        response && response->body.find("body=") != std::string::npos && settled_record(fixture.only_record(), ava::process::TerminationReasonV1::NaturalExit),
        "an empty curl request body preserves config behavior without requiring a temporary file");
  }
}

void test_stop_reasons_and_limits()
{
  {
    CurlFixture fixture;
    auto const started = std::chrono::steady_clock::now();
    auto response = fixture.transport.send(request_for("/output-limit", 60000));
    auto const elapsed = std::chrono::steady_clock::now() - started;
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    expect(!response && response.error().message() == "curl response exceeded byte limit" && elapsed < 5s &&
               settled_record(record, ava::process::TerminationReasonV1::OutputLimit) && record->stdout_truncated && record->stdout_bytes > 8U * 1024U * 1024U,
           "curl stdout hard cap starts cleanup at failure, remains below its elapsed ceiling, and accounts raw output");
  }
  {
    CurlFixture fixture;
    auto const started = std::chrono::steady_clock::now();
    auto response = fixture.transport.send(request_for("/progress-timeout", 120));
    auto const elapsed = std::chrono::steady_clock::now() - started;
    fixture.capture_snapshot();
    expect(!response && response.error().message() == "curl transport deadline expired" && elapsed < 1500ms &&
               settled_record(fixture.only_record(), ava::process::TerminationReasonV1::DeadlineExpired),
           "periodic curl progress never resets the one absolute request deadline");
  }
  {
    CurlFixture fixture;
    auto const started = std::chrono::steady_clock::now();
    auto response = fixture.transport.send_streaming(request_for("/stream", 60000), [](std::string_view) -> ava::core::VoidResult {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "original sink failure"));
    });
    auto const elapsed = std::chrono::steady_clock::now() - started;
    fixture.capture_snapshot();
    expect(!response && response.error().category() == ava::core::ErrorCategory::Tool && response.error().message() == "original sink failure" &&
               elapsed < 5s && settled_record(fixture.only_record(), ava::process::TerminationReasonV1::ProtocolFailure),
           "stream sink failure starts bounded cleanup at failure and returns the original error below its elapsed ceiling");
  }
  {
    CurlFixture fixture;
    unsigned callback_calls = 0;
    std::optional<ava::core::Result<ava::http::HttpResponse>> response;
    bool callback_unwound = false;
    auto const started = std::chrono::steady_clock::now();
    try
    {
      response.emplace(fixture.transport.send(request_for("/term-refusal", 60000), [&callback_calls]() -> bool {
        if (++callback_calls == 4)
          throw std::runtime_error("POSTSPAWN_CANCEL_CALLBACK_SECRET");
        return false;
      }));
    }
    catch (...)
    {
      callback_unwound = true;
    }
    auto const elapsed = std::chrono::steady_clock::now() - started;
    fixture.capture_snapshot();
    auto const formatted = response && !*response ? response->error().format() : std::string{};
    expect(!callback_unwound && response && !*response && response->error().message() == "curl cancellation callback failed" && callback_calls == 4 &&
               formatted.find("POSTSPAWN_CANCEL_CALLBACK_SECRET") == std::string::npos && elapsed < 5s &&
               settled_record(fixture.only_record(), ava::process::TerminationReasonV1::ProtocolFailure),
           "post-spawn cancellation callback exceptions request bounded ProtocolFailure cleanup without unwinding");
  }
  {
    CurlFixture fixture;
    auto request = request_for("/config-failure");
    request.headers.emplace("X-Large", std::string(256U * 1024U, 'c'));
    auto response = fixture.transport.send(request);
    fixture.capture_snapshot();
    expect(!response && response.error().message() == "failed to write curl configuration" &&
               settled_record(fixture.only_record(), ava::process::TerminationReasonV1::ProtocolFailure),
           "incremental config pipe failure uses supervised ProtocolFailure cleanup without retry");
  }
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/protocol"));
    fixture.capture_snapshot();
    expect(!response && response.error().message() == "curl response did not include an HTTP status" &&
               settled_record(fixture.only_record(), ava::process::TerminationReasonV1::ProtocolFailure),
           "missing curl status marker is a supervised protocol failure");
  }
}

void test_group_cleanup_and_stderr_truncation()
{
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/descendant"));
    fixture.capture_snapshot();
    expect(response && response->body == "descendant body" && settled_record(fixture.only_record(), ava::process::TerminationReasonV1::NaturalExit) &&
               no_waitable_immediate_child(),
           "a same-group descendant surviving its curl leader is killed before endpoint EOF and complete settlement");
  }
  {
    CurlFixture fixture;
    auto const started = std::chrono::steady_clock::now();
    auto response = fixture.transport.send(request_for("/term-refusal", 60000), [started] { return std::chrono::steady_clock::now() - started > 100ms; });
    auto const elapsed = std::chrono::steady_clock::now() - started;
    fixture.capture_snapshot();
    expect(!response && elapsed < 5s && settled_record(fixture.only_record(), ava::process::TerminationReasonV1::Canceled) && no_waitable_immediate_child(),
           "curl cancellation starts bounded cleanup at failure, escalates a TERM-refusing group, and leaves no waitable child");
  }
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/stderr-limit"));
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    expect(response && response->body == "stderr bounded" && settled_record(record, ava::process::TerminationReasonV1::NaturalExit) &&
               record->stderr_bytes == 72U * 1024U && record->stderr_truncated,
           "stream-independent stderr retention truncates at 64 KiB while draining and accounting all bytes");
  }
}

void test_natural_failures_exec_failure_and_redaction()
{
  constexpr std::string_view url_canary = "CURL_URL_SECRET_CANARY";
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/nonzero"));
    fixture.capture_snapshot();
    auto const formatted = response ? std::string{} : response.error().format();
    auto const* record = fixture.only_record();
    expect(!response && response.error().message() == "curl transport failed" && formatted.find("NONZERO_SECRET_CANARY") == std::string::npos &&
               formatted.find("exit_code: 7") != std::string::npos && settled_record(record, ava::process::TerminationReasonV1::NaturalExit) &&
               record->has_exit_code && record->exit_code == 7,
           "nonzero curl exits map to content-free generic metadata with no legacy retry");
  }
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/signal"));
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    expect(!response && response.error().message() == "curl transport failed" && settled_record(record, ava::process::TerminationReasonV1::NaturalExit) &&
               record->has_signal_number && record->settlement_count == 1,
           "naturally signaled curl exits remain one generic exactly-once record");
  }
  {
    CurlFixture fixture;
    ava::process::testing::SupervisorTestAccess::fail_next_common_child_working_directory(*fixture.supervisor);
    auto response = fixture.transport.send(request_for("/success"));
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    expect(!response && response.error().message() == "curl transport failed" && record != nullptr &&
               record->reason == ava::process::TerminationReasonV1::LaunchFailed && record->state == ava::process::ProcessStateV1::Finished &&
               record->cleanup == ava::process::CleanupStateV1::Complete && record->settlement_count == 1 && fixture.snapshot.live_records == 0,
           "curl post-fork launch failure is generic, completely cleaned, exactly once settled, and never retried");
  }
  {
    char path_template[] = "/tmp/ava-fake-curl-exec-failure-XXXXXX";
    int const descriptor = ::mkstemp(path_template);
    expect(descriptor >= 0, "exec-failure fixture is created");
    if (descriptor >= 0)
    {
      auto const written = ::write(descriptor, "not an executable format\n", 25);
      expect(written == 25, "exec-failure fixture content is written");
      static_cast<void>(::close(descriptor));
      static_cast<void>(::chmod(path_template, S_IRUSR | S_IWUSR | S_IXUSR));
      CurlFixture fixture;
      auto installed = ava::http::testing::CurlTransportTestAccess::set_executable(fixture.transport, path_template);
      auto response =
          installed ? fixture.transport.send(request_for("/success")) : ava::core::Result<ava::http::HttpResponse>(std::unexpected(installed.error()));
      fixture.capture_snapshot();
      auto const* record = fixture.only_record();
      expect(!response && response.error().message() == "curl transport failed" && record != nullptr &&
                 record->reason == ava::process::TerminationReasonV1::ExecFailed && record->state == ava::process::ProcessStateV1::Finished &&
                 record->cleanup == ava::process::CleanupStateV1::Complete && record->settlement_count == 1 && fixture.snapshot.live_records == 0,
             "curl exec failure is generic, supervised, completely cleaned, and never retried");
      static_cast<void>(::unlink(path_template));
    }
  }
  {
    CurlFixture fixture;
    auto request = request_for("/nonzero");
    request.url += std::string(url_canary);
    request.headers.emplace("X-Secret", "HEADER_SECRET_CANARY");
    request.body = "BODY_SECRET_CANARY";
    auto response = fixture.transport.send(request);
    fixture.capture_snapshot();
    auto const formatted = response ? std::string{} : response.error().format();
    expect(!response && formatted.find(url_canary) == std::string::npos && formatted.find("HEADER_SECRET_CANARY") == std::string::npos &&
               formatted.find("BODY_SECRET_CANARY") == std::string::npos && formatted.find("NONZERO_SECRET_CANARY") == std::string::npos,
           "curl errors omit URL, config, header, body, stdout, and stderr canaries");
  }
}

}  // namespace

void run_curl_transport_process_tests()
{
  expect(std::string_view(AVA_FAKE_CURL_CHILD_PATH).starts_with('/'), "repository-owned fake curl child has an absolute test path");
  test_exact_argv_config_body_and_success();
  test_exact_environment_capture();
  test_stream_separation_hup_and_callbacks();
  test_streaming_retry_with_nonempty_error_body();
  test_streaming_error_bodies_and_retry_exhaustion();
  test_streaming_sink_failure_stops_retry_and_cleans_up();
  test_streaming_response_framing();
  test_streaming_incremental_gate_limits_and_cancellation();
  test_prelaunch_checkpoints_and_empty_body();
  test_stop_reasons_and_limits();
  test_group_cleanup_and_stderr_truncation();
  test_natural_failures_exec_failure_and_redaction();
}
