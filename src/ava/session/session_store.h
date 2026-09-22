#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/observability/trace_context.h"
#include "ava/session/assistant_output.h"
#include "ava/core/result.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

namespace ava::observability {
class RunObservation;
}

namespace ava::app {
class SessionRunController;
}

namespace ava::session {

inline constexpr long long kCurrentSessionEntryVersion = 4;
inline constexpr std::size_t kMaxSessionAppendBatchEntries = kMaxAssistantOutputItemsPerTurn + 1;
inline constexpr std::size_t kMaxSessionAppendBatchBytes = 4U * 1024U * 1024U;

enum class EntryType
{
  SessionStart,
  SessionMetadata,
  UserMessage,
  AssistantMessage,
  ToolCall,
  ToolResult,
  PermissionDecision,
  ModeChange,
  ModelChange,
  ReasoningBlock,
  ReasoningChange,
  AssistantOutputItem,
  AssistantTurnCommit,
  Compaction,
  BranchSummary,
  Error,
  Cancel,
};

inline constexpr std::string_view kSyntheticSubagentDeliverySource = "synthetic_subagent_delivery";
inline constexpr std::size_t kMaxSyntheticDeliveryProvenanceIdBytes = 96;

struct SyntheticDeliveryProvenance
{
  std::string delivery_id;
  std::string prompt_fingerprint;
  std::string source = std::string(kSyntheticSubagentDeliverySource);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionEntry
{
  std::string id;
  std::string parent_id;
  EntryType type;
  std::string timestamp;
  std::string data_json;
  long long version = kCurrentSessionEntryVersion;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionStoreOptions
{
  std::filesystem::path root_dir;
  std::filesystem::path workspace_dir;
  std::string session_id;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionReadLimits
{
  std::size_t max_file_bytes = 8U * 1024U * 1024U;
  std::size_t max_line_bytes = 1024U * 1024U;
  std::size_t max_entries = 16384;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionListLimits
{
  SessionReadLimits per_session = {};
  std::size_t max_sessions = 4096;
  std::size_t max_total_file_bytes = 32U * 1024U * 1024U;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Collision-resistant identity of every exact byte in one bounded leased
// snapshot. It deliberately excludes inode metadata so consent can remain
// content-bound across an identical exact-path replacement.
struct SessionByteFingerprint
{
  std::uint64_t byte_count = 0;
  std::array<std::uint8_t, 32> sha256 = {};

  [[nodiscard]] friend bool operator==(SessionByteFingerprint const&, SessionByteFingerprint const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionRecoverableSnapshot
{
  std::vector<SessionEntry> entries;
  SessionByteFingerprint fingerprint;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Opening/resuming is the only recovery boundary for a complete but
// uncommitted v4 assistant-output suffix. Persistent recovery quarantines the
// exact bytes before truncating them; ephemeral recovery removes only the
// proven in-memory entries and has no quarantine path.
struct AssistantOutputSuffixRecovery
{
  std::optional<std::filesystem::path> quarantine_path = std::nullopt;
  std::size_t removed_entry_count = 0;
  std::size_t removed_byte_count = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using SessionCancelCallback = std::function<bool()>;
using SessionEntryVisitor = std::function<ava::core::Result<bool>(SessionEntry const&)>;

// Legacy CLI/TUI/RPC reads retain their historical unbounded file/entry policy
// while still enforcing the hard session-line and strict JSON nesting bounds.
[[nodiscard]] SessionReadLimits legacy_unbounded_session_read_limits();

// Legacy ordinary user messages have no provenance. When present, synthetic
// delivery provenance is strict, bounded backend metadata and malformed
// objects are rejected rather than interpreted as user text markers.
[[nodiscard]] ava::core::Result<std::optional<SyntheticDeliveryProvenance>> parse_synthetic_delivery_provenance(SessionEntry const& entry);

struct SessionSummary
{
  std::string session_id = {};
  std::filesystem::path path = {};
  std::string last_updated = {};
  std::size_t entry_count = 0;
  std::filesystem::path original_cwd = {};
  std::string title = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Exclusive advisory lease for one regular session file. The originally
// requested final path component is opened with O_NOFOLLOW; the descriptor is
// CLOEXEC and the lock is released automatically on destruction.
class SessionAppendTarget;
class SessionReadAuthority;

class SessionLease
{
 public:
  SessionLease() = default;
  SessionLease(SessionLease const&) = delete;
  SessionLease& operator=(SessionLease const&) = delete;
  SessionLease(SessionLease&& other) noexcept;
  SessionLease& operator=(SessionLease&& other) noexcept;
  ~SessionLease();

  [[nodiscard]] static ava::core::Result<SessionLease> create_and_acquire(std::filesystem::path const& session_path);
  [[nodiscard]] static ava::core::Result<SessionLease> acquire(std::filesystem::path const& session_path);
  [[nodiscard]] bool active() const noexcept;
  // Duplicates the already locked open-file description with CLOEXEC. This
  // never reacquires by pathname and therefore preserves exact inode/lease
  // identity for detached runtime ownership.
  [[nodiscard]] ava::core::Result<SessionLease> duplicate() const;
  [[nodiscard]] std::filesystem::path const& canonical_path() const noexcept;
  // Test-only observation used to prove lease-bound pread snapshots do not
  // mutate the shared open-file-description offset.
  [[nodiscard]] off_t offset_for_test() const noexcept;

  void swap(SessionLease& other);

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  friend class SessionStore;
  friend class SessionAppendTarget;
  friend class SessionReadAuthority;

  SessionLease(int fd, std::filesystem::path canonical_path, bool created);
  int fd_ = -1;
  std::filesystem::path canonical_path_;
  bool created_ = false;
};

class SessionStore
{
 public:
  explicit SessionStore(SessionStoreOptions options);

  [[nodiscard]] std::string const& session_id() const noexcept;
  [[nodiscard]] std::filesystem::path session_path() const;
  [[nodiscard]] bool is_ephemeral() const noexcept;

  // Observation is non-authoritative and never serialized into session JSONL.
  // Copies of a store share this attachment. A generation lets a finished stale
  // run clear only the attachment it installed, never a newer run's attachment.
  // This is a no-throw best-effort transaction. It returns zero on a disabled
  // observer or setup failure and leaves any existing attachment unchanged.
  [[nodiscard]] std::uint64_t set_run_observation(std::shared_ptr<ava::observability::RunObservation> const& observation,
                                                  ava::observability::TraceContext const& context = {}) noexcept;
  void clear_run_observation(std::uint64_t generation) noexcept;
  // Test-only deterministic fault injection for the no-throw attachment path.
  void fail_next_run_observation_attachment_for_test() noexcept;
  // A background persistence copy intentionally has no live trace attachment.
  // It may write the same session, but cannot inherit or clear a newer run.
  [[nodiscard]] SessionStore detached_copy_for_background_persistence() const;

  // Persistent stores require the active exact matching lease. Ephemeral
  // stores deliberately use the explicitly named in-memory path below. These
  // raw bootstrap/compat mutation APIs reject v4 assistant-output records and
  // preflight ordinary records against the current v4 state, so they cannot
  // split a staged assistant transaction. Runtime v4 writes must use
  // SessionAppendTarget so preflight remains coupled to persistence.
  [[nodiscard]] ava::core::VoidResult append(SessionLease const& lease, SessionEntry const& entry);
  [[nodiscard]] ava::core::VoidResult append_ephemeral(SessionEntry const& entry);
  // Copies one already-validated complete history into a newly created empty
  // persistent destination while retaining its creating lease. This is the
  // only v4 bypass for import/branch copy-forward; it prevalidates the whole
  // copy and holds per-path serialization through all durable writes.
  [[nodiscard]] ava::core::VoidResult append_validated_copy(SessionLease const& lease, std::vector<SessionEntry> const& entries);
  // Removes a newly-created persistent session only when the supplied active
  // lease still identifies its sole linked pathname. Intended for rollback
  // before ownership is transferred into a runtime::Session.
  [[nodiscard]] ava::core::VoidResult remove_created_file(SessionLease const& lease) const;
  // Test-only deterministic race seams. Production callers must not install them.
  void set_before_append_identity_check_for_test(std::function<void()> hook);
  void set_after_append_write_for_test(std::function<void()> hook);
  void set_after_lease_bound_read_for_test(std::function<void()> hook);
  void fail_persistent_append_target_allocation_for_test(bool fail = true) noexcept;
  void fail_persistent_read_authority_allocation_for_test(bool fail = true) noexcept;
  // Test-only write seam. A negative result is treated like write(2) failure
  // with errno supplied by the hook, allowing deterministic torn-tail tests.
  void set_append_write_for_test(std::function<ssize_t(int, std::string_view)> hook);
  void set_before_recovery_quarantine_publication_for_test(std::function<void(std::filesystem::path const&)> hook);
  void set_after_recovery_quarantine_publication_for_test(std::function<void()> hook);
  void set_before_created_file_rollback_detach_for_test(std::function<void()> hook);
  void set_after_created_file_rollback_detach_for_test(std::function<void()> hook);
  // Repairs only a single incomplete final JSONL record while the matching
  // exclusive lease is held. All scanning and mutation are subject to explicit
  // read limits and cancellation. A returned path names the quarantined exact
  // tail; no path means the file was already framed or only needed a final LF.
  [[nodiscard]] ava::core::Result<std::optional<std::filesystem::path>> recover_torn_tail(SessionLease const& lease, SessionReadLimits limits,
                                                                                          SessionCancelCallback cancel_requested = nullptr) const;
  // Repairs only a structurally valid final complete-line v4 staging suffix
  // after torn-tail recovery. Persistent callers must retain the exact active
  // lease; the ephemeral overload exists only for an in-memory session.
  [[nodiscard]] ava::core::Result<std::optional<AssistantOutputSuffixRecovery>> recover_incomplete_assistant_output_suffix(
      SessionLease const& lease, SessionReadLimits limits, SessionCancelCallback cancel_requested = nullptr) const;
  [[nodiscard]] ava::core::Result<std::optional<AssistantOutputSuffixRecovery>> recover_incomplete_assistant_output_suffix(
      SessionReadLimits limits, SessionCancelCallback cancel_requested = nullptr) const;
  [[nodiscard]] ava::core::Result<std::vector<SessionEntry>> load() const;
  // Authority-sensitive persistent reads are pinned to the active leased inode
  // and to the exact canonical parent/name publication for their full snapshot.
  [[nodiscard]] ava::core::Result<std::vector<SessionEntry>> load(SessionLease const& lease) const;
  [[nodiscard]] ava::core::Result<std::vector<SessionEntry>> load_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested = nullptr) const;
  [[nodiscard]] ava::core::Result<std::vector<SessionEntry>> load_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                          SessionCancelCallback cancel_requested = nullptr) const;
  // Read-only recovery preview pinned to the exact leased inode. Every consumed
  // record is strict; only an invalid final unterminated suffix is omitted so a
  // caller can decide whether to request explicit lease-gated recovery later.
  [[nodiscard]] ava::core::Result<std::vector<SessionEntry>> load_recoverable_prefix_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                                             SessionCancelCallback cancel_requested = nullptr) const;
  // Captures the same strict recoverable prefix plus a SHA-256 identity of all
  // exact bytes (including an invalid final unterminated suffix) in one stable,
  // size-bounded leased snapshot.
  [[nodiscard]] ava::core::Result<SessionRecoverableSnapshot> load_recoverable_snapshot_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                                                SessionCancelCallback cancel_requested = nullptr) const;
  [[nodiscard]] ava::core::VoidResult visit_entries(SessionReadLimits limits, SessionEntryVisitor const& visitor,
                                                    SessionCancelCallback cancel_requested = nullptr) const;
  [[nodiscard]] ava::core::Result<SessionSummary> inspect_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested = nullptr) const;
  [[nodiscard]] ava::core::Result<SessionSummary> inspect_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                  SessionCancelCallback cancel_requested = nullptr) const;

  [[nodiscard]] static ava::core::Result<SessionStore> create(std::filesystem::path const& workspace_dir,
                                                              std::filesystem::path const& root_dir = default_root_dir());
  [[nodiscard]] static ava::core::Result<SessionStore> create_ephemeral(std::filesystem::path const& workspace_dir);
  [[nodiscard]] static ava::core::Result<SessionStore> open(std::filesystem::path const& workspace_dir, std::string session_id,
                                                            std::filesystem::path const& root_dir = default_root_dir());
  [[nodiscard]] static ava::core::Result<std::vector<SessionSummary>> list_sessions(std::filesystem::path const& workspace_dir,
                                                                                    std::filesystem::path const& root_dir = default_root_dir());
  [[nodiscard]] static ava::core::Result<std::vector<SessionSummary>> list_sessions_bounded(std::filesystem::path const& workspace_dir,
                                                                                            std::filesystem::path const& root_dir, SessionListLimits limits,
                                                                                            SessionCancelCallback cancel_requested = nullptr);
  [[nodiscard]] static std::filesystem::path default_root_dir();

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  friend class SessionAppendTarget;
  friend class SessionReadAuthority;
  struct EphemeralState;
  struct ObservationAttachment;

  [[nodiscard]] ava::core::Result<SessionSummary> inspect_bounded_for_listing(SessionReadLimits limits, bool inspect_metadata,
                                                                              SessionCancelCallback cancel_requested = nullptr) const;
  [[nodiscard]] ava::core::VoidResult visit_entries_leased(SessionLease const& lease, SessionReadLimits limits, SessionEntryVisitor const& visitor,
                                                           SessionCancelCallback cancel_requested = nullptr,
                                                           bool invoke_after_lease_bound_read_test_hook = true, bool tolerate_invalid_final_suffix = false,
                                                           SessionByteFingerprint* exact_fingerprint = nullptr) const;
  [[nodiscard]] ava::core::Result<std::optional<AssistantOutputSuffixRecovery>> recover_incomplete_assistant_output_suffix_ephemeral_impl(
      SessionReadLimits limits, SessionCancelCallback cancel_requested, bool mutation_serialization_held) const;
  [[nodiscard]] ava::core::VoidResult append_impl(SessionLease const* lease, SessionEntry const& entry, bool append_serialization_held = false);
  explicit SessionStore(SessionStoreOptions options, std::shared_ptr<EphemeralState> ephemeral_state);

  SessionStoreOptions options_;
  std::shared_ptr<EphemeralState> ephemeral_state_;
  std::shared_ptr<ObservationAttachment> observation_attachment_;
  std::function<void()> before_append_identity_check_for_test_;
  std::function<void()> after_append_write_for_test_;
  std::function<void()> after_lease_bound_read_for_test_;
  std::function<ssize_t(int, std::string_view)> append_write_for_test_;
  bool fail_persistent_append_target_allocation_for_test_ = false;
  bool fail_persistent_read_authority_allocation_for_test_ = false;
  std::function<void(std::filesystem::path const&)> before_recovery_quarantine_publication_for_test_;
  std::function<void()> after_recovery_quarantine_publication_for_test_;
  std::function<void()> before_created_file_rollback_detach_for_test_;
  std::function<void()> after_created_file_rollback_detach_for_test_;
};

// Lease-bound or ephemeral content identity used to skip redundant history
// loads when a caller already holds a fresh projection. Equality means no
// observable content change on the exact authority target.
struct SessionContentFingerprint
{
  bool ephemeral = false;
  std::uint64_t dev = 0;
  std::uint64_t ino = 0;
  std::uint64_t size = 0;
  std::int64_t mtime_sec = 0;
  std::int64_t mtime_nsec = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t tip_hash = 0;

  [[nodiscard]] friend bool operator==(SessionContentFingerprint const&, SessionContentFingerprint const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Narrow copyable history authority for one runtime session. Persistent
// authorities share an owned F_DUPFD_CLOEXEC duplicate of the exact active
// lease; ephemeral authorities retain only the shared in-memory store state.
// Each authority also owns the read policy selected when its runtime session
// was opened, so ordinary load() calls cannot silently bypass that policy.
// Caller-supplied load_bounded/inspect_bounded limits are clamped per field to
// min(authority policy, request) and never widen the embedded policy.
class SessionReadAuthority
{
 public:
  SessionReadAuthority(SessionReadAuthority const&) noexcept = default;
  SessionReadAuthority& operator=(SessionReadAuthority const&) noexcept = default;
  SessionReadAuthority(SessionReadAuthority&&) noexcept = default;
  SessionReadAuthority& operator=(SessionReadAuthority&&) noexcept = default;
  ~SessionReadAuthority() = default;

  [[nodiscard]] static ava::core::Result<SessionReadAuthority> create_persistent(SessionStore const& store, SessionLease const& lease,
                                                                                 SessionReadLimits limits = legacy_unbounded_session_read_limits());
  [[nodiscard]] static ava::core::Result<SessionReadAuthority> create_ephemeral(SessionStore const& store,
                                                                                SessionReadLimits limits = legacy_unbounded_session_read_limits());

  [[nodiscard]] std::string const& session_id() const noexcept;
  [[nodiscard]] bool is_ephemeral() const noexcept;
  [[nodiscard]] bool active() const noexcept;
  // Persistent fingerprints are fstat(2) of the owned lease descriptor only.
  // Ephemeral fingerprints are an append-only in-memory tip summary under the
  // store entries lock (not durable identity). Neither path reopens by pathname.
  [[nodiscard]] ava::core::Result<SessionContentFingerprint> content_fingerprint() const;
  [[nodiscard]] SessionReadLimits read_limits() const noexcept;

  [[nodiscard]] ava::core::Result<std::vector<SessionEntry>> load() const;
  // Effective limits are per-field min(authority embedded policy, request).
  // Strict classification only; never repairs torn tails or tolerates invalid
  // final suffixes.
  [[nodiscard]] ava::core::Result<std::vector<SessionEntry>> load_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested = nullptr) const;
  [[nodiscard]] ava::core::Result<SessionSummary> inspect_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested = nullptr) const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  struct State;
  explicit SessionReadAuthority(std::shared_ptr<State const> state);
  [[nodiscard]] SessionReadLimits clamp_limits(SessionReadLimits request) const noexcept;

  std::shared_ptr<State const> state_;
};

enum class SessionAppendDisposition
{
  Appended,
  Existing,
};

struct SessionConditionalAppendResult
{
  SessionEntry entry;
  SessionAppendDisposition disposition = SessionAppendDisposition::Appended;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class SessionCompactionAppendResult
{
  Appended,
  SnapshotMismatch,
};

using SessionCompactionAppendSink =
    std::function<ava::core::Result<SessionCompactionAppendResult>(SessionEntry, std::vector<SessionEntry>, SessionCancelCallback)>;

// The sole copyable append authority for runtime routes. Persistent targets
// duplicate the caller's locked open-file description and revalidate the exact
// published inode before accepting it. Ephemeral targets retain only the
// shared in-memory store state. Targets retain the selected read policy so
// their recovery and derived read authorities preserve the runtime boundary.
class SessionAppendTarget
{
 public:
  [[nodiscard]] static ava::core::Result<std::shared_ptr<SessionAppendTarget>> create_persistent(SessionStore const& store, SessionLease const& lease,
                                                                                                 SessionReadLimits read_limits,
                                                                                                 SessionCancelCallback cancel_requested = nullptr);
  [[nodiscard]] static ava::core::Result<std::shared_ptr<SessionAppendTarget>> create_ephemeral(
      SessionStore const& store, SessionReadLimits read_limits = legacy_unbounded_session_read_limits());

  [[nodiscard]] ava::core::VoidResult append(SessionEntry const& entry);
  // Atomically revalidates one prepared BranchSummary against a fresh bounded
  // exact-lease snapshot and suppresses an existing matching final identity.
  // Cancellation linearizes under per-path serialization after the bounded
  // scan and immediately before append preflight. Cancellation after that point
  // may race with a committed append and is reported by the append result.
  [[nodiscard]] ava::core::Result<SessionConditionalAppendResult> append_branch_summary_if_absent(SessionEntry const& entry,
                                                                                                  SessionCancelCallback cancel_requested = nullptr);
  // Atomically reloads this target's authoritative bounded history, compares
  // the exact compaction snapshot, and appends only on a match. A mismatch is a
  // successful, nonmutating result and never requires recovery.
  [[nodiscard]] ava::core::Result<SessionCompactionAppendResult> append_compaction_if_snapshot_matches(SessionEntry const& entry,
                                                                                                       std::vector<SessionEntry> const& expected,
                                                                                                       SessionCancelCallback cancel_requested = nullptr);
  // One all-or-nothing preflight reservation for exactly one complete bounded
  // v4 assistant transaction: zero or more ordered output items followed by
  // its one matching commit. Durable writes remain append-only. Once any
  // batch record may be durable and a later write reports failure, this target
  // rejects all mutations until recover() rebuilds state from storage.
  [[nodiscard]] ava::core::VoidResult append_batch(std::vector<SessionEntry> entries);
  // Recovery is explicit: persistent targets repair through their private
  // duplicated lease, while ephemeral targets remove only a proven in-memory
  // incomplete v4 staging suffix. Both rebuild their append state afterward.
  [[nodiscard]] ava::core::VoidResult recover();
  [[nodiscard]] ava::core::Result<SessionReadAuthority> read_authority() const;
  [[nodiscard]] bool is_ephemeral() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  enum class ConditionalAppendCompletion
  {
    Succeeded,
    RejectedBeforeAppend,
    AppendFailed,
  };

  struct ConditionalAppendOutcome
  {
    ConditionalAppendCompletion completion = ConditionalAppendCompletion::RejectedBeforeAppend;
    std::optional<SessionConditionalAppendResult> result;
    std::optional<ava::core::Error> error;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  struct CompactionAppendOutcome
  {
    ConditionalAppendCompletion completion = ConditionalAppendCompletion::RejectedBeforeAppend;
    std::optional<SessionCompactionAppendResult> result;
    std::optional<ava::core::Error> error;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  // Controller queue policy needs this typed mutation-boundary outcome so only
  // errors from append_impl latch persistence. The public wrapper preserves the
  // ordinary Result API for direct append-target callers.
  [[nodiscard]] ConditionalAppendOutcome append_branch_summary_if_absent_classified(SessionEntry const& entry, SessionCancelCallback const& cancel_requested);
  [[nodiscard]] CompactionAppendOutcome append_compaction_if_snapshot_matches_classified(SessionEntry const& entry, std::vector<SessionEntry> const& expected,
                                                                                         SessionCancelCallback const& cancel_requested);

  SessionAppendTarget(SessionStore store, std::optional<SessionLease> lease, AssistantOutputAppendState assistant_output_state, SessionReadLimits read_limits);

  // Reload assistant_output_state_ from storage when another writer advanced the
  // file since this target last folded it into its cache. Must be called with
  // the per-path serialization lock held. No-op (returns success) when the
  // shared append epoch still equals last_seen_append_epoch_.
  [[nodiscard]] ava::core::VoidResult refresh_state_if_stale();
  // Advance the shared append epoch after a durable write and record the new
  // value as the generation this target's cache now reflects. Returns the new
  // epoch. Must be called with the serialization lock held and only after
  // assistant_output_state_ was updated to match the write.
  std::uint64_t advance_append_epoch();
  // Advance the shared append epoch after a durable but indeterminate write
  // (partial/unknown) without claiming the cache still matches storage. Used on
  // failure paths that latch recovery_required_; the next successful recover()
  // reestablishes the cache-to-epoch correspondence.
  void bump_append_epoch();
  // The shared append epoch for this target's destination: the per-path epoch
  // for persistent targets, or the ephemeral store's in-memory epoch otherwise.
  [[nodiscard]] std::atomic<std::uint64_t>& append_epoch_ref() const;

  friend class ava::app::SessionRunController;

  SessionStore store_;
  std::optional<SessionLease> lease_;
  SessionReadLimits read_limits_;
  mutable std::mutex mutex_;
  AssistantOutputAppendState assistant_output_state_;
  // The shared append epoch value that assistant_output_state_ reflects. Zero at
  // construction is corrected by create_persistent/create_ephemeral, which
  // snapshot the live epoch right after building the initial cached state.
  std::uint64_t last_seen_append_epoch_ = 0;
  bool recovery_required_ = false;
};

[[nodiscard]] std::string to_string(EntryType type);
[[nodiscard]] ava::core::Result<EntryType> parse_entry_type(std::string_view value);
[[nodiscard]] bool is_internal_replay_user_message(SessionEntry const& entry);
[[nodiscard]] std::string now_timestamp();
[[nodiscard]] std::string json_escape(std::string_view value);

}  // namespace ava::session
