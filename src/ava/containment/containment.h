#pragma once

#include "ava/command/command.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ava::containment {

// Result of probing the kernel for development containment features.
enum class ContainmentAvailability
{
  Available,
  Unavailable,
};

// One descriptor-resolved logical path and the Landlock access mask permitted
// beneath it. Writable paths are opened through the shared AnchorSet in the
// child; no canonical spelling is used as authority.
struct ContainmentFilesystemRule
{
  std::filesystem::path logical_path;
  std::uint64_t access_mask = 0;
  // Descriptor identity captured in the parent. The child opens the logical
  // spelling once, verifies this identity, and adds that same descriptor to
  // Landlock so a pathname replacement cannot redirect a rule.
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
// The plan is prepared in the parent (probing kernel features, validating
// roots, building rule lists) and applied in the child (installing Landlock
// rules and a seccomp network filter before exec).
//
// The plan never claims containment it cannot verify. If kernel features are
// unavailable or roots cannot be validated, availability is Unavailable and
// callers must downgrade to Ask.
struct DevelopmentContainmentPlan
{
  ContainmentAvailability availability = ContainmentAvailability::Unavailable;
  std::string profile_id;
  ava::core::NoLogString unavailable_reason;
  std::uint32_t landlock_abi_version = 0;
  bool network_allowed = false;
  // True when the plan calls for a mode-aware seccomp filter (planned, not
  // yet installed). Both network modes restrict socket domains and io_uring;
  // the parent verifies child installation before reporting containment_applied.
  bool network_filter_planned = false;
  ContainmentFilesystemScope filesystem_scope;

  // Child-side application data. These are not exposed in metadata/audit.
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

// Apply the containment plan in the child process after stdio/cwd setup and
// before exec. Installs Landlock and a seccomp filter in both network modes
// after no_new_privs. Only IP/NETLINK_ROUTE sockets are permitted when networking
// is allowed; private UNIX stream/seqpacket pairs work in either mode.
// Returns an error if installation fails; the caller must clean up the verified
// group and not exec.
//
// This function is called only in the child after fork.
[[nodiscard]] ava::core::VoidResult apply_containment_in_child(DevelopmentContainmentPlan const& plan);

// Close inherited non-stdio file descriptors in the child before applying
// seccomp. Skips the containment handshake descriptors and, when present, the
// approved executable descriptor that must survive until descriptor exec.
void close_inherited_fds_except(int keep_fd_a, int keep_fd_b, int keep_fd_c = -1) noexcept;

[[nodiscard]] bool containment_is_available(DevelopmentContainmentPlan const& plan) noexcept;

// A diagnostics-safe summary that includes profile, availability, filesystem
// scope, and network status without leaking secret paths.
[[nodiscard]] std::string containment_summary(DevelopmentContainmentPlan const& plan);

// The minimum Landlock ABI version that mediates all writable operations AVA
// relies on, including truncate and refer.
constexpr std::uint32_t kRequiredLandlockAbiVersion = 3;

}  // namespace ava::containment
