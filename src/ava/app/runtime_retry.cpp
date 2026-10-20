#include "sys.h"
#include "ava/event/events.h"
#include "ava/http/transport.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_retry.h"
#include "ava/session/session_store.h"

#include <mutex>
#include <string>
#include <utility>

namespace ava::app::runtime {

ava::http::RetryOptions runtime_retry_options(session_ts const& unlocked_session, RunOptions const& options)
{
  ava::http::RetryOptions retry_options;
  retry_options.cancel_requested = options.cancel_requested;
  retry_options.observation = {.observation = options.observation, .context = options.trace_context};
  retry_options.response_retry_decision = ava::provider::provider_retry_decision;
  retry_options.on_retry = [&unlocked_session, &options](ava::http::RetryOptions::Event const& retry) {
    ava::event::RetryPayload payload;
    if (retry.status_code > 0)
    {
      payload.text = "HTTP status " + std::to_string(retry.status_code);
    }
    payload.status = retry.streaming ? "streaming" : "request";
    payload.trigger = "provider_transport";
    payload.reason = retry.reason;
    payload.attempt = retry.attempt;
    payload.max_attempts = retry.max_attempts;
    payload.delay_ms = retry.delay_ms;
    payload.remaining_ms = retry.remaining_ms;
    ava::event::RuntimeEventMetadata metadata{
      .timestamp = ava::session::now_timestamp(),
      .session_id = session_ts::crat(unlocked_session)->store.options_r()->session_id,
    };
    if (retry.countdown_tick)
    {
      return ava::event::emit_event(
          options.event_sink, ava::event::RuntimeEvent{std::move(metadata), ava::event::RetryTickEvent{.payload = std::move(payload), .diagnostics = {}}});
    }
    return ava::event::emit_event(options.event_sink,
                                  ava::event::RuntimeEvent{std::move(metadata), ava::event::RetryEvent{.payload = std::move(payload), .diagnostics = {}}});
  };
  return retry_options;
}

}  // namespace ava::app::runtime
