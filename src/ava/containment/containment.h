#pragma once

#include "ava/command/command.h"
#include "ava/debug/print_members_on.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include <sys/stat.h>

namespace ava::containment {

// Result of probing the kernel for development containment features.
enum class ContainmentAvailability
{
  Available,
  Unavailable,
};

// One descriptor-resolved logical path and its permitted Landlock access mask.
// The parent reopens and validates approved authority immediately before fork.
struct ContainmentFilesystemRule
{
  std::filesystem::path logical_path;
  std::uint64_t access_mask = 0;
  // Preparation reopens the logical spelling, verifies this identity and pins
  // that exact descriptor in the ruleset; pathname replacements cannot redirect it.
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t mode = 0;
  std::uint64_t owner = 0;
  std::uint64_t group = 0;
  std::uint64_t special_device = 0;
  bool identity_bound = false;

  friend bool operator==(ContainmentFilesystemRule const&, ContainmentFilesystemRule const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Redacted summary of the filesystem scope for metadata/audit. Deliberately
// excludes actual paths so secret locations cannot leak through diagnostics.
struct ContainmentFilesystemScope
{
  bool workspace_writable = false;
  bool synthetic_environment_writable = false;
  std::size_t read_only_root_count = 0;
  std::size_t device_file_count = 0;

  friend bool operator==(ContainmentFilesystemScope const&, ContainmentFilesystemScope const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// A development containment plan prepared from a sealed command preparation.
// Planning and application preparation run in the parent: probe features,
// validate roots, pin rule descriptors and build the Landlock ruleset. The
// child only rechecks retained metadata and installs Landlock/seccomp before exec.
//
// The plan never claims containment it cannot verify. If kernel features are
// unavailable or roots cannot be validated, availability is Unavailable and
// callers must downgrade to Ask.
struct DevelopmentContainmentPlan
{
  ContainmentAvailability availability = ContainmentAvailability::Unavailable;
  std::string profile_id;
  std::string unavailable_reason;
  std::uint32_t landlock_abi_version = 0;
  bool network_allowed = false;
  // True when the plan calls for a mode-aware seccomp filter (planned, not
  // yet installed). Both network modes restrict socket domains and io_uring;
  // the parent verifies child installation before reporting containment_applied.
  bool network_filter_planned = false;
  ContainmentFilesystemScope filesystem_scope;

  // Parent-side application preparation data, never exposed in metadata/audit.
  std::uint64_t handled_access_fs = 0;
  std::vector<ContainmentFilesystemRule> filesystem_rules;
  std::shared_ptr<ava::core::AnchorSet const> anchor_set;

  friend bool operator==(DevelopmentContainmentPlan const&, DevelopmentContainmentPlan const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Probe the highest supported Landlock ABI version without mutating parent
// state. Returns 0 if Landlock is unavailable.
[[nodiscard]] std::uint32_t probe_landlock_abi_version() noexcept;

// Probe whether the seccomp network filter is supported on this architecture.
// This performs a non-mutating kernel query (SECCOMP_GET_ACTION_AVAIL) to
// verify that the kernel supports SECCOMP_RET_ERRNO. Returns false when
// seccomp filter mode or the required action is unavailable.
[[nodiscard]] bool seccomp_network_filter_supported() noexcept;

// Prepare a development containment plan from the exact command preparation,
// workspace metadata, synthetic environment roots, sealed PATH/toolchain
// entries, executable/interpreter chain, and capability network flag.
// Probes required kernel features without mutating parent state.
// Canonical executable/interpreter targets outside the planned roots make the
// plan unavailable; containment never adds target roots implicitly.
[[nodiscard]] DevelopmentContainmentPlan prepare_development_containment(ava::command::CommandPreparation const& preparation, bool network_enabled);

// Parent-owned rule descriptors remain pinned until child installation. The
// child rechecks metadata without reopening paths or entering AnchorSet locks.
struct PreparedContainmentRule
{
  ava::core::UniqueFd descriptor;
  struct stat identity{};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PreparedContainment
{
  ava::core::UniqueFd ruleset;
  std::vector<PreparedContainmentRule> rules;
  bool network_allowed = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class ContainmentFailureStage : std::uint32_t
{
  None,
  DescriptorIdentity,
  NoNewPrivileges,
  Landlock,
  Seccomp,
};

// Fixed-size child/parent wire frame. No strings, allocation, or destructors.
struct ContainmentChildStatus
{
  ContainmentFailureStage stage = ContainmentFailureStage::None;
  std::int32_t error_number = 0;

  [[nodiscard]] explicit operator bool() const noexcept { return error_number == 0; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

static_assert(std::is_trivially_copyable_v<ContainmentChildStatus>);
static_assert(sizeof(ContainmentChildStatus) == 8);
static_assert(offsetof(ContainmentChildStatus, error_number) == 4);

[[nodiscard]] ava::core::Result<PreparedContainment> prepare_containment_application(DevelopmentContainmentPlan const& plan);
// Only syscalls and fixed-storage operations are permitted after fork. Consumes
// the child's prepared fds; the parent's independent copies remain RAII-owned.
[[nodiscard]] ContainmentChildStatus apply_containment_in_child(PreparedContainment& prepared) noexcept;
// Parent-only diagnostic construction from the fixed child frame.
[[nodiscard]] ava::core::Error containment_child_error(ContainmentChildStatus status);

[[nodiscard]] bool containment_is_available(DevelopmentContainmentPlan const& plan) noexcept;

// A diagnostics-safe summary that includes profile, availability, filesystem
// scope, and network status without leaking secret paths.
[[nodiscard]] std::string containment_summary(DevelopmentContainmentPlan const& plan);

// The minimum Landlock ABI version that mediates all writable operations AVA
// relies on, including truncate and refer.
constexpr std::uint32_t kRequiredLandlockAbiVersion = 3;

}  // namespace ava::containment
