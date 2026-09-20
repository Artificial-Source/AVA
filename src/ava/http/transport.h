#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/observability/run_observer.h"
#include "ava/core/result.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "debug.h"

namespace ava::http {

struct HttpRequest
{
  std::string method;
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  // HTTP transports should honor this deadline and tests should preserve it verbatim.
  int timeout_ms = 60000;
  bool follow_redirects = true;
  bool include_response_headers = false;
  std::vector<std::string> resolve_hosts;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct HttpResponse
{
  int status_code = 0;
  std::map<std::string, std::string> headers;
  std::string body;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TransportObservation
{
  std::shared_ptr<ava::observability::RunObservation> observation;
  ava::observability::TraceContext context;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class Transport
{
 public:
  // Streaming transports invoke this sink only for bytes from the final 2xx
  // response after its authoritative status is known. Bytes may already have
  // been accepted if a later stream, sink, or process failure is returned.
  // HttpResponse::body is retained for every status.
  using BodyChunkSink = std::function<ava::core::VoidResult(std::string_view)>;
  using CancelCallback = std::function<bool()>;

  virtual ~Transport() = default;
  [[nodiscard]] virtual ava::core::Result<HttpResponse> send(HttpRequest const& request) = 0;
  [[nodiscard]] virtual ava::core::Result<HttpResponse> send(HttpRequest const& request, CancelCallback cancel_requested);
  [[nodiscard]] virtual bool supports_streaming() const noexcept;
  [[nodiscard]] virtual ava::core::Result<HttpResponse> send_streaming(HttpRequest const& request, BodyChunkSink on_body_chunk,
                                                                       CancelCallback cancel_requested = nullptr);

  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

// Neutral construction seam. Production factories capture explicit process
// authority; tests may return in-process transports that spawn nothing.
using TransportFactory = std::function<ava::core::Result<std::unique_ptr<Transport>>()>;

enum class ResponseRetryDecision
{
  NoRetry,
  RateLimited,
  Transient,
};

struct RetryOptions
{
  // Deliberately separate from HttpRequest: this configuration observes retry
  // composition, not protocol bytes.
  TransportObservation observation = {};
  int max_attempts = 3;
  int base_delay_ms = 250;
  int max_retry_after_ms = 60'000;
  int countdown_tick_ms = 1000;
  struct Event
  {
    std::size_t attempt = 0;
    std::size_t max_attempts = 0;
    std::size_t delay_ms = 0;
    std::size_t remaining_ms = 0;
    std::string reason = {};
    int status_code = 0;
    bool streaming = false;
    bool countdown_tick = false;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };
  std::function<ava::core::VoidResult(Event const&)> on_retry = nullptr;
  Transport::CancelCallback cancel_requested = nullptr;
  // Closed HTTP response retry policy. Null means responses are never retried;
  // transport IO retries remain independent of this callback.
  std::function<ResponseRetryDecision(HttpResponse const&)> response_retry_decision = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class RetryTransport final : public Transport
{
 public:
  RetryTransport(Transport& inner, RetryOptions options = {});
  [[nodiscard]] ava::core::Result<HttpResponse> send(HttpRequest const& request) override;
  [[nodiscard]] ava::core::Result<HttpResponse> send(HttpRequest const& request, CancelCallback cancel_requested) override;
  [[nodiscard]] bool supports_streaming() const noexcept override;
  [[nodiscard]] ava::core::Result<HttpResponse> send_streaming(HttpRequest const& request, BodyChunkSink on_body_chunk,
                                                               CancelCallback cancel_requested = nullptr) override;
  // RetryOptions contains runtime callbacks and observation state.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  Transport& inner_;
  RetryOptions options_;
};

// The non-overridable observation boundary. New concrete transports only
// implement Transport; callers compose this decorator when tracing is enabled.
class ObservedTransport final : public Transport
{
 public:
  ObservedTransport(Transport& inner, TransportObservation observation);
  [[nodiscard]] ava::core::Result<HttpResponse> send(HttpRequest const& request) override;
  [[nodiscard]] ava::core::Result<HttpResponse> send(HttpRequest const& request, CancelCallback cancel_requested) override;
  [[nodiscard]] bool supports_streaming() const noexcept override;
  [[nodiscard]] ava::core::Result<HttpResponse> send_streaming(HttpRequest const& request, BodyChunkSink on_body_chunk,
                                                               CancelCallback cancel_requested = nullptr) override;

 private:
  Transport& inner_;
  TransportObservation observation_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

void observe_transport_result(TransportObservation const& observation, HttpRequest const& request, ava::core::Result<HttpResponse> const& result,
                              bool canceled = false, bool attempt = false) noexcept;
void observe_transport_retry(TransportObservation const& observation, std::size_t next_attempt, std::size_t max_attempts, std::size_t delay_ms,
                             std::string_view reason, int status_code, bool streaming) noexcept;

[[nodiscard]] std::optional<std::string> retry_after_header(HttpResponse const& response);

}  // namespace ava::http
