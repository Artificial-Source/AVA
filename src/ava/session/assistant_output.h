#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include "debug.h"

namespace ava::session {
class SessionAppendTarget;
struct SessionEntry;
} // namespace ava::session

namespace ava::session {

inline constexpr std::size_t kMaxAssistantOutputItemsPerTurn = 4096;
inline constexpr std::size_t kCurrentAssistantOutputSchemaVersion = 1;

// The schema has an explicit unknown phase for provider families that do not
// expose the native Responses API distinction. New v4 text records must still
// write one of these closed values.
enum class AssistantOutputTextPhase
{
  Unknown,
  Commentary,
  FinalAnswer,
};

enum class AssistantOutputItemKind
{
  Text,
  Reasoning,
  FunctionCall,
};

struct AssistantOutputText
{
  ava::core::NoLogString text;
  AssistantOutputTextPhase assistant_phase = AssistantOutputTextPhase::Unknown;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct AssistantOutputReasoning
{
  ava::core::NoLogString text;
  std::string format;
  bool redacted = false;
  std::optional<ava::core::NoLogString> signature;
  std::optional<ava::core::NoLogString> redacted_data;
  std::optional<ava::core::NoLogString> native_item_json;
  // Portable archives intentionally remove provider-private replay material.
  // Marking that loss keeps imported reasoning on the safe text-only path.
  bool private_replay_metadata_omitted = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct AssistantOutputFunctionCall
{
  std::string call_id;
  std::string name;
  ava::core::NoLogString arguments_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using AssistantOutputItemPayload = std::variant<AssistantOutputText, AssistantOutputReasoning, AssistantOutputFunctionCall>;

struct AssistantOutputItem
{
  std::string assistant_turn_id;
  std::size_t sequence = 0;
  AssistantOutputItemKind kind = AssistantOutputItemKind::Text;
  std::optional<std::string> provider_item_id;
  std::optional<std::size_t> provider_output_index;
  AssistantOutputItemPayload payload;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Usage remains serialized in the same bounded JSON object shape emitted by
// agent/usage_accounting. Keeping the raw validated object preserves exact
// accounting metadata without teaching this read-side phase a new cost model.
struct AssistantTurnCommit
{
  std::string assistant_turn_id;
  std::size_t item_count = 0;
  std::string provider;
  std::string model;
  // Additive source-capability provenance for request-time replay decisions.
  // Legacy commits omit both fields and rely on a preceding session/model
  // snapshot; empty or contradictory values are never replay authority.
  std::optional<std::string> api_family = std::nullopt;
  std::optional<std::string> reasoning_format = std::nullopt;
  std::string finish_reason;
  std::optional<ava::core::NoLogString> usage_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class AssistantOutputDiagnosticSeverity
{
  Warning,
  Error,
};

enum class AssistantOutputDiagnosticKind
{
  InvalidAssistantOutputItem,
  InvalidAssistantTurnCommit,
  IncompleteAssistantTurn,
  MalformedAssistantTurn,
};

struct AssistantOutputDiagnostic
{
  AssistantOutputDiagnosticSeverity severity = AssistantOutputDiagnosticSeverity::Error;
  AssistantOutputDiagnosticKind kind = AssistantOutputDiagnosticKind::MalformedAssistantTurn;
  std::size_t entry_index = 0;
  std::string entry_id;
  ava::core::NoLogString message;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommittedAssistantOutputItem
{
  AssistantOutputItem item;
  std::size_t entry_index = 0;
  std::string entry_id;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommittedAssistantTurn
{
  std::size_t start_index = 0;
  std::size_t commit_index = 0;
  std::string commit_entry_id;
  AssistantTurnCommit commit;
  std::vector<CommittedAssistantOutputItem> items;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// The half-open physical-record range immediately after one committed v4 turn
// where its exact function results may appear. Audit/bookkeeping entries are
// allowed; a user/assistant message, another v4 item/commit, or compaction
// starts the next logical context and closes this window.
struct AssistantOutputToolResultWindow
{
  std::size_t begin_index = 0;
  std::size_t end_index = 0;

  [[nodiscard]] bool contains(std::size_t entry_index) const noexcept { return entry_index >= begin_index && entry_index < end_index; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Exact v4 function identity. A provider call_id alone is intentionally not
// enough to pair or recover a result because provider IDs are only logical
// identifiers; the physical assistant-output entry pins one committed call.
struct UnresolvedCommittedFunctionCall
{
  std::size_t committed_entry_index = 0;
  std::string assistant_output_entry_id;
  std::string call_id;
  std::string name;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct AssistantOutputItemReference
{
  std::size_t turn_index = 0;
  std::size_t item_index = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct AssistantOutputProjection
{
  std::vector<CommittedAssistantTurn> turns;
  std::vector<AssistantOutputDiagnostic> diagnostics;
  std::unordered_map<std::size_t, std::size_t> turn_indices_by_commit_index;
  std::unordered_map<std::string, AssistantOutputItemReference> item_references_by_entry_id;

  [[nodiscard]] CommittedAssistantTurn const* find_turn_by_commit_index(std::size_t commit_index) const noexcept;
  [[nodiscard]] CommittedAssistantTurn const* find_turn_by_output_entry_id(std::string_view entry_id) const noexcept;
  [[nodiscard]] CommittedAssistantOutputItem const* find_item_by_output_entry_id(std::string_view entry_id) const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string_view to_string(AssistantOutputTextPhase phase) noexcept;
[[nodiscard]] std::string_view to_string(AssistantOutputItemKind kind) noexcept;
[[nodiscard]] std::string_view to_string(AssistantOutputDiagnosticSeverity severity) noexcept;
[[nodiscard]] std::string_view to_string(AssistantOutputDiagnosticKind kind) noexcept;

// These functions are the sole strict v4 payload codecs. They validate the
// envelope type/version as well as every data field, and reject additive,
// duplicate, or variant-incompatible fields.
[[nodiscard]] ava::core::Result<std::string> serialize_assistant_output_item_data_json(AssistantOutputItem const& item);
[[nodiscard]] ava::core::Result<std::string> serialize_assistant_turn_commit_data_json(AssistantTurnCommit const& commit);
[[nodiscard]] ava::core::Result<AssistantOutputItem> parse_assistant_output_item(SessionEntry const& entry);
[[nodiscard]] ava::core::Result<AssistantTurnCommit> parse_assistant_turn_commit(SessionEntry const& entry);

// Classifies physical v4 records into only fully committed logical turns. A
// structurally valid final staged suffix is deliberately a warning and never
// becomes visible; malformed staged suffixes remain hard errors.
[[nodiscard]] AssistantOutputProjection classify_assistant_output(std::vector<SessionEntry> const& entries);
[[nodiscard]] AssistantOutputToolResultWindow committed_assistant_output_tool_result_window(std::vector<SessionEntry> const& entries,
                                                                                            CommittedAssistantTurn const& turn) noexcept;
// Returns only committed v4 function calls that have no exact later bound
// tool_result. A malformed v4 group or ambiguous/mismatched binding is an
// error; structurally valid final staging is deliberately ignored.
[[nodiscard]] ava::core::Result<std::vector<UnresolvedCommittedFunctionCall>> find_unresolved_committed_function_calls(
    std::vector<SessionEntry> const& entries);

// Internal append guard for v4 staged assistant output. Construction accepts
// only histories whose assistant-output structure is valid; a valid final
// staging prefix becomes the pending state. SessionAppendTarget owns v4 writes;
// SessionStore's raw ordinary-record compatibility path may only preflight a
// candidate under the same shared mutation serialization.
class AssistantOutputAppendState
{
 public:
  [[nodiscard]] static ava::core::Result<AssistantOutputAppendState> from_validated_history(std::vector<SessionEntry> const& entries);
  // Ready means no incomplete assistant-output transaction is staged. Batch
  // append reserves only one complete transaction from this state.
  [[nodiscard]] bool ready() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  enum class Mode
  {
    Closed,
    Pending,
  };

  [[nodiscard]] ava::core::VoidResult apply_candidate(SessionEntry const& entry);

  Mode mode_ = Mode::Closed;
  std::unordered_set<std::string> committed_turn_ids_;
  // Physical output entry identity is independent from provider identity and
  // must remain unique across every committed v4 turn.
  std::unordered_set<std::string> committed_output_entry_ids_;
  std::string pending_turn_id_;
  std::size_t pending_next_sequence_ = 0;
  std::unordered_set<std::string> pending_output_entry_ids_;
  std::unordered_set<std::string> pending_provider_item_ids_;
  std::unordered_set<std::size_t> pending_provider_output_indices_;

  friend class SessionAppendTarget;
  friend class SessionStore;
};

}  // namespace ava::session
