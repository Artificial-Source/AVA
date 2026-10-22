#include "sys.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
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
  request.headers = {{"Accept", "application/json"}, {"X-Test", "header-value"}, {"Authorization", "Bearer ARGV_SECRET_CANARY"}};
  request.body = "fixed request body";
  request.follow_redirects = true;
  request.include_response_headers = true;
  request.resolve_hosts = {"curl.test:443:127.0.0.1"};
  auto response = fixture.transport.send(request);
  fixture.capture_snapshot();
  auto const* record = fixture.only_record();
  if (!ava::tests::require(response.has_value(), "curl fixed-argv fixture returns a response"))
    return;
  ava::tests::expect_all("curl preserves config/body/status while credentials never enter the fixed argv",
                         {{"HTTP status", response->status_code == 200},
                          {"response header", response->headers["X-Fake"] == "yes"},
                          {"fixed argv accepted", response->body.starts_with("argv=fixed\nstreaming=false\nconfig-begin\n")},
                          {"method in config", response->body.find("request = \"PUT\"") != std::string::npos},
                          {"test header in config", response->body.find("header = \"X-Test: header-value\"") != std::string::npos},
                          {"authorization in config", response->body.find("header = \"Authorization: Bearer ARGV_SECRET_CANARY\"") != std::string::npos},
                          {"resolve in config", response->body.find("resolve = \"curl.test:443:127.0.0.1\"") != std::string::npos},
                          {"exact request body", response->body.ends_with("config-end\nbody=fixed request body")}});
  ava::tests::expect_all("successful curl settles once and accounts raw output",
                         {{"natural settlement", settled_record(record, ava::process::TerminationReasonV1::NaturalExit)},
                          {"raw stdout accounted", record && record->stdout_bytes > response->body.size()},
                          {"stdout retained", record && !record->stdout_truncated},
                          {"stderr retained", record && !record->stderr_truncated},
                          {"no live records", fixture.snapshot.live_records == 0}});
}

void test_exact_environment_capture()
{
  ScopedEnvVar environment_166("HTTPS_PROXY", "https://captured-proxy.invalid");
  ScopedEnvVar environment_167("CURL_CA_BUNDLE", "/captured/ca.pem");
  ScopedEnvVar environment_168("OPENAI_API_KEY", "PROVIDER_SECRET_CANARY");
  ScopedEnvVar environment_169("AWS_SECRET_ACCESS_KEY", "CLOUD_SECRET_CANARY");
  ScopedEnvVar environment_170("LD_PRELOAD", "/loader/canary.so");
  ScopedEnvVar environment_171("GIT_ASKPASS", "/askpass/canary");
  ScopedEnvVar environment_172("SSH_AUTH_SOCK", "/agent/canary");
  ScopedEnvVar environment_173("AVA_CURL_FORBIDDEN_CANARY", "ARBITRARY_AVA_CANARY");

  CurlFixture fixture;
  ScopedEnvVar environment_176("HTTPS_PROXY", "https://mutated-proxy.invalid");
  ScopedEnvVar environment_177("CURL_CA_BUNDLE", "/mutated/ca.pem");
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
    auto const gate_root = ScopedTestDirectory::create_unique("/tmp", "ava-curl-stream-gate-");
    auto const token = gate_root.path().filename().string().substr(std::string_view("ava-curl-stream-gate-").size()) + "/release";
    auto const gate = gate_root / "release";
    bool opened_gate = false;
    auto response = fixture.transport.send_streaming(request_for("/stream-gate-" + token, 5000), [&](std::string_view) -> ava::core::VoidResult {
      ava::tests::write_test_file(gate, "");
      opened_gate = true;
      return {};
    });
    expect(response && opened_gate && response->body == "incremental accepted body before gate|after gate",
           "streaming curl publishes accepted bytes before the child can pass its deterministic completion gate");
  }
  for (auto const& path : {"/stream-header-limit", "/stream-output-limit"})
  {
    CurlFixture fixture;
    auto response = fixture.transport.send_streaming(request_for(path, 60000), [](std::string_view) -> ava::core::VoidResult { return {}; });
    fixture.capture_snapshot();
    expect(!response && response.error().message().find("exceeded byte limit") != std::string::npos &&
               (response.error().code() == ava::core::ErrorCode::BodyOutputLimit) == (path == std::string_view("/stream-output-limit")) &&
               settled_record(fixture.only_record(), path == std::string_view("/stream-output-limit") ? ava::process::TerminationReasonV1::OutputLimit
                                                                                                      : ava::process::TerminationReasonV1::ProtocolFailure),
           "streaming curl independently enforces bounded cumulative headers and the existing body cap");
  }
  {
    CurlFixture fixture;
    std::size_t sink_bytes = 0;
    auto response = fixture.transport.send_streaming(
        request_for("/stream-error-cancel", 60000),
        [&sink_bytes](std::string_view chunk) -> ava::core::VoidResult {
          sink_bytes += chunk.size();
          return {};
        },
        [&fixture] {
          auto const snapshot = fixture.supervisor->snapshot();
          return snapshot.records.size() == 1 && snapshot.records.front().stdout_bytes > 0;
        });
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
    auto const root = ScopedTestDirectory::create_unique(temp_root(), "curl-missing-temp-");
    auto const missing_temp = root / "absent";
    ScopedEnvVar missing_tmpdir("TMPDIR", missing_temp.string());
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
    auto response = fixture.transport.send(request_for("/output-limit", 60000));
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    ava::tests::expect_all("curl stdout hard cap accounts bytes and settles OutputLimit",
                           {{"body output limit", !response && response.error().code() == ava::core::ErrorCode::BodyOutputLimit},
                            {"settled", settled_record(record, ava::process::TerminationReasonV1::OutputLimit)},
                            {"truncated", record && record->stdout_truncated},
                            {"limit exceeded", record && record->stdout_bytes > 8U * 1024U * 1024U}});
  }
  {
    CurlFixture fixture;
    auto response = fixture.transport.send(request_for("/progress-timeout", 2500));
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    ava::tests::expect_all("curl expires its absolute request deadline after observing progress but before the stream completes",
                           {{"deadline error", !response && response.error().category() == ava::core::ErrorCategory::Provider &&
                                                   response.error().message() == "curl transport deadline expired"},
                            {"deadline settlement", settled_record(record, ava::process::TerminationReasonV1::DeadlineExpired)},
                            {"progress observed", record && record->stdout_bytes > 0},
                            {"stream interrupted", record && record->stdout_bytes < 500}});
  }
  {
    CurlFixture fixture;
    auto response = fixture.transport.send_streaming(request_for("/stream", 60000), [](std::string_view) -> ava::core::VoidResult {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "original sink failure"));
    });
    fixture.capture_snapshot();
    expect(!response && response.error().category() == ava::core::ErrorCategory::Tool && response.error().message() == "original sink failure" &&
               settled_record(fixture.only_record(), ava::process::TerminationReasonV1::ProtocolFailure),
           "stream sink failure performs complete supervised cleanup and returns the original error");
  }
  {
    CurlFixture fixture;
    unsigned callback_calls = 0;
    std::optional<ava::core::Result<ava::http::HttpResponse>> response;
    bool callback_unwound = false;
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
    fixture.capture_snapshot();
    auto const formatted = response && !*response ? response->error().format() : std::string{};
    expect(!callback_unwound && response && !*response && response->error().message() == "curl cancellation callback failed" && callback_calls == 4 &&
               formatted.find("POSTSPAWN_CANCEL_CALLBACK_SECRET") == std::string::npos &&
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

void test_streaming_deadline_before_eof()
{
  for (bool const reject_chunk : {false, true})
  {
    CurlFixture fixture;
    auto const gate_root = ScopedTestDirectory::create_unique("/tmp", "ava-curl-stream-gate-");
    auto const token = gate_root.path().filename().string().substr(std::string_view("ava-curl-stream-gate-").size()) + "/release";
    auto const gate = gate_root / "release";
    expect(!std::filesystem::exists(gate), "deadline fixture owns an absent completion gate");
    bool gate_entered = false;
    bool observed_deadline = false;
    std::size_t sink_bytes = 0;
    auto const observation_deadline = ava::tests::now_plus_seconds(10);
    auto response = fixture.transport.send_streaming(request_for("/stream-gate-" + token, 2500), [&](std::string_view chunk) -> ava::core::VoidResult {
      gate_entered = true;
      sink_bytes += chunk.size();
      // Hold the caller after real body progress until the independent monitor
      // finishes. A successful sink then reaches deadline-caused EOF before try_wait.
      while (std::chrono::steady_clock::now() < observation_deadline)
      {
        auto const cycle = ava::process::testing::SupervisorTestAccess::monitor_cycle(*fixture.supervisor);
        auto const snapshot = fixture.supervisor->snapshot();
        auto const* record = snapshot.records.size() == 1 ? &snapshot.records.front() : nullptr;
        if (record != nullptr && record->state == ava::process::ProcessStateV1::Finished)
        {
          observed_deadline = settled_record(record, ava::process::TerminationReasonV1::DeadlineExpired) && snapshot.live_records == 0;
          break;
        }
        if (!ava::process::testing::SupervisorTestAccess::wait_for_monitor_cycle(*fixture.supervisor, cycle, observation_deadline))
          break;
      }
      if (reject_chunk)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "original sink failure"));
      return {};
    });
    fixture.capture_snapshot();
    expect(gate_entered && sink_bytes > 0, "deadline EOF regression enters its sink gate with real body progress");
    expect(observed_deadline, "sink gate observes the independent monitor's complete deadline settlement before releasing the callback");
    if (reject_chunk)
    {
      expect(!response && response.error().category() == ava::core::ErrorCategory::Tool && response.error().code() == ava::core::ErrorCode::Unspecified &&
                 response.error().message() == "original sink failure",
             "a sink error returned after committed deadline settlement remains the original error");
    }
    else
    {
      expect(!response && response.error().category() == ava::core::ErrorCategory::Provider && response.error().code() == ava::core::ErrorCode::Unspecified &&
                 response.error().message() == "curl transport deadline expired",
             "deadline-caused EOF cannot replace the committed deadline with a missing status trailer error");
    }
    auto const* record = fixture.only_record();
    expect(settled_record(record, ava::process::TerminationReasonV1::DeadlineExpired) && record->stdout_bytes >= sink_bytes && record->stderr_bytes > 0 &&
               fixture.snapshot.live_records == 0,
           "deadline EOF regression retains raw progress and one complete Curl settlement with no live records");
    expect(no_waitable_immediate_child(), "deadline EOF regression leaves no waitable immediate child");
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
    auto response = fixture.transport.send(request_for("/term-refusal", 60000), [&fixture] {
      auto const snapshot = fixture.supervisor->snapshot();
      return snapshot.records.size() == 1 && snapshot.records.front().stdout_bytes > 0;
    });
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    ava::tests::expect_all("curl cancellation after TERM-refusal readiness escalates to SIGKILL and reaps the child",
                           {{"canceled error", !response && response.error().code() == ava::core::ErrorCode::Canceled},
                            {"ready bytes observed", record && record->stdout_bytes > 0},
                            {"canceled settlement", settled_record(record, ava::process::TerminationReasonV1::Canceled)},
                            {"SIGKILL observed", record && record->has_signal_number && record->signal_number == SIGKILL},
                            {"no waitable child", no_waitable_immediate_child()}});
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
    auto const root = ScopedTestDirectory::create_unique(temp_root(), "curl-exec-failure-");
    auto const executable = root / "malformed-helper";
    ava::tests::write_test_file(executable, "not an executable format\n", 0700);
    CurlFixture fixture;
    auto installed = ava::http::testing::CurlTransportTestAccess::set_executable(fixture.transport, executable);
    auto response =
        installed ? fixture.transport.send(request_for("/success")) : ava::core::Result<ava::http::HttpResponse>(std::unexpected(installed.error()));
    fixture.capture_snapshot();
    auto const* record = fixture.only_record();
    ava::tests::expect_all("curl exec failure is generic, supervised, completely cleaned, and never retried",
                           {{"helper installed", installed.has_value()},
                            {"generic transport rejection", !response && response.error().message() == "curl transport failed"},
                            {"ExecFailed", record && record->reason == ava::process::TerminationReasonV1::ExecFailed},
                            {"finished", record && record->state == ava::process::ProcessStateV1::Finished},
                            {"complete cleanup", record && record->cleanup == ava::process::CleanupStateV1::Complete},
                            {"one settlement", record && record->settlement_count == 1},
                            {"no live records", fixture.snapshot.live_records == 0}});
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

void test_binary_response_bytes()
{
  CurlFixture fixture;
  auto request = request_for("/success");
  request.body = std::string("binary\0\xff\xc0\x80\r\npayload", 19);
  auto response = fixture.transport.send(request);
  if (!ava::tests::require(response.has_value(), "binary curl response succeeds"))
    return;
  auto const body_offset = response->body.find("config-end\nbody=");
  if (!ava::tests::require(body_offset != std::string::npos, "binary response contains the fixture body boundary"))
    return;
  ava::tests::expect_eq(response->body.substr(body_offset + std::string_view("config-end\nbody=").size()), request.body,
                        "curl preserves embedded NUL and invalid UTF-8 response bytes");
  fixture.capture_snapshot();
  expect(settled_record(fixture.only_record(), ava::process::TerminationReasonV1::NaturalExit), "binary response settles its curl child");
}

}  // namespace

void run_curl_transport_process_tests()
{
  ava::tests::HermeticTestEnvironment environment("curl-process-");
  expect(std::string_view(AVA_FAKE_CURL_CHILD_PATH).starts_with('/'), "repository-owned fake curl child has an absolute test path");
  test_exact_argv_config_body_and_success();
  test_binary_response_bytes();
  test_exact_environment_capture();
  test_stream_separation_hup_and_callbacks();
  test_streaming_retry_with_nonempty_error_body();
  test_streaming_error_bodies_and_retry_exhaustion();
  test_streaming_sink_failure_stops_retry_and_cleans_up();
  test_streaming_response_framing();
  test_streaming_incremental_gate_limits_and_cancellation();
  test_prelaunch_checkpoints_and_empty_body();
  test_stop_reasons_and_limits();
  test_streaming_deadline_before_eof();
  test_group_cleanup_and_stderr_truncation();
  test_natural_failures_exec_failure_and_redaction();
}
