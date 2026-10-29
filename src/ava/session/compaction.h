#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/session_store.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::session {

struct CompactionConfig
{
  std::size_t auto_threshold_tokens = 0;
  bool auto_threshold_tokens_explicit = false;
  std::size_t auto_threshold_percent = 80;
  bool auto_threshold_percent_explicit = false;
  std::size_t keep_recent_tokens = 20'000;
  std::size_t keep_recent_turns = 2;
  bool keep_recent_turns_explicit = false;
  std::size_t keep_recent_messages = 0;
  bool keep_recent_messages_explicit = false;
  std::string provider_id;
  // Compatibility fallback for direct API callers. Runtime resolution replaces
  // this whenever the model was not explicitly configured.
  std::string model_id = "gpt-5.5";
  bool provider_explicit = false;
  bool model_explicit = false;
  std::size_t max_summary_bytes = 16 * 1024;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CompactionDecision
{
  bool should_compact = false;
  std::size_t estimated_tokens = 0;
  std::size_t threshold_tokens = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ManualCompactionRequest
{
  ava::core::NoLogString summary;
  ava::core::NoLogString instructions;
  CompactionConfig config;
  std::size_t estimated_tokens = 0;
  std::size_t threshold_tokens = 0;
  std::size_t retained_tokens = 0;
  std::string trigger = "manual";
  ava::core::NoLogString recent_context;
  bool recent_context_omitted = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] CompactionConfig default_compaction_config();
[[nodiscard]] ava::core::Result<CompactionConfig> parse_compaction_config(std::string_view content);
[[nodiscard]] ava::core::Result<CompactionConfig> load_compaction_config(ava::config::XdgPaths const& paths);
[[nodiscard]] std::size_t estimate_tokens(std::string_view text) noexcept;
// Estimates the shared logical/public projection, so committed v4 turns count
// while uncommitted staging and provider-private payloads never affect a
// compaction decision.
[[nodiscard]] ava::core::Result<std::size_t> estimate_session_tokens(std::vector<SessionEntry> const& entries);
// Returns the latest valid compaction checkpoint and everything after it, or
// the complete ordered public history when no checkpoint exists.
[[nodiscard]] ava::core::Result<std::vector<SessionEntry>> project_active_compaction_context(std::vector<SessionEntry> const& entries);
[[nodiscard]] ava::core::Result<std::size_t> estimate_active_context_tokens(std::vector<SessionEntry> const& entries);
// Exact ordered compaction compare-and-swap matcher. Every persisted entry
// field participates, including the record version. The only accepted suffix
// is strictly validated, context-neutral automatic-title metadata.
[[nodiscard]] bool compaction_snapshot_matches(std::vector<SessionEntry> const& expected, std::vector<SessionEntry> const& actual);
[[nodiscard]] std::size_t effective_auto_threshold_tokens(CompactionConfig const& config, std::optional<long long> context_window_tokens) noexcept;
[[nodiscard]] ava::core::Result<CompactionDecision> should_auto_compact(std::vector<SessionEntry> const& entries, CompactionConfig const& config);
[[nodiscard]] ava::core::Result<CompactionDecision> should_auto_compact(std::vector<SessionEntry> const& entries, CompactionConfig const& config,
                                                                        std::optional<long long> context_window_tokens);
[[nodiscard]] ava::core::Result<SessionEntry> make_manual_compaction_entry(ManualCompactionRequest request);
[[nodiscard]] ava::core::VoidResult append_manual_compaction(SessionStore& store, SessionLease const& lease, ManualCompactionRequest request);
[[nodiscard]] ava::core::VoidResult append_manual_compaction_ephemeral(SessionStore& store, ManualCompactionRequest request);

}  // namespace ava::session
