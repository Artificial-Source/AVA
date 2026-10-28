#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/http/transport.h"
#include "ava/app/runtime/session_ts.h"
#include "ava/config/model_config.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/session_branch.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ava::core {
class AnchorSet;
}  // namespace ava::core

namespace ava::provider {
class ProviderCatalog;
}  // namespace ava::provider

namespace ava::app {
class SessionRunController;

inline constexpr ava::session::SessionReadLimits kBranchSummaryHardReadLimits{
    .max_file_bytes = 8U * 1024U * 1024U, .max_line_bytes = 1024U * 1024U, .max_entries = 16384};
inline constexpr std::size_t kMaxBranchSummaryCandidateRecords = 4096;
inline constexpr std::size_t kMaxBranchSummaryProjectedTextBytes = 8U * 1024U;
inline constexpr std::size_t kMaxBranchSummaryProjectionBytes = 128U * 1024U;
inline constexpr std::size_t kMaxGeneratedBranchSummaryBytes = 8U * 1024U;
inline constexpr std::size_t kMaxBranchSummaryRawResponseBytes = 64U * 1024U;
inline constexpr std::size_t kMaxBranchSummaryDisplayLabelBytes = 256;

// One isolated metadata-generation prompt. The system instruction is immutable
// host policy and user_payload is only the pure session projection; no runtime
// prompt, tool, path, ID, credential, or provider context is admitted here.
struct BranchSummaryGenerationPrompt
{
  std::string system_instruction;
  std::string user_payload;

  // Prompt payloads can contain user data and must never enter debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// These seams are reachable only from trusted application wiring and tests,
// never extensions or plugins. Callbacks must be cooperative, nonblocking, and
// must not invoke coordinator or current-session controller shutdown
// reentrantly.
using BranchSummaryGenerator =
    std::function<ava::core::Result<std::string>(BranchSummaryGenerationPrompt const&, std::stop_token, std::chrono::steady_clock::time_point)>;
using BranchSummaryTransportFactory = ava::http::TransportFactory;

// Narrow provider inputs copied on the frontend thread. Authentication remains
// lazy: the production generator resolves credentials only after confirmation.
// Explicit credential values are a compatibility/test seam and are never
// copied into BranchSummarySnapshot.
struct BranchSummaryProviderOptions
{
  std::string access_token = {};
  std::string credential_type = "bearer";
  std::string account_id = {};
  bool openai_oauth = false;
  bool offline = false;
  BranchSummaryTransportFactory transport_factory = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Value-owned request assembled while runtime::Session is stable on the
// frontend thread. It deliberately contains no Session reference, pointer, or
// view. The current controller owner is retained only for run checks and the
// post-confirmation maintenance reservation; all source mutation uses a
// freshly acquired exact parent lease and remains independent of that owner.
struct BranchSummaryOperationRequest
{
  std::string current_session_id;
  std::string source_session_id;
  std::filesystem::path source_session_path;
  std::string source_label;
  std::filesystem::path workspace_dir;
  std::filesystem::path root_dir;
  ava::session::SessionReadLimits read_limits;
  ava::session::SessionReadAuthority current_read_authority;
  std::shared_ptr<SessionRunController> current_controller;
  ava::config::XdgPaths paths;
  ava::config::ModelInfo selected_model;
  std::shared_ptr<ava::provider::ProviderCatalog const> provider_catalog;
  std::shared_ptr<ava::core::AnchorSet> anchor_set;
  BranchSummaryProviderOptions provider_options = {};
  BranchSummaryGenerator generator = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class BranchSummaryPhase
{
  Idle,
  Preparing,
  AwaitingConfirmation,
  Generating,
  Revalidating,
  Appending,
  Succeeded,
  Existing,
  Ineligible,
  Canceled,
  Failed,
};

enum class BranchSummaryEligibilityCode
{
  CurrentSessionEphemeral,
  CurrentSessionUnavailable,
  ActiveRun,
  InvalidSourceSelection,
  NotDirectSource,
  InvalidFork,
  SourceUnavailable,
  SourceLeaseBusy,
  SourceCorrupt,
  ForkEntryNotFound,
  EmptySuffix,
};

enum class BranchSummaryFailureCode
{
  Deadline,
  RecoveryFailed,
  ProjectionRecordLimit,
  ProjectionTextLimit,
  ProjectionByteLimit,
  ProjectionInvalidText,
  ProjectionEmpty,
  ModelUnavailable,
  AuthenticationUnavailable,
  ProviderFailed,
  InvalidGeneratedSummary,
  StaleSource,
  AppendFailed,
  Internal,
};

// Bounded, path-free, credential-free, and raw-payload-free public state.
// refresh_required is true only after a clean append or an authoritative exact
// Existing observation.
struct BranchSummarySnapshot
{
  std::uint64_t generation = 0;
  BranchSummaryPhase phase = BranchSummaryPhase::Idle;
  std::string source_label = {};
  std::string model_label = {};
  std::optional<BranchSummaryEligibilityCode> eligibility_code = std::nullopt;
  std::optional<BranchSummaryFailureCode> failure_code = std::nullopt;
  std::string reason = {};
  std::optional<std::string> append_commit_state = std::nullopt;
  bool refresh_required = false;

  [[nodiscard]] bool terminal() const noexcept;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Trusted application/test observer under the same cooperative, nonblocking,
// non-reentrant-shutdown contract as BranchSummaryGenerator.
using BranchSummarySnapshotCallback = std::function<void(BranchSummarySnapshot const&)>;

struct BranchSummaryCoordinatorOptions
{
  std::chrono::milliseconds operation_deadline = std::chrono::seconds(30);
  BranchSummarySnapshotCallback on_snapshot = nullptr;
  // Deterministic test-only seam. Production callers leave this null. It is
  // invoked without coordinator locks on the freshly opened post-confirmation
  // source store, after exact consent revalidation but before recovery, so
  // existing SessionStore fault hooks can be
  // installed without weakening the production authority path.
  std::function<void(ava::session::SessionStore&)> configure_source_store_for_test = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Application-scoped owner for one explicit abandoned-parent summary
// operation at a time. A single jthread performs filesystem, recovery,
// provider, revalidation, and append work without ever entering curses.
class BranchSummaryCoordinator final
{
  struct Impl;
  class ConstructionKey
  {
    friend class BranchSummaryCoordinator;
    ConstructionKey() = default;

   public:
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

 public:
  BranchSummaryCoordinator(ConstructionKey, std::unique_ptr<Impl> impl);
  [[nodiscard]] static ava::core::Result<std::shared_ptr<BranchSummaryCoordinator>> create(BranchSummaryCoordinatorOptions options = {});
  ~BranchSummaryCoordinator();

  BranchSummaryCoordinator(BranchSummaryCoordinator const&) = delete;
  BranchSummaryCoordinator& operator=(BranchSummaryCoordinator const&) = delete;

  // Asynchronously performs the read-only preparation pass. A conflicting
  // operation is rejected instead of queued.
  [[nodiscard]] ava::core::Result<std::uint64_t> prepare(BranchSummaryOperationRequest request);
  // Confirmation is a separate command and starts the one absolute deadline.
  [[nodiscard]] ava::core::Result<bool> confirm(std::uint64_t generation);
  // Cancellation is cooperative and idempotent. Before confirmation it is
  // strictly nonmutating because no source authority is retained.
  [[nodiscard]] ava::core::Result<bool> cancel(std::uint64_t generation);

  [[nodiscard]] BranchSummarySnapshot snapshot() const;
  [[nodiscard]] bool wait_for_phase(std::uint64_t generation, BranchSummaryPhase phase, std::chrono::milliseconds timeout) const;
  [[nodiscard]] bool wait_until_idle(std::chrono::milliseconds timeout) const;
  void shutdown() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::unique_ptr<Impl> impl_;
};

// Pure projection and output boundaries used by both the coordinator and
// deterministic tests. Neither function truncates accepted content.
[[nodiscard]] ava::core::Result<std::string> project_branch_summary_prompt(std::vector<ava::session::SessionEntry> const& entries,
                                                                           ava::session::BranchSummaryCoverage const& coverage);
[[nodiscard]] ava::core::Result<std::string> sanitize_generated_branch_summary(std::string_view text);
[[nodiscard]] std::string_view branch_summary_system_instruction() noexcept;

// Production integration seam: snapshot the exact immutable operation inputs
// from a stable runtime session and one value-owned catalog selection. This
// performs no authentication or provider I/O and does not retain session state.
[[nodiscard]] ava::core::Result<BranchSummaryOperationRequest> make_branch_summary_operation_request(runtime::session_ts const& current,
                                                                                                     ava::session::SessionSummary selected_source,
                                                                                                     BranchSummaryGenerator generator = nullptr,
                                                                                                     BranchSummaryProviderOptions provider_options = {});

[[nodiscard]] std::string_view to_string(BranchSummaryPhase phase) noexcept;
[[nodiscard]] std::string_view to_string(BranchSummaryEligibilityCode code) noexcept;
[[nodiscard]] std::string_view to_string(BranchSummaryFailureCode code) noexcept;

}  // namespace ava::app
