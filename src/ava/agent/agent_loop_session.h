#pragma once

#include "ava/agent/assistant_turn.h"
#include "ava/agent/tool_types.h"
#include "ava/tools/tool_context.h"
#include "ava/session/attachments.h"
#include "ava/session/session_store.h"
#include "ava/provider/provider.h"
#include "ava/core/result.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "debug.h"

namespace ava::agent {

using SessionAppendSink = std::function<ava::core::VoidResult(ava::session::SessionEntry)>;
using SessionAppendBatchSink = std::function<ava::core::VoidResult(std::vector<ava::session::SessionEntry>)>;

// The v4 assistant turn is visible only after its trailing commit is appended.
// Function results bind to the physical output-item entry rather than only the
// provider's logical call ID.
struct PersistedAssistantTurn
{
  // Exact SessionEntry id of the trailing AssistantTurnCommit. Consumers use
  // this durable transaction identity rather than inferring completion from
  // streamed text.
  std::string committed_turn_id;
  std::unordered_map<std::string, std::string> function_output_entry_ids_by_call_id;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<PersistedAssistantTurn> append_assistant_turn(SessionAppendBatchSink const& sink, ParsedAssistantTurn const& turn,
                                                                              std::string_view provider_id, std::string_view model_id,
                                                                              ava::provider::TokenUsage const& usage,
                                                                              std::optional<long double> const& cost_usd,
                                                                              std::optional<std::string_view> api_family = std::nullopt,
                                                                              std::optional<std::string_view> reasoning_format = std::nullopt);

[[nodiscard]] ava::core::Result<std::string> append_user_message(SessionAppendSink const& sink, std::string const& text,
                                                                 std::vector<ava::session::ImageAttachmentRef> const& attachments = {},
                                                                 std::optional<ava::session::SyntheticDeliveryProvenance> const& provenance = std::nullopt);
[[nodiscard]] ava::core::VoidResult append_replay_user_message(SessionAppendSink const& sink, std::string const& text,
                                                               std::vector<ava::session::ImageAttachmentRef> const& attachments, std::string const& replay_of);
[[nodiscard]] ava::core::VoidResult append_assistant_message(SessionAppendSink const& sink, std::string const& text, std::size_t tool_call_count,
                                                             ava::provider::TokenUsage const& usage, std::optional<long double> const& cost_usd);
[[nodiscard]] ava::core::VoidResult append_reasoning_block(SessionAppendSink const& sink, ParsedReasoningBlock const& block, std::string_view provider_id,
                                                           std::string_view model_id);
[[nodiscard]] ava::core::VoidResult append_tool_call(SessionAppendSink const& sink, ProviderToolCall const& call);
[[nodiscard]] ava::core::VoidResult append_tool_result(SessionAppendSink const& sink, ToolDispatchResult const& result,
                                                       std::optional<std::string_view> assistant_output_entry_id = std::nullopt);
[[nodiscard]] ava::core::VoidResult append_permission_decision(SessionAppendSink const& sink, ava::tools::PermissionAuditEvent const& event);
[[nodiscard]] ava::core::VoidResult append_error(SessionAppendSink const& sink, ava::core::Error const& error);
[[nodiscard]] ava::core::VoidResult append_cancel(SessionAppendSink const& sink, std::string_view boundary);

// Synthetic terminal results are used only to close already committed v4
// function calls after cancellation/interruption. They never execute a tool
// and explicitly say that an unknown outcome must not be retried automatically.
[[nodiscard]] ToolDispatchResult synthetic_terminal_tool_result(ProviderToolCall const& call, ToolResultStatus status);
[[nodiscard]] ava::core::VoidResult reconcile_unresolved_committed_function_calls(ava::session::SessionReadAuthority const& read_authority,
                                                                                  SessionAppendSink const& sink, ava::session::SessionReadLimits limits);

}  // namespace ava::agent
