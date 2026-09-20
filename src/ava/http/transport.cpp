#include "sys.h"
#include "ava/http/transport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ava::http {
namespace {

std::string lower_copy(std::string_view value)
{
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lowered;
}

bool is_retryable_transport_error(ava::core::Error const& error) noexcept
{
  return error.category() == ava::core::ErrorCategory::Io;
}

int exponential_delay_ms(RetryOptions const& options, int attempt) noexcept
{
  if (options.base_delay_ms <= 0)
    return 0;
  long long delay = options.base_delay_ms;
  for (int index = 1; index < attempt; ++index)
  {
    if (delay > std::numeric_limits<int>::max() / 2)
      return std::numeric_limits<int>::max();
    delay *= 2;
  }
  return static_cast<int>(delay);
}

ava::core::VoidResult publish_retry_event(RetryOptions const& options, std::size_t attempt, std::size_t max_attempts, int delay_ms, std::size_t remaining_ms,
                                          std::string_view reason, int status_code, bool streaming, bool countdown_tick = false)
{
  // Countdown ticks are UI progress only; one trace records the actual retry.
  if (!countdown_tick)
    observe_transport_retry(options.observation, attempt, max_attempts, static_cast<std::size_t>(std::max(0, delay_ms)), reason, status_code, streaming);
  if (!options.on_retry)
    return {};
  return options.on_retry(RetryOptions::Event{.attempt = attempt,
                                              .max_attempts = max_attempts,
                                              .delay_ms = static_cast<std::size_t>(std::max(0, delay_ms)),
                                              .remaining_ms = remaining_ms,
                                              .reason = std::string(reason),
                                              .status_code = status_code,
                                              .streaming = streaming,
                                              .countdown_tick = countdown_tick});
}

bool retry_cancel_requested(RetryOptions const& options, Transport::CancelCallback const& cancel_requested)
{
  return (options.cancel_requested && options.cancel_requested()) || (cancel_requested && cancel_requested());
}

ava::core::Error retry_canceled_error()
{
  return ava::core::Error(ava::core::ErrorCategory::Unknown, "transport retry canceled");
}

std::optional<std::string_view> response_retry_reason(RetryOptions const& options, HttpResponse const& response)
{
  if (!options.response_retry_decision)
    return std::nullopt;
  switch (options.response_retry_decision(response))
  {
    case ResponseRetryDecision::NoRetry:
      return std::nullopt;
    case ResponseRetryDecision::RateLimited:
      return "rate_limited";
    case ResponseRetryDecision::Transient:
      return "transient";
  }
  return std::nullopt;
}

ava::core::VoidResult sleep_before_retry(RetryOptions const& options, std::size_t attempt, std::size_t max_attempts, int delay_ms, std::string_view reason,
                                         int status_code, bool streaming, Transport::CancelCallback const& cancel_requested = nullptr)
{
  if (delay_ms <= 0)
    return {};
  auto const tick_ms = std::max(0, options.countdown_tick_ms);
  auto remaining_ms = delay_ms;
  while (remaining_ms > 0)
  {
    if (retry_cancel_requested(options, cancel_requested))
      return std::unexpected(retry_canceled_error());
    auto const chunk_ms = tick_ms > 0 ? std::min(tick_ms, remaining_ms) : remaining_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(chunk_ms));
    remaining_ms -= chunk_ms;
    if (retry_cancel_requested(options, cancel_requested))
      return std::unexpected(retry_canceled_error());
    if (tick_ms > 0)
    {
      if (auto published =
              publish_retry_event(options, attempt, max_attempts, delay_ms, static_cast<std::size_t>(remaining_ms), reason, status_code, streaming, true);
          !published)
      {
        return std::unexpected(std::move(published.error()));
      }
    }
  }
  return {};
}

std::optional<int> retry_after_ms(HttpResponse const& response, int max_retry_after_ms)
{
  auto const value = retry_after_header(response);
  if (!value)
    return std::nullopt;
  std::size_t index = 0;
  while (index < value->size() && std::isspace(static_cast<unsigned char>((*value)[index])) != 0) ++index;
  auto const start = index;
  while (index < value->size() && std::isdigit(static_cast<unsigned char>((*value)[index])) != 0) ++index;
  if (index == start)
    return std::nullopt;
  try
  {
    auto const seconds = std::stoi(value->substr(start, index - start));
    if (seconds < 0)
      return std::nullopt;
    long long const delay_ms = static_cast<long long>(seconds) * 1000LL;
    return static_cast<int>(std::min(delay_ms, static_cast<long long>(max_retry_after_ms)));
  }
  catch (...)
  {
    return std::nullopt;
  }
}

}  // namespace

bool Transport::supports_streaming() const noexcept
{
  return false;
}

ava::core::Result<HttpResponse> Transport::send(HttpRequest const& request, CancelCallback cancel_requested)
{
  if (cancel_requested && cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
  }
  auto response = send(request);
  if (!response)
    return std::unexpected(std::move(response.error()));
  if (cancel_requested && cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
  }
  return response;
}

ava::core::Result<HttpResponse> Transport::send_streaming(HttpRequest const& request, BodyChunkSink on_body_chunk, CancelCallback cancel_requested)
{
  if (cancel_requested && cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
  }
  auto response = send(request, cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  if (cancel_requested && cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
  }
  if (response->status_code >= 200 && response->status_code < 300 && on_body_chunk && !response->body.empty())
  {
    if (auto delivered = on_body_chunk(response->body); !delivered)
      return std::unexpected(std::move(delivered.error()));
  }
  if (cancel_requested && cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
  }
  return response;
}

RetryTransport::RetryTransport(Transport& inner, RetryOptions options) : inner_(inner), options_(options)
{
}

ava::core::Result<HttpResponse> RetryTransport::send(HttpRequest const& request)
{
  return send(request, nullptr);
}

ava::core::Result<HttpResponse> RetryTransport::send(HttpRequest const& request, CancelCallback cancel_requested)
{
  int const max_attempts = std::max(1, options_.max_attempts);
  auto combined_cancel_requested = [this, cancel_requested] { return retry_cancel_requested(options_, cancel_requested); };
  if (combined_cancel_requested())
    return std::unexpected(retry_canceled_error());
  ava::core::Result<HttpResponse> response = inner_.send(request, combined_cancel_requested);
  bool canceled_after_attempt = combined_cancel_requested();
  observe_transport_result(options_.observation, request, response, canceled_after_attempt, true);
  if (canceled_after_attempt)
    return std::unexpected(retry_canceled_error());
  for (int attempt = 1; attempt < max_attempts; ++attempt)
  {
    if (combined_cancel_requested())
      return std::unexpected(retry_canceled_error());
    if (response)
    {
      auto const reason = response_retry_reason(options_, *response);
      if (!reason)
        break;
      int const delay_ms = retry_after_ms(*response, options_.max_retry_after_ms).value_or(exponential_delay_ms(options_, attempt));
      if (auto published = publish_retry_event(options_, static_cast<std::size_t>(attempt + 1), static_cast<std::size_t>(max_attempts), delay_ms,
                                               static_cast<std::size_t>(delay_ms), *reason, response->status_code, false);
          !published)
      {
        return std::unexpected(std::move(published.error()));
      }
      if (auto slept = sleep_before_retry(options_, static_cast<std::size_t>(attempt + 1), static_cast<std::size_t>(max_attempts), delay_ms, *reason,
                                          response->status_code, false, cancel_requested);
          !slept)
      {
        return std::unexpected(std::move(slept.error()));
      }
    }
    else
    {
      if (!is_retryable_transport_error(response.error()))
        break;
      int const delay_ms = exponential_delay_ms(options_, attempt);
      if (auto published = publish_retry_event(options_, static_cast<std::size_t>(attempt + 1), static_cast<std::size_t>(max_attempts), delay_ms,
                                               static_cast<std::size_t>(delay_ms), "transport_io", 0, false);
          !published)
      {
        return std::unexpected(std::move(published.error()));
      }
      if (auto slept = sleep_before_retry(options_, static_cast<std::size_t>(attempt + 1), static_cast<std::size_t>(max_attempts), delay_ms, "transport_io", 0,
                                          false, cancel_requested);
          !slept)
      {
        return std::unexpected(std::move(slept.error()));
      }
    }
    if (combined_cancel_requested())
      return std::unexpected(retry_canceled_error());
    response = inner_.send(request, combined_cancel_requested);
    canceled_after_attempt = combined_cancel_requested();
    observe_transport_result(options_.observation, request, response, canceled_after_attempt, true);
    if (canceled_after_attempt)
      return std::unexpected(retry_canceled_error());
  }
  return response;
}

bool RetryTransport::supports_streaming() const noexcept
{
  return inner_.supports_streaming();
}

ava::core::Result<HttpResponse> RetryTransport::send_streaming(HttpRequest const& request, BodyChunkSink on_body_chunk, CancelCallback cancel_requested)
{
  int const max_attempts = std::max(1, options_.max_attempts);
  auto combined_cancel_requested = [this, cancel_requested] { return retry_cancel_requested(options_, cancel_requested); };
  if (combined_cancel_requested())
    return std::unexpected(retry_canceled_error());
  ava::core::Result<HttpResponse> response = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "streaming request was not attempted"));
  for (int attempt = 1; attempt <= max_attempts; ++attempt)
  {
    if (combined_cancel_requested())
      return std::unexpected(retry_canceled_error());
    bool accepted_body = false;
    response = inner_.send_streaming(
        request,
        [&](std::string_view chunk) -> ava::core::VoidResult {
          if (chunk.empty())
            return {};
          accepted_body = true;
          if (!on_body_chunk)
            return {};
          return on_body_chunk(chunk);
        },
        combined_cancel_requested);
    bool const canceled_after_attempt = combined_cancel_requested();
    observe_transport_result(options_.observation, request, response, canceled_after_attempt, true);
    if (canceled_after_attempt)
      return std::unexpected(retry_canceled_error());

    bool const last_attempt = attempt == max_attempts;
    if (response)
    {
      if (!last_attempt && !accepted_body)
      {
        if (auto const reason = response_retry_reason(options_, *response))
        {
          int const delay_ms = retry_after_ms(*response, options_.max_retry_after_ms).value_or(exponential_delay_ms(options_, attempt));
          if (auto published = publish_retry_event(options_, static_cast<std::size_t>(attempt + 1), static_cast<std::size_t>(max_attempts), delay_ms,
                                                   static_cast<std::size_t>(delay_ms), *reason, response->status_code, true);
              !published)
          {
            return std::unexpected(std::move(published.error()));
          }
          if (auto slept = sleep_before_retry(options_, static_cast<std::size_t>(attempt + 1), static_cast<std::size_t>(max_attempts), delay_ms, *reason,
                                              response->status_code, true, cancel_requested);
              !slept)
          {
            return std::unexpected(std::move(slept.error()));
          }
          continue;
        }
      }
      break;
    }

    if (last_attempt || accepted_body || !is_retryable_transport_error(response.error()))
      break;
    int const delay_ms = exponential_delay_ms(options_, attempt);
    if (auto published = publish_retry_event(options_, static_cast<std::size_t>(attempt + 1), static_cast<std::size_t>(max_attempts), delay_ms,
                                             static_cast<std::size_t>(delay_ms), "transport_io", 0, true);
        !published)
    {
      return std::unexpected(std::move(published.error()));
    }
    if (auto slept = sleep_before_retry(options_, static_cast<std::size_t>(attempt + 1), static_cast<std::size_t>(max_attempts), delay_ms, "transport_io", 0,
                                        true, cancel_requested);
        !slept)
    {
      return std::unexpected(std::move(slept.error()));
    }
  }
  if (!response)
    return std::unexpected(std::move(response.error()));
  if (combined_cancel_requested())
    return std::unexpected(retry_canceled_error());
  return response;
}

ObservedTransport::ObservedTransport(Transport& inner, TransportObservation observation) : inner_(inner), observation_(std::move(observation))
{
}

ava::core::Result<HttpResponse> ObservedTransport::send(HttpRequest const& request)
{
  return send(request, nullptr);
}

ava::core::Result<HttpResponse> ObservedTransport::send(HttpRequest const& request, CancelCallback cancel_requested)
{
  bool cancellation_observed = false;
  auto forwarded_cancel_requested = cancel_requested ? CancelCallback{[&cancel_requested, &cancellation_observed] {
    bool const canceled = cancel_requested();
    cancellation_observed = cancellation_observed || canceled;
    return canceled;
  }}
                                                     : CancelCallback{};
  auto result = inner_.send(request, std::move(forwarded_cancel_requested));
  observe_transport_result(observation_, request, result, cancellation_observed);
  return result;
}

bool ObservedTransport::supports_streaming() const noexcept
{
  return inner_.supports_streaming();
}

ava::core::Result<HttpResponse> ObservedTransport::send_streaming(HttpRequest const& request, BodyChunkSink on_body_chunk, CancelCallback cancel_requested)
{
  bool cancellation_observed = false;
  auto forwarded_cancel_requested = cancel_requested ? CancelCallback{[&cancel_requested, &cancellation_observed] {
    bool const canceled = cancel_requested();
    cancellation_observed = cancellation_observed || canceled;
    return canceled;
  }}
                                                     : CancelCallback{};
  auto result = inner_.send_streaming(request, std::move(on_body_chunk), std::move(forwarded_cancel_requested));
  observe_transport_result(observation_, request, result, cancellation_observed);
  return result;
}

void observe_transport_result(TransportObservation const& observation, HttpRequest const& request, ava::core::Result<HttpResponse> const& result, bool canceled,
                              bool attempt) noexcept
{
  if (!observation.observation)
    return;
  observation.observation->emit(
      attempt ? ava::observability::TraceEventType::TransportAttemptResult : ava::observability::TraceEventType::TransportRequestResult, observation.context,
      [&](auto& event) {
        event.phase = ava::observability::TracePhase::Transport;
        event.outcome = canceled ? ava::observability::TraceOutcome::Canceled
                        : result ? (result->status_code >= 200 && result->status_code < 300 ? ava::observability::TraceOutcome::Success
                                                                                            : ava::observability::TraceOutcome::Error)
                                 : ava::observability::TraceOutcome::Error;
        event.fields = {{.key = "request_bytes", .value = std::to_string(request.body.size())},
                        {.key = "status_code", .value = result ? std::to_string(result->status_code) : "0"}};
      });
}

void observe_transport_retry(TransportObservation const& observation, std::size_t next_attempt, std::size_t max_attempts, std::size_t delay_ms,
                             std::string_view reason, int status_code, bool streaming) noexcept
{
  if (!observation.observation)
    return;
  observation.observation->emit(ava::observability::TraceEventType::TransportRetry, observation.context, [&](auto& event) {
    event.phase = ava::observability::TracePhase::Transport;
    event.outcome = ava::observability::TraceOutcome::Retrying;
    event.fields = {{.key = "next_attempt", .value = std::to_string(next_attempt)}, {.key = "max_attempts", .value = std::to_string(max_attempts)},
                    {.key = "delay_ms", .value = std::to_string(delay_ms)},         {.key = "status_code", .value = std::to_string(status_code)},
                    {.key = "streaming", .value = streaming ? "true" : "false"},    {.key = "reason", .value = std::string(reason)}};
  });
}

std::optional<std::string> retry_after_header(HttpResponse const& response)
{
  constexpr unsigned int kMaxRetryAfterSeconds = 86'400;
  for (auto const& [key, value] : response.headers)
  {
    if (lower_copy(key) != "retry-after" || value.empty() || value.size() > 5)
      continue;
    unsigned int seconds = 0;
    for (unsigned char const ch : value)
    {
      if (std::isdigit(ch) == 0 || seconds > (kMaxRetryAfterSeconds - static_cast<unsigned int>(ch - '0')) / 10U)
        return std::nullopt;
      seconds = seconds * 10U + static_cast<unsigned int>(ch - '0');
    }
    return std::to_string(seconds);
  }
  return std::nullopt;
}

}  // namespace ava::http
