#pragma once
#include "ava/debug/print_members_on.h"
#include "ava/agent/tool_types.h"
#include "ava/provider/provider.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <variant>
#include <vector>
#include "debug.h"

namespace ava::agent {

struct ParsedReasoningBlock
{
  ava::core::NoLogString text;
  std::string format;
  ava::core::NoLogString signature;
  ava::core::NoLogString redacted_data;
  // Opaque provider-only JSON retained only for exact native replay.
  ava::core::NoLogString native_item_json;
  bool redacted = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Provider item identity is separate from a function's logical call ID.
// Unknown phase is retained only for provider families without native phase
// metadata; OpenAI Responses message lifecycles validate known phases before
// reaching this assembler.
struct AssistantItemMetadata
{
  std::string provider_item_id = {};
  std::optional<std::size_t> provider_output_index = std::nullopt;
  ava::provider::AssistantPhase phase = ava::provider::AssistantPhase::Unknown;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct AssistantTextItem
{
  AssistantItemMetadata metadata = {};
  ava::core::NoLogString text = {};

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct AssistantReasoningItem
{
  AssistantItemMetadata metadata = {};
  ParsedReasoningBlock reasoning = {};

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct AssistantFunctionCallItem
{
  AssistantItemMetadata metadata = {};
  ProviderToolCall tool_call = {};

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using AssistantItem = std::variant<AssistantTextItem, AssistantReasoningItem, AssistantFunctionCallItem>;

struct OrderedAssistantItem
{
  std::size_t sequence = 0;
  AssistantItem item = AssistantTextItem{};

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ParsedAssistantTurn
{
  // This is the sole mutable representation produced by the parser. The
  // legacy aggregate fields below are rebuilt from it once at completion.
  std::vector<OrderedAssistantItem> ordered_items;
  ava::core::NoLogString text;
  std::vector<ParsedReasoningBlock> reasoning_blocks;
  std::vector<ProviderToolCall> tool_calls;
  std::optional<ava::provider::TokenUsage> usage;
  std::optional<ava::provider::ProviderFinishReason> finish_reason;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ProviderOutputLimits
{
  std::size_t max_events = 0;
  std::size_t max_assistant_text_bytes = 0;
  std::size_t max_tool_argument_bytes = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<ParsedAssistantTurn> parse_assistant_turn(std::vector<ava::provider::StreamEvent> const& events, ProviderOutputLimits limits);

}  // namespace ava::agent
