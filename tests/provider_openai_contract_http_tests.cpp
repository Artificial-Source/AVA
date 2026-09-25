#include "sys.h"
#include "tests/provider_openai_test_suite.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/curl_transport.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/provider/openai_response_parser.h"
#include "ava/provider/provider.h"
#include "ava/core/error.h"
#include "ava/core/result.h"

#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tests::provider_openai_suite {

namespace {

class StreamingFakeTransport final : public ava::http::Transport
{
 public:
  explicit StreamingFakeTransport(std::vector<ava::http::HttpResponse> responses) : responses_(responses.begin(), responses.end()) { }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override { return send_streaming(request, nullptr); }

  [[nodiscard]] bool supports_streaming() const noexcept override { return true; }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send_streaming(ava::http::HttpRequest const& request, BodyChunkSink on_body_chunk,
                                                                          CancelCallback cancel_requested = nullptr) override
  {
    requests_.push_back(request);
    if (responses_.empty())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "fake transport has no response"));
    }
    auto response = responses_.front();
    responses_.pop_front();
    if (cancel_requested && cancel_requested())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
    }
    if (response.status_code >= 200 && response.status_code < 300 && on_body_chunk && !response.body.empty())
    {
      if (auto delivered = on_body_chunk(response.body); !delivered)
        return std::unexpected(std::move(delivered.error()));
    }
    return response;
  }

  [[nodiscard]] std::vector<ava::http::HttpRequest> const& requests() const noexcept { return requests_; }

 private:
  std::deque<ava::http::HttpResponse> responses_;
  std::vector<ava::http::HttpRequest> requests_;
};

class PartialStreamingFailureTransport final : public ava::http::Transport
{
 public:
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const&) override
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "streaming only"));
  }

  [[nodiscard]] bool supports_streaming() const noexcept override { return true; }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send_streaming(ava::http::HttpRequest const&, BodyChunkSink on_body_chunk, CancelCallback) override
  {
    ++attempts_;
    if (on_body_chunk)
    {
      if (auto delivered = on_body_chunk("accepted partial body"); !delivered)
        return std::unexpected(std::move(delivered.error()));
    }
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "temporary failure after body"));
  }

  [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }

 private:
  std::size_t attempts_ = 0;
};

class FailingOnceTransport final : public ava::http::Transport
{
 public:
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    requests_.push_back(request);
    if (requests_.size() == 1)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "temporary transport failure"));
    }
    return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"};
  }

  [[nodiscard]] std::vector<ava::http::HttpRequest> const& requests() const noexcept { return requests_; }

 private:
  std::vector<ava::http::HttpRequest> requests_;
};

class CancelDuringSendTransport final : public ava::http::Transport
{
 public:
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    requests_.push_back(request);
    return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"};
  }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request, CancelCallback cancel_requested) override
  {
    requests_.push_back(request);
    saw_cancel_callback_ = static_cast<bool>(cancel_requested);
    if (before_cancel_check_)
      before_cancel_check_();
    if (cancel_requested && cancel_requested())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled during callback"));
    }
    return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"};
  }

  void before_cancel_check(std::function<void()> callback) { before_cancel_check_ = std::move(callback); }
  [[nodiscard]] bool saw_cancel_callback() const noexcept { return saw_cancel_callback_; }
  [[nodiscard]] std::vector<ava::http::HttpRequest> const& requests() const noexcept { return requests_; }

 private:
  std::function<void()> before_cancel_check_;
  bool saw_cancel_callback_ = false;
  std::vector<ava::http::HttpRequest> requests_;
};

}  // namespace

void exercise_contract_http_retry(ava::provider::OpenAIProvider const&)
{
  auto http_error = ava::provider::parse_openai_sse_response(ava::http::HttpResponse{
      .status_code = 401,
      .headers = {{"Retry-After", "Bearer OPENAI_HTTP_HEADER_BEARER_CANARY"}},
      .body =
          R"({"error":{"message":"bad auth\ntry again","private_unknown":"OPENAI_HTTP_NESTED_CANARY","nested":{"reasoning":"OPENAI_HTTP_REASONING_CANARY"}},"unknown":"OPENAI_HTTP_OUTER_CANARY","Authorization":"Bearer OPENAI_HTTP_BEARER_CANARY"})"});
  expect(!http_error && http_error.error().category() == ava::core::ErrorCategory::Provider && http_error.error().message().find("401") != std::string::npos,
         "OpenAI auth response is normalized as a provider error with status context");
  if (!http_error)
  {
    auto const formatted = http_error.error().format();
    expect(formatted.find("provider_error_kind: authentication") != std::string::npos && formatted.find("provider_message") == std::string::npos &&
               formatted.find("body_snippet") == std::string::npos && formatted.find("OPENAI_HTTP_NESTED_CANARY") == std::string::npos &&
               formatted.find("OPENAI_HTTP_REASONING_CANARY") == std::string::npos && formatted.find("OPENAI_HTTP_OUTER_CANARY") == std::string::npos &&
               formatted.find("OPENAI_HTTP_BEARER_CANARY") == std::string::npos && formatted.find("OPENAI_HTTP_HEADER_BEARER_CANARY") == std::string::npos &&
               formatted.find("retry_after") == std::string::npos,
           "OpenAI non-200 response exposes only a bounded control-safe allowlisted provider message");
  }
  auto html_error =
      ava::provider::parse_openai_sse_response(ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "<html>OPENAI_HTTP_HTML_CANARY</html>"});
  if (!html_error)
  {
    auto const formatted = html_error.error().format();
    expect(formatted.find("OPENAI_HTTP_HTML_CANARY") == std::string::npos && formatted.find("<html>") == std::string::npos &&
               formatted.find("provider_message") == std::string::npos,
           "OpenAI HTML error bodies remain opaque and use only generic diagnostics");
  }
  auto curl_supervisor = std::make_shared<ava::process::Supervisor>();
  auto curl_scope = ava::process::ProcessScopeV1::application(curl_supervisor);
  expect(static_cast<bool>(curl_scope), "curl contract test creates explicit process authority");
  if (!curl_scope)
    return;
  ava::http::CurlCliTransport curl_transport(*curl_scope);
  ava::http::HttpRequest curl_failure_request{};
  curl_failure_request.method = "GET";
  curl_failure_request.url = "unsupported-provider-scheme://CURL_DIAGNOSTIC_PAYLOAD_CANARY";
  curl_failure_request.timeout_ms = 1000;
  auto curl_failure = curl_transport.send(curl_failure_request);
  auto curl_stream_failure = curl_transport.send_streaming(curl_failure_request, [](std::string_view) { return ava::core::VoidResult{}; });
  auto const curl_failure_text = curl_failure ? std::string{} : curl_failure.error().format();
  auto const curl_stream_failure_text = curl_stream_failure ? std::string{} : curl_stream_failure.error().format();
  expect(!curl_failure && !curl_stream_failure && curl_failure_text.find("CURL_DIAGNOSTIC_PAYLOAD_CANARY") == std::string::npos &&
             curl_stream_failure_text.find("CURL_DIAGNOSTIC_PAYLOAD_CANARY") == std::string::npos && curl_failure_text.find("output:") == std::string::npos &&
             curl_stream_failure_text.find("stderr:") == std::string::npos && curl_failure_text.find("exit_code:") != std::string::npos &&
             curl_stream_failure_text.find("exit_code:") != std::string::npos,
         "curl transport failures expose only framing metadata and never subprocess response or stderr contents");

  auto bearer_message_error = ava::provider::parse_openai_sse_response(ava::http::HttpResponse{
      .status_code = 400, .headers = {}, .body = R"({"message":"authorization failed for Bearer OPENAI_ALLOWED_MESSAGE_BEARER_CANARY"})"});
  auto const bearer_message_text = bearer_message_error ? std::string{} : bearer_message_error.error().format();
  expect(!bearer_message_error && bearer_message_text.find("provider_message") == std::string::npos &&
             bearer_message_text.find("OPENAI_ALLOWED_MESSAGE_BEARER_CANARY") == std::string::npos,
         "HTTP diagnostics omit arbitrary provider messages, including bearer-shaped values");
  auto rate_limit = ava::provider::parse_openai_sse_response(
      ava::http::HttpResponse{.status_code = 429, .headers = {{"Retry-After", "2"}}, .body = "{\"error\":\"rate limited\"}"});
  expect(!rate_limit && rate_limit.error().format().find("provider_error_kind: rate_limited") != std::string::npos &&
             rate_limit.error().format().find("retry_after: 2") != std::string::npos,
         "OpenAI rate-limit errors carry normalized kind and Retry-After context");
  expect(ava::provider::is_auth_status(401) && ava::provider::is_auth_status(403) && !ava::provider::is_auth_status(429),
         "OpenAI auth status helper classifies auth failures");
  expect(ava::provider::is_retryable_status(429) && ava::provider::is_retryable_status(500) && !ava::provider::is_retryable_status(401),
         "OpenAI retryable status helper classifies transient failures");
  expect(ava::provider::classify_provider_error(ava::http::HttpResponse{.status_code = 400, .headers = {}, .body = "Input token length too long"}) ==
             ava::provider::ProviderErrorKind::ContextOverflow,
         "provider error classifier recognizes Kimi input token overflow wording");
  expect(
      ava::provider::classify_provider_error(ava::http::HttpResponse{
          .status_code = 400, .headers = {}, .body = "Your request exceeded model token limit : 131072"}) == ava::provider::ProviderErrorKind::ContextOverflow,
      "provider error classifier recognizes Kimi combined model token overflow wording");
  expect(ava::provider::classify_provider_error(ava::http::HttpResponse{.status_code = 400, .headers = {}, .body = "exceeded model token limit"}) ==
             ava::provider::ProviderErrorKind::ContextOverflow,
         "provider error classifier recognizes short Kimi model token overflow wording");

  ava::tests::FakeTransport retry_inner({ava::http::HttpResponse{.status_code = 429, .headers = {{"Retry-After", "0"}}, .body = "rate limited"},
                                         ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  std::vector<ava::http::RetryOptions::Event> retry_events;
  ava::http::RetryTransport retry_transport(retry_inner, ava::http::RetryOptions{.max_attempts = 2,
                                                                                 .base_delay_ms = 0,
                                                                                 .max_retry_after_ms = 0,
                                                                                 .on_retry =
                                                                                     [&retry_events](ava::http::RetryOptions::Event const& event) {
                                                                                       retry_events.push_back(event);
                                                                                       return ava::core::VoidResult{};
                                                                                     },
                                                                                 .response_retry_decision = ava::provider::provider_retry_decision});
  auto const retry_request = ava::http::HttpRequest{.method = "POST",
                                                    .url = "https://api.example.test",
                                                    .headers = {},
                                                    .body = {},
                                                    .timeout_ms = 60000,
                                                    .follow_redirects = true,
                                                    .include_response_headers = false,
                                                    .resolve_hosts = {}};
  auto retried = retry_transport.send(retry_request);
  expect(retried && retried->status_code == 200 && retry_inner.requests().size() == 2, "retry transport retries rate-limited non-streaming responses");
  expect(retry_events.size() == 1 && retry_events[0].attempt == 2 && retry_events[0].max_attempts == 2 && retry_events[0].reason == "rate_limited" &&
             retry_events[0].status_code == 429 && !retry_events[0].streaming && !retry_events[0].countdown_tick,
         "retry transport reports backend-owned retry metadata before sleeping");

  ava::tests::FakeTransport countdown_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "try again"},
                                             ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  std::vector<ava::http::RetryOptions::Event> countdown_events;
  ava::http::RetryTransport countdown_transport(countdown_inner, ava::http::RetryOptions{.max_attempts = 2,
                                                                                         .base_delay_ms = 1,
                                                                                         .max_retry_after_ms = 1,
                                                                                         .countdown_tick_ms = 1,
                                                                                         .on_retry =
                                                                                             [&countdown_events](ava::http::RetryOptions::Event const& event) {
                                                                                               countdown_events.push_back(event);
                                                                                               return ava::core::VoidResult{};
                                                                                             },
                                                                                         .response_retry_decision = ava::provider::provider_retry_decision});
  auto countdown_retry = countdown_transport.send(retry_request);
  expect(countdown_retry && countdown_retry->status_code == 200 && countdown_inner.requests().size() == 2,
         "retry transport completes after a countdown-backed retry");
  expect(countdown_events.size() == 2 && !countdown_events[0].countdown_tick && countdown_events[0].delay_ms == 1 && countdown_events[0].remaining_ms == 1 &&
             countdown_events[1].countdown_tick && countdown_events[1].remaining_ms == 0 && countdown_events[1].reason == "transient",
         "retry transport emits explicit backend countdown ticks while waiting to retry");

  ava::tests::FakeTransport cancel_retry_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "try again"},
                                                ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  std::vector<ava::http::RetryOptions::Event> cancel_retry_events;
  ava::http::RetryTransport cancel_retry_transport(cancel_retry_inner, ava::http::RetryOptions{
                                                                           .max_attempts = 2,
                                                                           .base_delay_ms = 10,
                                                                           .max_retry_after_ms = 0,
                                                                           .countdown_tick_ms = 10,
                                                                           .on_retry =
                                                                               [&cancel_retry_events](ava::http::RetryOptions::Event const& event) {
                                                                                 cancel_retry_events.push_back(event);
                                                                                 return ava::core::VoidResult{};
                                                                               },
                                                                           .cancel_requested = [&cancel_retry_events] { return !cancel_retry_events.empty(); },
                                                                           .response_retry_decision = ava::provider::provider_retry_decision,
                                                                       });
  auto canceled_retry = cancel_retry_transport.send(retry_request);
  expect(!canceled_retry && canceled_retry.error().message().find("retry canceled") != std::string::npos && cancel_retry_inner.requests().size() == 1 &&
             cancel_retry_events.size() == 1,
         "retry transport observes cancellation before sleeping for a retry");

  ava::tests::FakeTransport direct_cancel_inner({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  auto direct_canceled = static_cast<ava::http::Transport&>(direct_cancel_inner).send(retry_request, [] { return true; });
  expect(!direct_canceled && direct_canceled.error().message().find("canceled") != std::string::npos && direct_cancel_inner.requests().empty(),
         "transport default cancellable send checks cancellation before dispatch");

  ava::tests::FakeTransport retry_call_cancel_inner({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  ava::http::RetryTransport retry_call_cancel_transport(retry_call_cancel_inner, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0});
  auto retry_call_canceled = retry_call_cancel_transport.send(retry_request, [] { return true; });
  expect(
      !retry_call_canceled && retry_call_canceled.error().message().find("retry canceled") != std::string::npos && retry_call_cancel_inner.requests().empty(),
      "retry transport cancellable send checks cancellation before dispatch");

  CancelDuringSendTransport cancel_during_send_inner;
  bool cancel_during_send = false;
  cancel_during_send_inner.before_cancel_check([&cancel_during_send] { cancel_during_send = true; });
  ava::http::RetryTransport cancel_during_send_transport(cancel_during_send_inner, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0});
  auto send_callback_canceled = cancel_during_send_transport.send(retry_request, [&cancel_during_send] { return cancel_during_send; });
  expect(!send_callback_canceled && send_callback_canceled.error().message().find("retry canceled") != std::string::npos &&
             cancel_during_send_inner.saw_cancel_callback() && cancel_during_send_inner.requests().size() == 1,
         "retry transport observes cancellation raised by the transport callback without retrying");

  FailingOnceTransport failing_once;
  ava::http::RetryTransport retry_transport_error(failing_once, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0, .max_retry_after_ms = 0});
  auto retried_transport_error = retry_transport_error.send(retry_request);
  expect(retried_transport_error && retried_transport_error->status_code == 200 && failing_once.requests().size() == 2,
         "retry transport retries retryable transport errors");

  StreamingFakeTransport streaming_inner({ava::http::HttpResponse{.status_code = 429, .headers = {{"Retry-After", "0"}}, .body = "rate limited body"},
                                          ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "data: [DONE]\n\n"}});
  ava::http::RetryTransport streaming_retry_transport(
      streaming_inner, ava::http::RetryOptions{
                           .max_attempts = 2, .base_delay_ms = 0, .max_retry_after_ms = 0, .response_retry_decision = ava::provider::provider_retry_decision});
  std::string streamed_body;
  auto streaming_retry = streaming_retry_transport.send_streaming(retry_request, [&streamed_body](std::string_view chunk) -> ava::core::VoidResult {
    streamed_body.append(chunk);
    return {};
  });
  expect(streaming_retry && streaming_retry->status_code == 200 && streaming_inner.requests().size() == 2 && streamed_body == "data: [DONE]\n\n",
         "retry transport retries rate-limited streaming responses and only delivers final chunks");

  PartialStreamingFailureTransport partial_failure_inner;
  ava::http::RetryTransport partial_failure_retry(partial_failure_inner, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0});
  std::string partial_body;
  auto partial_failure = partial_failure_retry.send_streaming(retry_request, [&partial_body](std::string_view chunk) -> ava::core::VoidResult {
    partial_body.append(chunk);
    return {};
  });
  expect(!partial_failure && partial_failure.error().category() == ava::core::ErrorCategory::Io && partial_failure_inner.attempts() == 1 &&
             partial_body == "accepted partial body",
         "retry transport never retries an IO failure after any accepted streaming body delivery");

  StreamingFakeTransport sink_failure_inner({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "accepted"},
                                             ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "unexpected retry"}});
  ava::http::RetryTransport sink_failure_retry(sink_failure_inner, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0});
  auto sink_failure = sink_failure_retry.send_streaming(retry_request, [](std::string_view) -> ava::core::VoidResult {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "sink rejected accepted bytes"));
  });
  expect(!sink_failure && sink_failure.error().message() == "sink rejected accepted bytes" && sink_failure_inner.requests().size() == 1,
         "retry transport marks body delivery before invoking a failing sink and never retries its IO error");

  ava::tests::FakeTransport default_error_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "withheld default error"}});
  std::string default_error_sink;
  auto default_error = static_cast<ava::http::Transport&>(default_error_inner)
                           .send_streaming(retry_request, [&default_error_sink](std::string_view chunk) -> ava::core::VoidResult {
                             default_error_sink.append(chunk);
                             return {};
                           });
  expect(default_error && default_error->body == "withheld default error" && default_error_sink.empty(),
         "default streaming transport retains non-2xx bodies without publishing them");

  StreamingFakeTransport streaming_cancel_inner({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "data: [DONE]\n\n"}});
  ava::http::RetryTransport streaming_cancel_transport(streaming_cancel_inner, ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0});
  auto streaming_pre_canceled =
      streaming_cancel_transport.send_streaming(retry_request, [](std::string_view) -> ava::core::VoidResult { return {}; }, [] { return true; });
  expect(!streaming_pre_canceled && streaming_pre_canceled.error().message().find("retry canceled") != std::string::npos &&
             streaming_cancel_inner.requests().empty(),
         "retry streaming transport checks cancellation before dispatching the first attempt");

  ava::tests::FakeTransport absent_callback_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "try again"},
                                                   ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  ava::http::RetryTransport absent_callback_transport(absent_callback_inner,
                                                      ava::http::RetryOptions{.max_attempts = 2, .base_delay_ms = 0, .max_retry_after_ms = 0});
  auto absent_callback = absent_callback_transport.send(retry_request);
  expect(absent_callback && absent_callback->status_code == 503 && absent_callback_inner.requests().size() == 1,
         "retry transport does not retry HTTP responses when response_retry_decision is absent");

  expect(ava::provider::provider_retry_decision(ava::http::HttpResponse{.status_code = 429, .headers = {}, .body = "rate limited"}) ==
             ava::http::ResponseRetryDecision::RateLimited,
         "provider retry decision retries generic 429 responses");
  expect(ava::provider::provider_retry_decision(ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "try again"}) ==
             ava::http::ResponseRetryDecision::Transient,
         "provider retry decision retries generic 503 responses");
  expect(ava::provider::provider_retry_decision(ava::http::HttpResponse{.status_code = 429, .headers = {}, .body = "insufficient_quota: billing hard limit"}) ==
             ava::http::ResponseRetryDecision::NoRetry,
         "provider retry decision maps 429 quota bodies to NoRetry");
  expect(ava::provider::provider_retry_decision(ava::http::HttpResponse{
             .status_code = 500, .headers = {}, .body = "prompt tokens exceeded context window limit"}) == ava::http::ResponseRetryDecision::NoRetry,
         "provider retry decision maps 500 context-overflow bodies to NoRetry");
  expect(ava::provider::provider_retry_decision(ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "model refusal: cannot comply"}) ==
             ava::http::ResponseRetryDecision::NoRetry,
         "provider retry decision maps 500 refusal bodies to NoRetry");

  ava::tests::FakeTransport no_retry_quota_inner({ava::http::HttpResponse{.status_code = 429, .headers = {}, .body = "insufficient_quota: billing hard limit"},
                                                  ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  ava::http::RetryTransport no_retry_quota_transport(
      no_retry_quota_inner,
      ava::http::RetryOptions{
          .max_attempts = 2, .base_delay_ms = 0, .max_retry_after_ms = 0, .response_retry_decision = ava::provider::provider_retry_decision});
  auto no_retry_quota = no_retry_quota_transport.send(retry_request);
  expect(no_retry_quota && no_retry_quota->status_code == 429 && no_retry_quota_inner.requests().size() == 1,
         "retry transport does not retry 429 quota bodies under provider_retry_decision");

  ava::tests::FakeTransport no_retry_overflow_inner(
      {ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "prompt tokens exceeded context window limit"},
       ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  ava::http::RetryTransport no_retry_overflow_transport(
      no_retry_overflow_inner,
      ava::http::RetryOptions{
          .max_attempts = 2, .base_delay_ms = 0, .max_retry_after_ms = 0, .response_retry_decision = ava::provider::provider_retry_decision});
  auto no_retry_overflow = no_retry_overflow_transport.send(retry_request);
  expect(no_retry_overflow && no_retry_overflow->status_code == 500 && no_retry_overflow_inner.requests().size() == 1,
         "retry transport does not retry 500 context-overflow bodies under provider_retry_decision");

  ava::tests::FakeTransport no_retry_refusal_inner({ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "model refusal: cannot comply"},
                                                    ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  ava::http::RetryTransport no_retry_refusal_transport(
      no_retry_refusal_inner,
      ava::http::RetryOptions{
          .max_attempts = 2, .base_delay_ms = 0, .max_retry_after_ms = 0, .response_retry_decision = ava::provider::provider_retry_decision});
  auto no_retry_refusal = no_retry_refusal_transport.send(retry_request);
  expect(no_retry_refusal && no_retry_refusal->status_code == 500 && no_retry_refusal_inner.requests().size() == 1,
         "retry transport does not retry 500 refusal bodies under provider_retry_decision");

  constexpr char const* kBodyCanary = "RETRY_REASON_BODY_CANARY_9f3c";
  ava::tests::FakeTransport canary_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = kBodyCanary},
                                          ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  std::vector<ava::http::RetryOptions::Event> canary_events;
  ava::http::RetryTransport canary_transport(canary_inner, ava::http::RetryOptions{.max_attempts = 2,
                                                                                   .base_delay_ms = 0,
                                                                                   .max_retry_after_ms = 0,
                                                                                   .on_retry =
                                                                                       [&canary_events](ava::http::RetryOptions::Event const& event) {
                                                                                         canary_events.push_back(event);
                                                                                         return ava::core::VoidResult{};
                                                                                       },
                                                                                   .response_retry_decision = ava::provider::provider_retry_decision});
  auto canary_retry = canary_transport.send(retry_request);
  expect(canary_retry && canary_retry->status_code == 200 && canary_inner.requests().size() == 2 && canary_events.size() == 1 &&
             canary_events[0].reason == "transient" && canary_events[0].reason.find(kBodyCanary) == std::string::npos,
         "retry transport emits fixed reason strings and never copies response body canaries into retry events");
}

void exercise_contract_final_transport(std::optional<ava::http::HttpRequest> const& request)
{
  if (request)
  {
    ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
    auto fake_response = transport.send(*request);
    expect(fake_response && fake_response->body == "ok" && transport.requests().size() == 1 && transport.requests()[0].timeout_ms == request->timeout_ms,
           "fake transport records offline provider request and preserves timeout");
  }
}

}  // namespace ava::tests::provider_openai_suite
