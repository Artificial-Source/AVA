#include "sys.h"
#include "ava/agent/agent_turn_executor_internal.h"
#include "ava/agent/agent_turn_provider_internal.h"
#include "ava/agent/provider_output_validation.h"
#include "ava/agent/stream_bridge.h"

#include <expected>
#include <string>
#include <utility>

namespace ava::agent::detail {

ProviderEventAccumulator::ProviderEventAccumulator(AgentLoopOptions const& options, ava::observability::TraceContext const& trace_context,
                                                   std::unordered_set<std::string> const& finalized_provider_tool_call_ids,
                                                   std::size_t provider_iteration) noexcept
    : options_(options),
      trace_context_(trace_context),
      finalized_provider_tool_call_ids_(finalized_provider_tool_call_ids),
      provider_iteration_(provider_iteration)
{
}

ava::core::VoidResult ProviderEventAccumulator::append(std::vector<ava::provider::StreamEvent> new_events, ProviderEventPublishMode publish_mode)
{
  for (auto& event : new_events)
  {
    if (options_.max_provider_events > 0 && events_.size() >= options_.max_provider_events)
    {
      return std::unexpected(provider_event_limit_error(options_.max_provider_events));
    }
    if (event.type == ava::provider::StreamEventType::TextDelta)
    {
      if (would_exceed(assistant_and_reasoning_bytes_, event.text.size(), options_.max_assistant_text_bytes))
      {
        return std::unexpected(output_limit_error("assistant text byte limit exceeded", "max_assistant_text_bytes", options_.max_assistant_text_bytes));
      }
      assistant_and_reasoning_bytes_ += event.text.size();
    }
    else if (event.type == ava::provider::StreamEventType::ReasoningStart || event.type == ava::provider::StreamEventType::ReasoningDelta ||
             event.type == ava::provider::StreamEventType::ReasoningEnd)
    {
      auto const event_bytes =
          event.type == ava::provider::StreamEventType::ReasoningEnd
              ? event.reasoning_signature.size() + event.reasoning_redacted_data.size() + event.reasoning_native_item_json.size()
              : event.text.size() + event.reasoning_signature.size() + event.reasoning_redacted_data.size() + event.reasoning_native_item_json.size();
      if (would_exceed(assistant_and_reasoning_bytes_, event_bytes, options_.max_assistant_text_bytes))
      {
        return std::unexpected(output_limit_error("reasoning byte limit exceeded", "max_assistant_text_bytes", options_.max_assistant_text_bytes));
      }
      assistant_and_reasoning_bytes_ += event_bytes;
    }
    else if (event.type == ava::provider::StreamEventType::ToolCallStart || event.type == ava::provider::StreamEventType::ToolCallDelta ||
             event.type == ava::provider::StreamEventType::ToolCallEnd)
    {
      if (auto valid_id = validate_provider_tool_call_id(event.tool_call_id); !valid_id)
      {
        return std::unexpected(std::move(valid_id.error()));
      }
      if (finalized_provider_tool_call_ids_.contains(event.tool_call_id))
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "provider reused a finalized tool call id in the persistent session");
        error.with_context("tool_call_id", event.tool_call_id);
        error.with_context("provider_iteration", std::to_string(provider_iteration_));
        error.with_context("hint", "provider tool call ids must remain unique for the complete persistent session");
        return std::unexpected(std::move(error));
      }
      current_provider_tool_call_ids_.insert(event.tool_call_id);
      if (event.type == ava::provider::StreamEventType::ToolCallDelta)
      {
        auto& bytes = tool_argument_bytes_[event.tool_call_id];
        if (would_exceed(bytes, event.text.size(), options_.max_tool_argument_bytes))
        {
          return std::unexpected(output_limit_error("tool argument byte limit exceeded", "max_tool_argument_bytes", options_.max_tool_argument_bytes));
        }
        bytes += event.text.size();
      }
    }

    // Trace parser output before publishing the product event: an observer
    // failure is isolated and cannot suppress or reorder product output.
    if (options_.observation)
    {
      options_.observation->emit(ava::observability::TraceEventType::ProviderStreamEvent, trace_context_, [&event](auto& trace) {
        trace.phase = ava::observability::TracePhase::Provider;
        switch (event.type)
        {
          case ava::provider::StreamEventType::TextStart:
          case ava::provider::StreamEventType::TextDelta:
          case ava::provider::StreamEventType::TextEnd:
            trace.outcome = ava::observability::TraceOutcome::TextDelta;
            break;
          case ava::provider::StreamEventType::ReasoningStart:
            trace.outcome = ava::observability::TraceOutcome::ReasoningStart;
            break;
          case ava::provider::StreamEventType::ReasoningDelta:
            trace.outcome = ava::observability::TraceOutcome::ReasoningDelta;
            break;
          case ava::provider::StreamEventType::ReasoningEnd:
            trace.outcome = ava::observability::TraceOutcome::ReasoningEnd;
            break;
          case ava::provider::StreamEventType::ToolCallStart:
            trace.outcome = ava::observability::TraceOutcome::ToolCallStart;
            break;
          case ava::provider::StreamEventType::ToolCallDelta:
            trace.outcome = ava::observability::TraceOutcome::ToolCallDelta;
            break;
          case ava::provider::StreamEventType::ToolCallEnd:
            trace.outcome = ava::observability::TraceOutcome::ToolCallEnd;
            break;
          case ava::provider::StreamEventType::Done:
            trace.outcome = ava::observability::TraceOutcome::Done;
            break;
          case ava::provider::StreamEventType::Error:
            trace.outcome = ava::observability::TraceOutcome::Error;
            break;
        }
        trace.fields = {{.key = "text_bytes", .value = std::to_string(event.text.size())},
                        {.key = "tool_name", .value = "[omitted]", .provenance = ava::observability::FieldProvenance::Content},
                        {.key = "usage_present", .value = event.usage ? "true" : "false"}};
      });
    }

    if (event.type == ava::provider::StreamEventType::ToolCallStart && !event.tool_call_id.empty() && !event.tool_name.empty())
      streamed_tool_names_[event.tool_call_id] = event.tool_name;

    bool const should_publish = publish_mode == ProviderEventPublishMode::All || event.type == ava::provider::StreamEventType::ReasoningStart ||
                                event.type == ava::provider::StreamEventType::ReasoningDelta || event.type == ava::provider::StreamEventType::ReasoningEnd;
    if (should_publish)
    {
      auto public_event = event;
      if (event.type == ava::provider::StreamEventType::ToolCallStart || event.type == ava::provider::StreamEventType::ToolCallEnd)
      {
        // Start/end carry only lifecycle identity. Never surface an unexpected
        // provider payload from those lifecycle records.
        public_event.text.clear();
      }
      else if (event.type == ava::provider::StreamEventType::ToolCallDelta)
      {
        auto const found = streamed_tool_names_.find(event.tool_call_id);
        if (found == streamed_tool_names_.end())
        {
          // A malformed stream without a start cannot prove that this is not a
          // shell request. Suppress its arguments rather than exposing a
          // payload before permission mediation.
          public_event.text = "<redacted tool arguments>";
        }
        else if (found->second == "bash")
        {
          public_event.tool_name = found->second;
          public_event.text = std::string(kRedactedRunCommand);
        }
      }
      if (auto published = publish_stream_event(options_, public_event); !published)
      {
        return std::unexpected(std::move(published.error()));
      }
    }
    if (event.type == ava::provider::StreamEventType::ToolCallEnd)
      streamed_tool_names_.erase(event.tool_call_id);
    events_.push_back(std::move(event));
  }
  return {};
}

std::vector<ava::provider::StreamEvent> const& ProviderEventAccumulator::events() const noexcept
{
  return events_;
}

std::unordered_set<std::string> const& ProviderEventAccumulator::current_provider_tool_call_ids() const noexcept
{
  return current_provider_tool_call_ids_;
}

}  // namespace ava::agent::detail
