#include "sys.h"
#include "ava/agent/assistant_turn.h"
#include "ava/agent/provider_output_validation.h"
#include "ava/agent/usage_accounting.h"

#include <algorithm>
#include <concepts>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace ava::agent {
namespace {

struct ActiveItem
{
  std::size_t ordered_item_index = 0;
  AssistantItemMetadata metadata = {};
  bool native_lifecycle = false;
};

struct ActiveFunctionCall
{
  std::size_t ordered_item_index = 0;
  AssistantItemMetadata metadata = {};
  bool native_lifecycle = false;
  bool completed = false;
};

ava::core::Error turn_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Provider, std::move(message));
}

bool has_native_metadata(AssistantItemMetadata const& metadata)
{
  return !metadata.provider_item_id.empty() || metadata.provider_output_index.has_value() || metadata.phase != ava::provider::AssistantPhase::Unknown;
}

bool is_valid_phase(ava::provider::AssistantPhase phase)
{
  return phase == ava::provider::AssistantPhase::Unknown || ava::provider::is_known_assistant_phase(phase);
}

ava::core::VoidResult validate_start_metadata(AssistantItemMetadata const& metadata, std::unordered_set<std::string>& provider_item_ids,
                                              std::unordered_set<std::size_t>& provider_output_indexes)
{
  if (!is_valid_phase(metadata.phase))
    return std::unexpected(turn_error("provider output item has an invalid assistant phase"));
  if (metadata.provider_item_id.empty())
  {
    if (has_native_metadata(metadata))
      return std::unexpected(turn_error("provider output item metadata requires a nonempty provider item ID"));
    return {};
  }
  if (!provider_item_ids.insert(metadata.provider_item_id).second)
    return std::unexpected(turn_error("provider output item ID is duplicated in one assistant turn"));
  if (metadata.provider_output_index && !provider_output_indexes.insert(*metadata.provider_output_index).second)
    return std::unexpected(turn_error("provider output item index is duplicated in one assistant turn"));
  return {};
}

ava::core::VoidResult validate_event_metadata(AssistantItemMetadata const& active, ava::provider::StreamEvent const& event)
{
  if (!is_valid_phase(event.assistant_phase))
    return std::unexpected(turn_error("provider output event has an invalid assistant phase"));
  if (!active.provider_item_id.empty() && event.provider_item_id != active.provider_item_id)
    return std::unexpected(turn_error("provider output item ID changed or was omitted during its lifecycle"));
  if (event.provider_output_index != active.provider_output_index)
    return std::unexpected(turn_error("provider output item index changed or was omitted during its lifecycle"));
  if (event.assistant_phase != active.phase)
    return std::unexpected(turn_error("provider output item assistant phase changed or was omitted during its lifecycle"));
  if (active.provider_item_id.empty() &&
      (!event.provider_item_id.empty() || event.provider_output_index || event.assistant_phase != ava::provider::AssistantPhase::Unknown))
    return std::unexpected(turn_error("legacy provider output lifecycle acquired native metadata"));
  return {};
}

std::size_t reasoning_block_bytes(ParsedReasoningBlock const& block)
{
  return block.text.size() + block.signature.size() + block.redacted_data.size() + block.native_item_json.size();
}

bool has_reasoning_payload(ParsedReasoningBlock const& block)
{
  return !block.text.empty() || !block.signature.empty() || !block.redacted_data.empty() || !block.native_item_json.empty();
}

void rebuild_legacy_aggregates(ParsedAssistantTurn& turn)
{
  auto const output_index = [](OrderedAssistantItem const& item) -> std::optional<std::size_t> {
    return std::visit([](auto const& value) { return value.metadata.provider_output_index; }, item.item);
  };
  bool const every_item_has_native_index =
      std::ranges::all_of(turn.ordered_items, [&output_index](OrderedAssistantItem const& item) { return output_index(item).has_value(); });
  if (every_item_has_native_index)
  {
    std::stable_sort(
        turn.ordered_items.begin(), turn.ordered_items.end(),
        [&output_index](OrderedAssistantItem const& left, OrderedAssistantItem const& right) { return *output_index(left) < *output_index(right); });
  }
  // A fixture with no native index (or an intentionally mixed compatible
  // event family) retains its validated item-start order. We never invent an
  // index or use a comparator whose mixed keys could reorder ambiguously.

  turn.text.clear();
  turn.reasoning_blocks.clear();
  turn.tool_calls.clear();
  for (std::size_t sequence = 0; sequence < turn.ordered_items.size(); ++sequence)
  {
    auto& ordered = turn.ordered_items[sequence];
    ordered.sequence = sequence;
    std::visit(
        [&turn](auto const& item) {
          using Item = std::decay_t<decltype(item)>;
          if constexpr (std::same_as<Item, AssistantTextItem>)
          {
            turn.text += item.text;
          }
          else if constexpr (std::same_as<Item, AssistantReasoningItem>)
          {
            if (has_reasoning_payload(item.reasoning))
              turn.reasoning_blocks.push_back(item.reasoning);
          }
          else
          {
            turn.tool_calls.push_back(item.tool_call);
          }
        },
        ordered.item);
  }
}

}  // namespace

ava::core::Result<ParsedAssistantTurn> parse_assistant_turn(std::vector<ava::provider::StreamEvent> const& events, ProviderOutputLimits limits)
{
  if (limits.max_events > 0 && events.size() > limits.max_events)
  {
    return std::unexpected(provider_event_limit_error(limits.max_events));
  }

  ParsedAssistantTurn turn;
  std::unordered_set<std::string> provider_item_ids;
  std::unordered_set<std::size_t> provider_output_indexes;
  std::unordered_map<std::string, ActiveItem> active_text_items;
  std::optional<ActiveItem> active_legacy_text_item;
  std::optional<std::size_t> synthesized_legacy_text_item;
  std::optional<ActiveItem> active_reasoning_item;
  std::unordered_map<std::string, ActiveFunctionCall> active_function_calls;
  std::size_t assistant_text_bytes = 0;
  bool done = false;

  auto append_item = [&turn](AssistantItem item) {
    auto const ordered_index = turn.ordered_items.size();
    turn.ordered_items.push_back(OrderedAssistantItem{.sequence = ordered_index, .item = std::move(item)});
    return ordered_index;
  };

  auto append_text = [&](std::size_t ordered_index, std::string_view text) -> ava::core::VoidResult {
    if (would_exceed(assistant_text_bytes, text.size(), limits.max_assistant_text_bytes))
    {
      return std::unexpected(output_limit_error("assistant text byte limit exceeded", "max_assistant_text_bytes", limits.max_assistant_text_bytes));
    }
    auto* item = std::get_if<AssistantTextItem>(&turn.ordered_items[ordered_index].item);
    if (!item)
      return std::unexpected(turn_error("internal assistant text lifecycle mismatch"));
    assistant_text_bytes += text.size();
    item->text += text;
    return {};
  };

  auto validate_text_event = [&](ActiveItem& active, ava::provider::StreamEvent const& event) -> ava::core::VoidResult {
    if (!active.metadata.provider_item_id.empty() && active.metadata.phase == ava::provider::AssistantPhase::Unknown &&
        ava::provider::is_known_assistant_phase(event.assistant_phase))
    {
      active.metadata.phase = event.assistant_phase;
      auto* item = std::get_if<AssistantTextItem>(&turn.ordered_items[active.ordered_item_index].item);
      if (!item)
        return std::unexpected(turn_error("internal assistant text lifecycle mismatch"));
      item->metadata.phase = event.assistant_phase;
    }
    return validate_event_metadata(active.metadata, event);
  };

  auto start_reasoning = [&](ava::provider::StreamEvent const& event, bool native_lifecycle) -> ava::core::Result<ActiveItem> {
    AssistantItemMetadata metadata{
        .provider_item_id = event.provider_item_id, .provider_output_index = event.provider_output_index, .phase = event.assistant_phase};
    if (auto valid = validate_start_metadata(metadata, provider_item_ids, provider_output_indexes); !valid)
      return std::unexpected(std::move(valid.error()));
    ParsedReasoningBlock reasoning{.text = "",
                                   .format = event.reasoning_format,
                                   .signature = event.reasoning_signature,
                                   .redacted_data = event.reasoning_redacted_data,
                                   .native_item_json = event.reasoning_native_item_json,
                                   .redacted = event.redacted};
    auto const private_bytes = event.reasoning_signature.size() + event.reasoning_redacted_data.size() + event.reasoning_native_item_json.size();
    if (would_exceed(std::size_t{0}, private_bytes, limits.max_assistant_text_bytes))
    {
      return std::unexpected(output_limit_error("reasoning byte limit exceeded", "max_assistant_text_bytes", limits.max_assistant_text_bytes));
    }
    auto const ordered_index = append_item(AssistantReasoningItem{.metadata = std::move(metadata), .reasoning = std::move(reasoning)});
    auto const& item = std::get<AssistantReasoningItem>(turn.ordered_items[ordered_index].item);
    return ActiveItem{.ordered_item_index = ordered_index, .metadata = item.metadata, .native_lifecycle = native_lifecycle};
  };

  auto finish_reasoning = [&]() { active_reasoning_item = std::nullopt; };

  for (auto const& event : events)
  {
    // Legacy provider families emit only deltas, so synthesize one text item
    // per contiguous delta run. A semantic output item (or a native text
    // lifecycle) is an ordering boundary, not an opportunity to append a
    // later legacy delta to text that preceded it.
    if (event.type == ava::provider::StreamEventType::TextStart || event.type == ava::provider::StreamEventType::ReasoningStart ||
        event.type == ava::provider::StreamEventType::ReasoningDelta || event.type == ava::provider::StreamEventType::ReasoningEnd ||
        event.type == ava::provider::StreamEventType::ToolCallStart || event.type == ava::provider::StreamEventType::ToolCallDelta ||
        event.type == ava::provider::StreamEventType::ToolCallEnd)
    {
      synthesized_legacy_text_item = std::nullopt;
    }

    if (done)
      return std::unexpected(turn_error("provider emitted an event after the terminal outcome"));
    if (event.usage)
      turn.usage = with_total_tokens(*event.usage);

    if (event.type == ava::provider::StreamEventType::TextStart)
    {
      AssistantItemMetadata metadata{
          .provider_item_id = event.provider_item_id, .provider_output_index = event.provider_output_index, .phase = event.assistant_phase};
      if (auto valid = validate_start_metadata(metadata, provider_item_ids, provider_output_indexes); !valid)
        return std::unexpected(std::move(valid.error()));
      auto const native_lifecycle = !metadata.provider_item_id.empty();
      if (native_lifecycle && active_text_items.contains(metadata.provider_item_id))
        return std::unexpected(turn_error("provider text item started twice before completion"));
      if (!native_lifecycle && active_legacy_text_item)
        return std::unexpected(turn_error("legacy provider text item started twice before completion"));
      auto const ordered_index = append_item(AssistantTextItem{.metadata = std::move(metadata), .text = ""});
      auto const& item = std::get<AssistantTextItem>(turn.ordered_items[ordered_index].item);
      ActiveItem active{.ordered_item_index = ordered_index, .metadata = item.metadata, .native_lifecycle = native_lifecycle};
      if (native_lifecycle)
        active_text_items.emplace(active.metadata.provider_item_id, std::move(active));
      else
        active_legacy_text_item = std::move(active);
    }
    else if (event.type == ava::provider::StreamEventType::TextDelta)
    {
      if (!is_valid_phase(event.assistant_phase))
        return std::unexpected(turn_error("provider output event has an invalid assistant phase"));
      if (!event.provider_item_id.empty())
      {
        auto const active = active_text_items.find(event.provider_item_id);
        if (active == active_text_items.end())
          return std::unexpected(turn_error("provider text delta references an unopened or completed item"));
        if (auto valid = validate_text_event(active->second, event); !valid)
          return std::unexpected(std::move(valid.error()));
        if (auto appended = append_text(active->second.ordered_item_index, event.text); !appended)
          return std::unexpected(std::move(appended.error()));
      }
      else if (active_legacy_text_item)
      {
        if (auto valid = validate_text_event(*active_legacy_text_item, event); !valid)
          return std::unexpected(std::move(valid.error()));
        if (auto appended = append_text(active_legacy_text_item->ordered_item_index, event.text); !appended)
          return std::unexpected(std::move(appended.error()));
      }
      else
      {
        if (!active_text_items.empty() || event.provider_output_index || event.assistant_phase != ava::provider::AssistantPhase::Unknown)
          return std::unexpected(turn_error("provider text delta has native metadata without a TextStart lifecycle event"));
        if (!synthesized_legacy_text_item)
        {
          synthesized_legacy_text_item = append_item(AssistantTextItem{.metadata = {}, .text = ""});
        }
        if (auto appended = append_text(*synthesized_legacy_text_item, event.text); !appended)
          return std::unexpected(std::move(appended.error()));
      }
    }
    else if (event.type == ava::provider::StreamEventType::TextEnd)
    {
      if (!event.provider_item_id.empty())
      {
        auto const active = active_text_items.find(event.provider_item_id);
        if (active == active_text_items.end())
          return std::unexpected(turn_error("provider text item ended without a matching TextStart"));
        if (auto valid = validate_text_event(active->second, event); !valid)
          return std::unexpected(std::move(valid.error()));
        active_text_items.erase(active);
      }
      else
      {
        if (!active_legacy_text_item)
          return std::unexpected(turn_error("legacy provider text item ended without a matching TextStart"));
        if (auto valid = validate_text_event(*active_legacy_text_item, event); !valid)
          return std::unexpected(std::move(valid.error()));
        active_legacy_text_item = std::nullopt;
      }
    }
    else if (event.type == ava::provider::StreamEventType::ReasoningStart)
    {
      bool const native_lifecycle = has_native_metadata(AssistantItemMetadata{
          .provider_item_id = event.provider_item_id, .provider_output_index = event.provider_output_index, .phase = event.assistant_phase});
      if (active_reasoning_item)
      {
        if (active_reasoning_item->native_lifecycle || native_lifecycle)
          return std::unexpected(turn_error("provider reasoning item started before the prior item completed"));
        finish_reasoning();
      }
      auto started = start_reasoning(event, native_lifecycle);
      if (!started)
        return std::unexpected(std::move(started.error()));
      active_reasoning_item = std::move(*started);
    }
    else if (event.type == ava::provider::StreamEventType::ReasoningDelta || event.type == ava::provider::StreamEventType::ReasoningEnd)
    {
      bool const native_lifecycle = has_native_metadata(AssistantItemMetadata{
          .provider_item_id = event.provider_item_id, .provider_output_index = event.provider_output_index, .phase = event.assistant_phase});
      if (!active_reasoning_item)
      {
        if (native_lifecycle)
          return std::unexpected(turn_error("provider reasoning lifecycle event has no matching ReasoningStart"));
        auto started = start_reasoning(event, false);
        if (!started)
          return std::unexpected(std::move(started.error()));
        active_reasoning_item = std::move(*started);
      }
      if (auto valid = validate_event_metadata(active_reasoning_item->metadata, event); !valid)
        return std::unexpected(std::move(valid.error()));
      auto* item = std::get_if<AssistantReasoningItem>(&turn.ordered_items[active_reasoning_item->ordered_item_index].item);
      if (!item)
        return std::unexpected(turn_error("internal assistant reasoning lifecycle mismatch"));
      if (item->reasoning.format.empty())
        item->reasoning.format = event.reasoning_format;
      item->reasoning.redacted = item->reasoning.redacted || event.redacted;
      if (event.type == ava::provider::StreamEventType::ReasoningDelta)
      {
        if (would_exceed(item->reasoning.text.size(), event.text.size(), limits.max_assistant_text_bytes))
        {
          return std::unexpected(output_limit_error("reasoning text byte limit exceeded", "max_assistant_text_bytes", limits.max_assistant_text_bytes));
        }
        item->reasoning.text += event.text;
      }
      else
      {
        if (!event.reasoning_signature.empty())
          item->reasoning.signature = event.reasoning_signature;
        if (!event.reasoning_redacted_data.empty())
          item->reasoning.redacted_data = event.reasoning_redacted_data;
        if (!event.reasoning_native_item_json.empty())
          item->reasoning.native_item_json = event.reasoning_native_item_json;
      }
      if (limits.max_assistant_text_bytes > 0 && reasoning_block_bytes(item->reasoning) > limits.max_assistant_text_bytes)
      {
        return std::unexpected(output_limit_error("reasoning byte limit exceeded", "max_assistant_text_bytes", limits.max_assistant_text_bytes));
      }
      if (event.type == ava::provider::StreamEventType::ReasoningEnd)
        finish_reasoning();
    }
    else if (event.type == ava::provider::StreamEventType::ToolCallStart)
    {
      if (auto valid_id = validate_provider_tool_call_id(event.tool_call_id); !valid_id)
        return std::unexpected(std::move(valid_id.error()));
      AssistantItemMetadata metadata{
          .provider_item_id = event.provider_item_id, .provider_output_index = event.provider_output_index, .phase = event.assistant_phase};
      auto const native_lifecycle = has_native_metadata(metadata);
      if (active_function_calls.contains(event.tool_call_id))
        return std::unexpected(turn_error("provider function call started twice before completion"));
      if (auto valid = validate_start_metadata(metadata, provider_item_ids, provider_output_indexes); !valid)
        return std::unexpected(std::move(valid.error()));
      auto const ordered_index = append_item(AssistantFunctionCallItem{
          .metadata = std::move(metadata), .tool_call = ProviderToolCall{.id = event.tool_call_id, .name = event.tool_name, .arguments_json = ""}});
      auto const& item = std::get<AssistantFunctionCallItem>(turn.ordered_items[ordered_index].item);
      active_function_calls.emplace(
          event.tool_call_id,
          ActiveFunctionCall{.ordered_item_index = ordered_index, .metadata = item.metadata, .native_lifecycle = native_lifecycle, .completed = false});
    }
    else if (event.type == ava::provider::StreamEventType::ToolCallDelta || event.type == ava::provider::StreamEventType::ToolCallEnd)
    {
      if (auto valid_id = validate_provider_tool_call_id(event.tool_call_id); !valid_id)
        return std::unexpected(std::move(valid_id.error()));
      auto active = active_function_calls.find(event.tool_call_id);
      if (active == active_function_calls.end())
      {
        if (has_native_metadata(AssistantItemMetadata{
                .provider_item_id = event.provider_item_id, .provider_output_index = event.provider_output_index, .phase = event.assistant_phase}))
        {
          return std::unexpected(turn_error("provider function call lifecycle event has no matching ToolCallStart"));
        }
        if (event.type == ava::provider::StreamEventType::ToolCallEnd)
          continue;  // Preserve the legacy no-op end marker behavior.
        auto const ordered_index = append_item(
            AssistantFunctionCallItem{.metadata = {}, .tool_call = ProviderToolCall{.id = event.tool_call_id, .name = event.tool_name, .arguments_json = ""}});
        active = active_function_calls
                     .emplace(event.tool_call_id,
                              ActiveFunctionCall{.ordered_item_index = ordered_index, .metadata = {}, .native_lifecycle = false, .completed = false})
                     .first;
      }
      if (active->second.completed)
        return std::unexpected(turn_error("provider function call emitted data after completion"));
      if (auto valid = validate_event_metadata(active->second.metadata, event); !valid)
        return std::unexpected(std::move(valid.error()));
      auto* item = std::get_if<AssistantFunctionCallItem>(&turn.ordered_items[active->second.ordered_item_index].item);
      if (!item)
        return std::unexpected(turn_error("internal assistant function-call lifecycle mismatch"));
      if (event.type == ava::provider::StreamEventType::ToolCallDelta)
      {
        if (would_exceed(item->tool_call.arguments_json.size(), event.text.size(), limits.max_tool_argument_bytes))
        {
          return std::unexpected(output_limit_error("tool argument byte limit exceeded", "max_tool_argument_bytes", limits.max_tool_argument_bytes));
        }
        item->tool_call.arguments_json += event.text;
      }
      else
      {
        active->second.completed = true;
      }
    }
    else if (event.type == ava::provider::StreamEventType::Done)
    {
      done = true;
      if (event.finish_reason)
        turn.finish_reason = event.finish_reason;
    }
    else if (event.type == ava::provider::StreamEventType::Error)
    {
      // Error payloads are provider-controlled even for third-party Provider
      // implementations. Keep the public/session-facing context local.
      auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "provider stream error");
      error.with_context("message", "provider streaming diagnostic omitted");
      return std::unexpected(std::move(error));
    }
  }

  if (!active_text_items.empty() || active_legacy_text_item)
    return std::unexpected(turn_error("provider response ended with an unbalanced text item lifecycle"));
  if (active_reasoning_item && active_reasoning_item->native_lifecycle)
    return std::unexpected(turn_error("provider response ended with an unbalanced reasoning item lifecycle"));
  for (auto const& [call_id, call] : active_function_calls)
  {
    static_cast<void>(call_id);
    // Legacy provider families historically signal a complete tool turn with
    // Done alone. Native output-item lifecycles must close explicitly.
    if (call.native_lifecycle && !call.completed)
      return std::unexpected(turn_error("provider response ended before a function call completed"));
  }
  if (!done)
  {
    auto const message = events.empty() ? "provider response was empty" : "provider response ended without a terminal outcome";
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, message));
  }
  if (!turn.finish_reason || *turn.finish_reason == ava::provider::ProviderFinishReason::Error)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "provider returned a failed or unrecognized terminal outcome"));
  }

  rebuild_legacy_aggregates(turn);
  if (*turn.finish_reason == ava::provider::ProviderFinishReason::ToolCalls && turn.tool_calls.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "provider reported tool calls without a valid tool call"));
  }
  return turn;
}

}  // namespace ava::agent
