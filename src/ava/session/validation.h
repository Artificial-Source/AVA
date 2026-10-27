#pragma once

#include "ava/session/session_store.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ava::session {

enum class SessionReplayIssueSeverity
{
  Warning,
  Error,
};

enum class SessionReplayIssueKind
{
  UnsupportedEntryVersion,
  DuplicateEntryId,
  UnknownParentId,
  EmptyToolCallId,
  DuplicateToolCallId,
  ToolResultWithoutCall,
  ToolResultToolMismatch,
  DuplicateToolResult,
  UnresolvedToolCall,
  MissingStructuredToolResult,
  InvalidStructuredToolResult,
  StructuredToolResultMismatch,
  InvalidPermissionDecision,
  PermissionResolutionWithoutAsk,
  UnresolvedPermissionPrompt,
  InvalidCompactionEntry,
  InvalidSessionMetadataEntry,
  InvalidBranchSummaryEntry,
  InvalidMessageEntry,
  CompactionWithUnresolvedToolCall,
  CompactionWithUnresolvedPermissionPrompt,
  InvalidModelEntry,
  InvalidReasoningEntry,
  InvalidAssistantOutputItem,
  InvalidAssistantTurnCommit,
  IncompleteAssistantTurn,
  MalformedAssistantTurn,
  ToolResultOutputItemMismatch,
};

struct SessionReplayIssue
{
  SessionReplayIssueSeverity severity = SessionReplayIssueSeverity::Error;
  SessionReplayIssueKind kind = SessionReplayIssueKind::DuplicateEntryId;
  std::size_t entry_index = 0;
  std::string entry_id;
  std::string call_id;
  ava::core::NoLogString message;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionReplayValidationOptions
{
  bool require_entry_versions = true;
  bool require_known_parent_ids = true;
  bool require_tool_result_pairing = true;
  bool require_permission_decision_integrity = true;
  bool require_compaction_integrity = true;
  bool require_model_reasoning_integrity = true;
  bool require_structured_tool_results = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionReplayValidation
{
  std::vector<SessionReplayIssue> issues;
  std::size_t error_count = 0;
  std::size_t warning_count = 0;

  [[nodiscard]] bool ok() const noexcept { return error_count == 0; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string_view to_string(SessionReplayIssueSeverity severity) noexcept;
[[nodiscard]] std::string_view to_string(SessionReplayIssueKind kind) noexcept;
[[nodiscard]] std::string sanitized_message_data_json(std::string_view data_json, bool allow_attachments = true);
[[nodiscard]] SessionReplayValidation validate_session_replay(std::vector<SessionEntry> const& entries, SessionReplayValidationOptions options = {});

}  // namespace ava::session
