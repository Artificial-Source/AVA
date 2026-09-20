#include "sys.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_turn_executor_internal.h"
#include "ava/agent/agent_turn_provider_internal.h"
#include "ava/agent/assistant_turn.h"
#include "ava/agent/usage_accounting.h"
#include "ava/provider/provider_utils.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ava::agent::detail {
namespace {

// AgentTurnSession::check_canceled persists the cancellation itself. Keep its
// boundary-tagged result out of provider retry and error persistence.
bool is_checked_agent_loop_cancellation(ava::core::Error const& error)
{
  if (error.category() != ava::core::ErrorCategory::Unknown || error.message() != "agent loop canceled")
    return false;
  for (auto const& context : error.context())
  {
    if (context.key == "boundary")
      return true;
  }
  return false;
}

}  // namespace

ava::core::VoidResult AgentTurnExecutor::receive_provider_events(ava::http::HttpRequest const& built_request,
                                                                 ava::provider::ProviderRequest const& provider_request, ProviderEventAccumulator& accumulator)
{
  if (auto not_canceled = session_.check_canceled("before_provider_transport"); !not_canceled)
    return std::unexpected(std::move(not_canceled.error()));

  if (provider_request.stream && effective_transport_->supports_streaming())
  {
    bool processed_stream_chunks = false;
    auto stream_parser = provider_.create_stream_parser();
    auto response = effective_transport_->send_streaming(
        built_request,
        [&](std::string_view chunk) -> ava::core::VoidResult {
          processed_stream_chunks = true;
          if (session_.is_canceled())
            return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled"));
          auto parsed = stream_parser->append(chunk);
          if (!parsed)
            return std::unexpected(std::move(parsed.error()));
          return accumulator.append(std::move(*parsed), ProviderEventPublishMode::All);
        },
        [cancel_requested = options_.cancel_requested]() { return cancel_requested && cancel_requested(); });
    if (!response)
    {
      if (session_.is_canceled())
      {
        if (auto not_canceled = session_.check_canceled("during_provider_stream"); !not_canceled)
          return std::unexpected(std::move(not_canceled.error()));
      }
      return std::unexpected(std::move(response.error()));
    }
    if (auto not_canceled = session_.check_canceled("after_provider_call"); !not_canceled)
      return std::unexpected(std::move(not_canceled.error()));

    if (response->status_code < 200 || response->status_code >= 300)
    {
      auto events = provider_.parse_response(*response, provider_request.stream);
      if (!events)
        return std::unexpected(std::move(events.error()));
      return accumulator.append(std::move(*events), ProviderEventPublishMode::All);
    }
    if (!processed_stream_chunks && accumulator.events().empty() && !response->body.empty())
    {
      auto events = provider_.parse_response(*response, provider_request.stream);
      if (!events)
        return std::unexpected(std::move(events.error()));
      auto appended = accumulator.append(std::move(*events), ProviderEventPublishMode::All);
      if (!appended && session_.is_canceled())
      {
        if (auto not_canceled = session_.check_canceled("during_provider_stream"); !not_canceled)
          return std::unexpected(std::move(not_canceled.error()));
      }
      return appended;
    }

    auto parsed = stream_parser->finish();
    if (!parsed)
      return std::unexpected(std::move(parsed.error()));
    auto appended = accumulator.append(std::move(*parsed), ProviderEventPublishMode::All);
    if (!appended && session_.is_canceled())
    {
      if (auto not_canceled = session_.check_canceled("during_provider_stream"); !not_canceled)
        return std::unexpected(std::move(not_canceled.error()));
    }
    return appended;
  }

  auto response =
      effective_transport_->send(built_request, [cancel_requested = options_.cancel_requested]() { return cancel_requested && cancel_requested(); });
  if (!response)
  {
    if (session_.is_canceled())
    {
      if (auto not_canceled = session_.check_canceled("during_provider_request"); !not_canceled)
        return std::unexpected(std::move(not_canceled.error()));
    }
    return std::unexpected(std::move(response.error()));
  }
  if (auto not_canceled = session_.check_canceled("after_provider_call"); !not_canceled)
    return std::unexpected(std::move(not_canceled.error()));
  auto events = provider_.parse_response(*response, provider_request.stream);
  if (!events)
    return std::unexpected(std::move(events.error()));
  return accumulator.append(std::move(*events), ProviderEventPublishMode::ReasoningOnly);
}

ava::core::Result<ProviderTurn> AgentTurnExecutor::request_provider_turn()
{
  ToolDispatcher const& dispatcher = *dispatcher_storage_;

  while (true)
  {
    if (auto not_canceled = session_.check_canceled("before_provider_call"); !not_canceled)
      return std::unexpected(std::move(not_canceled.error()));

    if (options_.take_steering_messages)
    {
      auto steering_messages = options_.take_steering_messages();
      if (!steering_messages)
        return std::unexpected(std::move(steering_messages.error()));
      for (auto const& steering_message : *steering_messages)
      {
        if (auto appended = append_active_turn_user_message(steering_message, {}); !appended)
          return std::unexpected(appended.error());
      }
    }

    if (skip_auto_compaction_after_overflow_retry_)
    {
      skip_auto_compaction_after_overflow_retry_ = false;
    }
    else if (options_.child_compaction_binding || (options_.compact_context && result_.provider_iterations == 0 && !pre_turn_compacted_))
    {
      auto compacted = compact_context("auto");
      if (!compacted)
        return std::unexpected(std::move(compacted.error()));
      if (*compacted)
      {
        if (auto replayed = replay_active_turn_user_messages(); !replayed)
          return std::unexpected(std::move(replayed.error()));
      }
      if (auto not_canceled = session_.check_canceled("after_auto_compaction"); !not_canceled)
        return std::unexpected(std::move(not_canceled.error()));
    }

    auto messages = build_messages();
    if (!messages)
      return std::unexpected(messages.error());
    auto const tool_schemas =
        options_.model.supports_tools && !finalizing_after_tool_limit_ ? dispatcher.registered_tool_schemas_json() : std::vector<std::string>{};
    auto max_output_tokens = options_.model.max_output_tokens;
    auto reasoning = options_.model.reasoning;
    if (finalizing_after_tool_limit_)
    {
      max_output_tokens = std::min<long long>(max_output_tokens.value_or(2048), 2048);
      reasoning = std::nullopt;
    }
    ava::provider::ProviderRequest provider_request{.provider_id = options_.model.provider_id,
                                                    .model_id = options_.model.model_id,
                                                    .system_prompt = options_.model.system_prompt,
                                                    .messages = messages->messages,
                                                    .tools_json = tool_schemas,
                                                    .stream = options_.model.stream && options_.model.supports_streaming,
                                                    .max_output_tokens = max_output_tokens,
                                                    .reasoning = reasoning,
                                                    .compatibility_quirks = options_.model.compatibility_quirks};
    bool const model_supports_images =
        std::find(options_.model.input_modalities.begin(), options_.model.input_modalities.end(), "image") != options_.model.input_modalities.end();
    if (auto valid_images = ava::provider::validate_image_content_parts(provider_request, model_supports_images); !valid_images)
    {
      static_cast<void>(session_.append_error(valid_images.error()));
      return std::unexpected(std::move(valid_images.error()));
    }
    if (auto attached_images = session_.attach_verified_image_payloads(provider_request); !attached_images)
    {
      static_cast<void>(session_.append_error(attached_images.error()));
      return std::unexpected(std::move(attached_images.error()));
    }

    ava::provider::ProviderAuthContext const auth_context{
        .access_token = options_.access_token,
        .credential_type = options_.openai_oauth && options_.credential_type == "bearer" ? "oauth" : options_.credential_type,
        .account_id = options_.openai_account_id};
    auto built_request = provider_.build_request(provider_request, auth_context);
    if (!built_request)
    {
      if (auto retry = prepare_context_overflow_retry(built_request.error()); !retry)
        return std::unexpected(std::move(retry.error()));
      else if (*retry)
        continue;
      static_cast<void>(session_.append_error(built_request.error()));
      return std::unexpected(built_request.error());
    }

    result_.used_compacted_context = result_.used_compacted_context || messages->used_compacted_context;
    if (result_.provider_iterations == 0)
      result_.initial_context_messages = provider_request.messages.size();

    ProviderEventAccumulator accumulator(options_, trace_context_, finalized_provider_tool_call_ids_, result_.provider_iterations + 1);
    if (auto phase = publish_phase(RunPhase::AwaitingProvider); !phase)
      return std::unexpected(std::move(phase.error()));
    auto received = receive_provider_events(*built_request, provider_request, accumulator);
    if (!received)
    {
      if (is_checked_agent_loop_cancellation(received.error()))
        return std::unexpected(std::move(received.error()));
      if (auto retry = prepare_context_overflow_retry(received.error()); !retry)
        return std::unexpected(std::move(retry.error()));
      else if (*retry)
        continue;
      static_cast<void>(session_.append_error(received.error()));
      return std::unexpected(std::move(received.error()));
    }

    auto turn = parse_assistant_turn(accumulator.events(), ProviderOutputLimits{.max_events = options_.max_provider_events,
                                                                                .max_assistant_text_bytes = options_.max_assistant_text_bytes,
                                                                                .max_tool_argument_bytes = options_.max_tool_argument_bytes});
    if (!turn)
    {
      if (auto retry = prepare_context_overflow_retry(turn.error()); !retry)
        return std::unexpected(std::move(retry.error()));
      else if (*retry)
        continue;
      static_cast<void>(session_.append_error(turn.error()));
      return std::unexpected(turn.error());
    }

    std::unordered_set<std::string> iteration_tool_call_ids;
    for (auto const& call : turn->tool_calls)
    {
      if (!iteration_tool_call_ids.insert(call.id).second)
        continue;  // One provider turn may merge multiple deltas for one finalized call.
      if (!accumulator.current_provider_tool_call_ids().contains(call.id) || finalized_provider_tool_call_ids_.contains(call.id))
      {
        auto error =
            ava::core::Error(ava::core::ErrorCategory::Provider, "provider reused or inconsistently finalized a tool call id in the persistent session");
        error.with_context("tool_call_id", call.id);
        error.with_context("provider_iteration", std::to_string(result_.provider_iterations + 1));
        error.with_context("hint", "provider tool call ids must remain unique for the complete persistent session");
        static_cast<void>(session_.append_error(error));
        return std::unexpected(std::move(error));
      }
    }
    if (auto not_canceled = session_.check_canceled("before_assistant_append"); !not_canceled)
      return std::unexpected(std::move(not_canceled.error()));

    ++result_.provider_iterations;
    auto usage = turn->usage ? with_total_tokens(*turn->usage) : estimate_usage_from_turn(built_request->body, *turn);
    auto const cost_usd = options_.model.pricing && !usage.estimated ? usage_cost_usd(*options_.model.pricing, usage) : std::optional<long double>{};
    accumulate_usage(result_.usage, usage);
    if (cost_usd && accumulated_cost_known_)
    {
      result_.cost_usd = result_.cost_usd.value_or(0.0L) + *cost_usd;
    }
    else
    {
      accumulated_cost_known_ = false;
      result_.cost_usd = std::nullopt;
    }
    auto const disposition =
        *turn->finish_reason == ava::provider::ProviderFinishReason::Cancelled ? ProviderTurnDisposition::TerminalCancelled : ProviderTurnDisposition::Parsed;
    return ProviderTurn{.disposition = disposition,
                        .assistant_turn = std::move(*turn),
                        .usage = std::move(usage),
                        .cost_usd = cost_usd,
                        .iteration_tool_call_ids = std::move(iteration_tool_call_ids)};
  }
}

}  // namespace ava::agent::detail
