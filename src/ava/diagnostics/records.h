#pragma once

#include "ava/diagnostics/safe_failure.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "debug.h"

namespace ava::diagnostics {

inline constexpr int kDiagnosticSchemaVersion = 1;

enum class DoctorCheckKind
{
  VersionPlatform,
  ConfigRoot,
  StateRoot,
  ModelRegistry,
  DefaultModel,
  AuthMetadata,
  PluginConfiguration,
  McpConfiguration,
  LspConfiguration,
  LspBuiltinClangd,
  PermissionRules,
};

enum class DoctorStatus
{
  Pass,
  Warning,
  Fail,
};

enum class DoctorCode
{
  Ready,
  BuiltinDefaults,
  Present,
  MissingOptional,
  UnsafeMetadata,
  InvalidConfiguration,
  DefaultUnavailable,
  Partial,
};

struct DoctorCheck
{
  DoctorCheckKind kind = DoctorCheckKind::VersionPlatform;
  DoctorStatus status = DoctorStatus::Pass;
  DoctorCode code = DoctorCode::Ready;
  std::uint64_t items = 0;
  std::uint64_t enabled = 0;
  std::uint64_t errors = 0;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct DoctorReport
{
  int schema_version = kDiagnosticSchemaVersion;
  std::vector<DoctorCheck> checks;

  [[nodiscard]] bool has_failures() const noexcept;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct LastFailureRecord
{
  int schema_version = kDiagnosticSchemaVersion;
  std::int64_t recorded_at = 0;
  SafeFailure failure;
  std::uint64_t occurrences = 1;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TraceWriterHealth
{
  bool complete = false;
  std::uint64_t events_written = 0;
  std::uint64_t events_dropped = 0;
  std::uint64_t writer_failures = 0;
  std::uint64_t bytes_written = 0;
  // Intentional summary-budget refusals, separate from trace event loss.
  std::uint64_t delta_summaries_refused = 0;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TraceCounterSnapshot
{
  int schema_version = kDiagnosticSchemaVersion;
  std::int64_t captured_at = 0;
  std::uint64_t runtime_starts = 0;
  std::uint64_t provider_requests = 0;
  std::uint64_t provider_failures = 0;
  std::uint64_t session_failures = 0;
  std::uint64_t plugin_failures = 0;
  std::uint64_t mcp_failures = 0;
  TraceWriterHealth writer_health;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class StoredRecordState
{
  Present,
  Absent,
  Malformed,
  Unsafe,
  Unavailable,
};

// Forward declaration so StoredRecord::print_on can render the state enum as
// text without depending on the debug ostream operator machinery (which cannot
// be pulled into this widely-included header due to include-order constraints).
[[nodiscard]] std::string_view to_string(StoredRecordState state) noexcept;

template <typename Record>
struct StoredRecord
{
  StoredRecordState state = StoredRecordState::Absent;
  std::optional<Record> record = std::nullopt;

  // StoredRecord is a template, so the print_members generator cannot emit a
  // definition for it; it therefore uses AVA_DEBUG_PRINT_MEMBERS_OPT_OUT below.
  // This hand-written print_on lets the debug catch-all operator<< stream a
  // StoredRecord<Record> when it appears as a member of a type whose
  // print_members is generated (for example SupportArtifact::trace and
  // SupportArtifact::last_failure).
  //
  // Only standard-library stream operators and the dependent `os << *record`
  // (resolved at instantiation via ADL, where the Record type's print_on is
  // visible) are used, so this compiles in every translation unit that includes
  // records.h without requiring ava/debug/debug_ostream_operators.h.
  void print_on(std::ostream& os) const
  {
    os << "{state:" << to_string(state) << ",record:";
    if (record.has_value())
      os << *record;
    else
      os << "nullopt";
    os << '}';
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct SupportArtifact
{
  int schema_version = kDiagnosticSchemaVersion;
  std::int64_t generated_at = 0;
  DoctorReport doctor;
  StoredRecord<TraceCounterSnapshot> trace;
  StoredRecord<LastFailureRecord> last_failure;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string_view to_string(DoctorCheckKind kind) noexcept;
[[nodiscard]] std::string_view to_string(DoctorStatus status) noexcept;
[[nodiscard]] std::string_view to_string(DoctorCode code) noexcept;
[[nodiscard]] std::string_view to_string(StoredRecordState state) noexcept;
[[nodiscard]] std::string_view compile_time_os() noexcept;
[[nodiscard]] std::string_view compile_time_arch() noexcept;

[[nodiscard]] std::string serialize_doctor_report_json(DoctorReport const& report);
[[nodiscard]] std::string serialize_doctor_report_human(DoctorReport const& report);
[[nodiscard]] std::optional<DoctorReport> parse_doctor_report_json(std::string_view json) noexcept;

[[nodiscard]] std::string serialize_last_failure_record(LastFailureRecord const& record);
[[nodiscard]] std::optional<LastFailureRecord> parse_last_failure_record(std::string_view json) noexcept;
[[nodiscard]] std::string serialize_trace_counter_snapshot(TraceCounterSnapshot const& snapshot);
[[nodiscard]] std::optional<TraceCounterSnapshot> parse_trace_counter_snapshot(std::string_view json) noexcept;

// The support serializer accepts typed, closed diagnostic records only. It has
// no arbitrary-string extension point.
[[nodiscard]] std::string serialize_support_artifact(SupportArtifact const& artifact);

}  // namespace ava::diagnostics
