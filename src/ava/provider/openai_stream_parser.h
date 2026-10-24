#pragma once

#include "ava/provider/parser_budget.h"
#include "ava/provider/provider.h"
#include "ava/core/result.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "debug.h"

namespace ava::provider {

class OpenAIStreamParser final : public StreamParser
{
 public:
  // Responses identifies the streamed item with an fc_ ID while callers
  // dispatch and replay through the separate opaque logical call_id.
  struct FunctionCallState
  {
    std::string provider_item_id;
    std::optional<std::size_t> provider_output_index = std::nullopt;
    std::string logical_call_id;
    std::string name;
    std::string arguments;
    std::size_t emitted_argument_bytes = 0;
    bool started = false;
    bool ended = false;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  struct MessageItemState
  {
    std::optional<std::size_t> provider_output_index = std::nullopt;
    AssistantPhase phase = AssistantPhase::Unknown;
    // Responses exposes independent output_text and refusal content parts on
    // one message item. Retain separate completion state so a refusal.done
    // never conflicts with a preceding output_text.done.
    std::string text;
    std::string output_text;
    std::string refusal_text;
    bool output_text_completed = false;
    bool refusal_completed = false;
    bool ended = false;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> append(std::string_view chunk) override;
  [[nodiscard]] ava::core::Result<std::vector<StreamEvent>> finish() override;

  // Framing buffers and native reasoning payloads remain private.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  enum class EventHandling
  {
    Unhandled,
    Handled,
  };

  enum class OutputItemLifecycle
  {
    Added,
    Done,
  };

  enum class MessageEventSource
  {
    Legacy,
    Documented,
  };

  enum class EventRoutingPhase
  {
    BeforeIgnoredLifecycle,
    AfterIgnoredLifecycle,
  };

  void append_event_for_data(std::vector<StreamEvent>& events, std::string_view data);
  [[nodiscard]] EventHandling handle_documented_output_item_lifecycle(std::vector<StreamEvent>& events, std::string_view data, std::string_view type);
  [[nodiscard]] EventHandling handle_message_event(std::vector<StreamEvent>& events, std::string_view data, std::string_view type,
                                                   EventRoutingPhase routing_phase);
  [[nodiscard]] EventHandling handle_reasoning_event(std::vector<StreamEvent>& events, std::string_view data, std::string_view type);
  [[nodiscard]] EventHandling handle_function_call_event(std::vector<StreamEvent>& events, std::string_view data, std::string_view type,
                                                         EventRoutingPhase routing_phase);
  [[nodiscard]] EventHandling handle_terminal_event(std::vector<StreamEvent>& events, std::string_view data, std::string_view type);

  void handle_documented_message_output_item(std::vector<StreamEvent>& events, std::string_view item, std::string const& item_id,
                                             std::optional<std::size_t> output_index, AssistantPhase phase, OutputItemLifecycle lifecycle);
  void handle_documented_reasoning_output_item(std::vector<StreamEvent>& events, std::string const& item, std::string const& item_id,
                                               std::optional<std::size_t> output_index, OutputItemLifecycle lifecycle);
  void handle_documented_function_call_output_item(std::vector<StreamEvent>& events, std::string_view data, std::string_view item,
                                                   std::optional<std::size_t> output_index, OutputItemLifecycle lifecycle);

  [[nodiscard]] std::pair<MessageEventSource, MessageItemState*> documented_message_item_for_event(std::vector<StreamEvent>& events, std::string_view data);
  [[nodiscard]] bool reject_unended_documented_message_items(std::vector<StreamEvent>& events);

  void append_start_reasoning_if_needed(std::vector<StreamEvent>& events);
  void append_reasoning_delta(std::vector<StreamEvent>& events, std::string_view text);
  [[nodiscard]] bool reconcile_complete_reasoning_text(std::vector<StreamEvent>& events, std::string_view complete_text);
  void append_finish_reasoning_if_open(std::vector<StreamEvent>& events, std::string native_item_json = {});
  [[nodiscard]] bool bind_documented_reasoning_event(std::vector<StreamEvent>& events, std::string_view data);

  [[nodiscard]] FunctionCallState* bind_documented_function_call_item(std::vector<StreamEvent>& events, std::string_view item,
                                                                      std::optional<std::size_t> provider_output_index);
  [[nodiscard]] FunctionCallState* documented_function_call_state_for_event(std::vector<StreamEvent>& events, std::string_view data);
  [[nodiscard]] bool reject_unended_documented_function_calls(std::vector<StreamEvent>& events);

  void reset_state() noexcept;

  ProviderSseBuffer sse_;
  bool saw_content_ = false;
  bool saw_refusal_ = false;
  bool reasoning_open_ = false;
  bool reasoning_text_seen_ = false;
  std::string active_reasoning_item_id_;
  std::optional<std::size_t> active_reasoning_output_index_ = std::nullopt;
  std::string active_reasoning_text_;
  std::vector<std::string> completed_reasoning_item_ids_;
  std::vector<std::string> completed_reasoning_texts_;
  // Documented output-item events bind these maps once. Legacy event-family
  // state remains separate so it cannot turn an item ID into a logical call ID.
  std::unordered_map<std::string, FunctionCallState> function_calls_;
  std::unordered_map<std::string, std::string> function_call_item_ids_by_logical_id_;
  std::unordered_map<std::string, FunctionCallState> legacy_function_calls_;
  std::unordered_map<std::string, MessageItemState> message_items_;
  std::unordered_map<std::string, std::string> documented_output_item_types_;
  std::unordered_set<std::string> documented_output_item_added_ids_;
  std::unordered_map<std::size_t, std::string> documented_output_item_ids_by_index_;
  bool done_seen_ = false;
  bool error_seen_ = false;
};

}  // namespace ava::provider
