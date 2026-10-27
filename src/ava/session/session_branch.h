#pragma once

#include "ava/session/session_metadata.h"
#include "ava/session/session_store.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::session {

enum class SessionBranchMode
{
  Fork,
  Clone
};

struct SessionBranchOptions
{
  std::filesystem::path workspace_dir;
  std::filesystem::path root_dir;
  std::string source_session_id;
  std::string branch_from_entry_id;
  std::optional<ava::core::NoLogString> name;
  std::optional<std::vector<ava::core::NoLogString>> labels;
  std::optional<SessionReadLimits> read_limits = std::nullopt;
  // Retained by callers that already own the source while preparing a branch.
  SessionLease const* source_lease = nullptr;
  SessionBranchMode mode = SessionBranchMode::Fork;
  std::string actor = "rpc";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionBranchResult
{
  SessionStore store;
  SessionLease lease;
  std::string source_session_id;
  std::string branch_from_entry_id;
  std::size_t copied_entry_count = 0;
  SessionMetadataView metadata;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class BranchSummaryEligibilityReason
{
  Eligible,
  ForkEntryNotFound,
  NoSubstantiveEntriesAfterFork,
  ExistingSummary,
};

// Pure candidate projection over one already bounded, validated source load.
// BranchSummary markers are excluded from the prompt indices and never advance
// the candidate root/tip identity.
struct BranchSummaryCoverage
{
  std::string source_session_id;
  std::string fork_entry_id;
  std::string branch_root_entry_id;
  std::string branch_tip_entry_id;
  std::vector<std::size_t> prompt_entry_indices;
  std::optional<SessionEntry> existing_summary;
  BranchSummaryEligibilityReason reason = BranchSummaryEligibilityReason::Eligible;

  [[nodiscard]] bool eligible() const noexcept { return reason == BranchSummaryEligibilityReason::Eligible; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class BranchSummaryDisposition
{
  Appended,
  Existing,
};

struct BranchSummaryOptions
{
  std::filesystem::path workspace_dir;
  std::filesystem::path root_dir;
  std::string source_session_id;
  std::string branch_root_entry_id;
  std::string branch_tip_entry_id;
  ava::core::NoLogString summary;
  std::string provider;
  std::string model;
  ava::core::NoLogString reason;
  // Required explicitly by every caller. The optional representation keeps
  // aggregate initialization source-compatible while omission fails closed.
  std::optional<SessionReadLimits> read_limits = std::nullopt;
  // The caller retains this exact source lease across snapshot validation and
  // the summary append. When omitted, this helper acquires one itself.
  SessionLease const* source_lease = nullptr;
  SessionCancelCallback cancel_requested = nullptr;
  std::string actor = "rpc";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct BranchSummaryResult
{
  std::string source_session_id;
  SessionEntry entry;
  BranchSummaryDisposition disposition = BranchSummaryDisposition::Appended;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Roll back only a newly-created session JSONL file while preserving any attachment tree in place.
// The primary error remains authoritative; rollback observations are appended as context.
void rollback_created_session_with_context(SessionStore const& store, SessionLease const& lease, ava::core::Error& error);

[[nodiscard]] ava::core::Result<SessionBranchResult> create_session_branch(SessionBranchOptions options);
[[nodiscard]] std::string_view branch_summary_eligibility_reason_text(BranchSummaryEligibilityReason reason) noexcept;
[[nodiscard]] BranchSummaryCoverage inspect_branch_summary_coverage(std::vector<SessionEntry> const& entries, std::string_view source_session_id,
                                                                    std::string_view fork_entry_id);
[[nodiscard]] ava::core::Result<std::vector<SessionEntry>> extract_branch_summary_prompt_range(std::vector<SessionEntry> const& entries,
                                                                                               BranchSummaryCoverage const& coverage);
// Builds and validates a summary from two bounded lease-bound source snapshots
// without mutating the source. Runtime owners use this to append through their
// owner route. Exact existing identities return disposition Existing.
[[nodiscard]] ava::core::Result<BranchSummaryResult> prepare_branch_summary(BranchSummaryOptions options);
// Non-runtime sources append through a policy-bound SessionAppendTarget that
// retains a duplicate of the exact temporary source lease.
[[nodiscard]] ava::core::Result<BranchSummaryResult> append_branch_summary(BranchSummaryOptions options);

}  // namespace ava::session
