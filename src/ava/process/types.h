#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::process {

inline constexpr std::uint32_t kProcessSchemaVersionV1 = 1;
inline constexpr std::size_t kMaxLiveProcessRecordsV1 = 256;
inline constexpr std::size_t kMaxRetainedProcessRecordsV1 = 256;
inline constexpr std::size_t kMaxOwnerSegmentBytesV1 = 64;
inline constexpr std::size_t kMaxPipeWatchesV1 = 8;
inline constexpr std::size_t kMaxRetainedScriptDescriptorsV1 = 8;

using ProcessDeadline = std::chrono::steady_clock::time_point;

enum class ProcessRoleV1
{
  Curl,
  Bash,
  Plugin,
  Mcp,
  Lsp,
  Mermaid,
  BrowserOpener,
  ClipboardHelper,
  ExternalEditor,
};

enum class ProcessStateV1
{
  Reserved,
  Launching,
  Running,
  StopRequested,
  Reaping,
  Finished,
};

enum class TerminationReasonV1
{
  NaturalExit,
  LaunchFailed,
  ExecFailed,
  Canceled,
  DeadlineExpired,
  OwnerShutdown,
  ApplicationShutdown,
  OutputLimit,
  ProtocolFailure,
  UnsupportedSuspension,
};

enum class CleanupStateV1
{
  NotRequired,
  Pending,
  Complete,
  Incomplete,
};

enum class ExitKindV1
{
  None,
  Exited,
  Signaled,
  LaunchError,
  CleanupIncomplete,
};

enum class StreamModeV1
{
  Capture,
  Inherit,
  Discard,
};

enum class StreamKindV1
{
  StandardInput,
  StandardOutput,
  StandardError,
};

enum class ChildMemberV1
{
  Leader,
  Sentinel,
};

enum class PipeIoStateV1
{
  Progress,
  WouldBlock,
  EndOfStream,
};

enum class PipeInterestV1
{
  Readable,
  Writable,
};

enum class AdoptionForkBranchV1
{
  Parent,
  Child,
};

enum class BashContainmentHandshakeV1
{
  None,
  Required,
};

enum class AdoptionChildFailureStageV1
{
  Streams,
  WorkingDirectory,
  Containment,
  DescriptorValidation,
};

enum class PlatformSupportV1
{
  Posix,
  Unsupported,
};

[[nodiscard]] constexpr bool is_valid(ProcessRoleV1 value) noexcept
{
  return value >= ProcessRoleV1::Curl && value <= ProcessRoleV1::ExternalEditor;
}

[[nodiscard]] constexpr bool is_valid(ProcessStateV1 value) noexcept
{
  return value >= ProcessStateV1::Reserved && value <= ProcessStateV1::Finished;
}

[[nodiscard]] constexpr bool is_valid(TerminationReasonV1 value) noexcept
{
  return value >= TerminationReasonV1::NaturalExit && value <= TerminationReasonV1::UnsupportedSuspension;
}

[[nodiscard]] constexpr bool is_valid(CleanupStateV1 value) noexcept
{
  return value >= CleanupStateV1::NotRequired && value <= CleanupStateV1::Incomplete;
}

[[nodiscard]] constexpr bool is_valid(ExitKindV1 value) noexcept
{
  return value >= ExitKindV1::None && value <= ExitKindV1::CleanupIncomplete;
}

[[nodiscard]] constexpr bool is_valid(StreamModeV1 value) noexcept
{
  return value >= StreamModeV1::Capture && value <= StreamModeV1::Discard;
}

[[nodiscard]] constexpr bool is_valid(StreamKindV1 value) noexcept
{
  return value >= StreamKindV1::StandardInput && value <= StreamKindV1::StandardError;
}

[[nodiscard]] constexpr bool is_valid(ChildMemberV1 value) noexcept
{
  return value >= ChildMemberV1::Leader && value <= ChildMemberV1::Sentinel;
}

[[nodiscard]] constexpr bool is_valid(PipeInterestV1 value) noexcept
{
  return value >= PipeInterestV1::Readable && value <= PipeInterestV1::Writable;
}

[[nodiscard]] constexpr bool is_valid(BashContainmentHandshakeV1 value) noexcept
{
  return value >= BashContainmentHandshakeV1::None && value <= BashContainmentHandshakeV1::Required;
}

[[nodiscard]] constexpr bool is_valid(AdoptionChildFailureStageV1 value) noexcept
{
  return value >= AdoptionChildFailureStageV1::Streams && value <= AdoptionChildFailureStageV1::DescriptorValidation;
}

[[nodiscard]] std::string_view to_string(ProcessRoleV1 value) noexcept;
[[nodiscard]] std::string_view to_string(ProcessStateV1 value) noexcept;
[[nodiscard]] std::string_view to_string(TerminationReasonV1 value) noexcept;
[[nodiscard]] std::string_view to_string(CleanupStateV1 value) noexcept;
[[nodiscard]] std::string_view to_string(ExitKindV1 value) noexcept;
[[nodiscard]] std::string_view to_string(StreamModeV1 value) noexcept;
[[nodiscard]] std::string_view to_string(StreamKindV1 value) noexcept;
[[nodiscard]] std::string_view to_string(ChildMemberV1 value) noexcept;
[[nodiscard]] std::string_view to_string(PipeInterestV1 value) noexcept;

struct LifecyclePolicyV1
{
  std::chrono::milliseconds termination_grace{100};
  std::chrono::milliseconds startup_timeout{2000};
  std::optional<ProcessDeadline> execution_deadline;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct EnvironmentVariableV1
{
  std::string name;
  ava::core::NoLogString value;

  // Environment content must never be emitted by generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PipeIoResultV1
{
  std::size_t bytes = 0;
  PipeIoStateV1 state = PipeIoStateV1::WouldBlock;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ExitStatusV1
{
  std::uint32_t schema_version = kProcessSchemaVersionV1;
  TerminationReasonV1 reason = TerminationReasonV1::LaunchFailed;
  ExitKindV1 kind = ExitKindV1::None;
  CleanupStateV1 cleanup = CleanupStateV1::Pending;
  int exit_code = 0;
  int signal_number = 0;
  bool has_exit_code = false;
  bool has_signal_number = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PipeReadyV1
{
  std::uint32_t token = 0;
  bool readable = false;
  bool writable = false;
  bool peer_closed = false;
  bool error = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ProcessActivityV1
{
  bool process_finished = false;
  bool deadline_expired = false;
  std::vector<PipeReadyV1> ready;  // Bounded by kMaxPipeWatchesV1.

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ProcessSnapshotRecordV1
{
  std::uint32_t schema_version = kProcessSchemaVersionV1;
  std::uint64_t record_alias = 0;
  std::uint64_t owner_alias = 0;
  ProcessRoleV1 role = ProcessRoleV1::Curl;
  ProcessStateV1 state = ProcessStateV1::Reserved;
  std::optional<TerminationReasonV1> reason;
  CleanupStateV1 cleanup = CleanupStateV1::NotRequired;
  ExitKindV1 exit_kind = ExitKindV1::None;
  std::uint64_t monotonic_milliseconds = 0;
  std::uint64_t stdout_bytes = 0;
  std::uint64_t stderr_bytes = 0;
  std::uint32_t settlement_count = 0;
  int exit_code = 0;
  int signal_number = 0;
  bool group_verified = false;
  bool stdout_truncated = false;
  bool stderr_truncated = false;
  bool has_exit_code = false;
  bool has_signal_number = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ProcessSnapshotV1
{
  std::uint32_t schema_version = kProcessSchemaVersionV1;
  bool accepting = true;
  bool monitor_started = false;
  std::size_t live_records = 0;
  std::size_t retained_terminal_records = 0;
  std::vector<ProcessSnapshotRecordV1> records;

  // The bounded records are individually printable; opting the container out
  // avoids an unbounded-looking generic container printer at call sites.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct StopResultV1
{
  std::size_t matched = 0;
  std::size_t newly_requested = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ShutdownResultV1
{
  bool complete = true;
  std::size_t incomplete_count = 0;
  std::size_t settled_count = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] PlatformSupportV1 platform_support_v1() noexcept;

}  // namespace ava::process
