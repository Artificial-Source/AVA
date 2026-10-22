#include "sys.h"
#include "ava/diagnostics/records.h"
#include "ava/core/strict_json.h"
#include "ava/core/version.h"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace ava::diagnostics {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaxDiagnosticJsonBytes = 64 * 1024;
constexpr std::size_t kMaxDoctorChecks = 32;

bool exact_keys(Json const& object, std::initializer_list<std::string_view> expected)
{
  if (!object.is_object() || object.size() != expected.size())
    return false;
  for (auto const key : expected)
  {
    if (!object.contains(key))
      return false;
  }
  return true;
}

std::optional<std::uint64_t> unsigned_value(Json const& value)
{
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>();
  if (value.is_number_integer())
  {
    auto const parsed = value.get<std::int64_t>();
    if (parsed >= 0)
      return static_cast<std::uint64_t>(parsed);
  }
  return std::nullopt;
}

std::optional<std::int64_t> timestamp_value(Json const& value)
{
  if (!value.is_number_integer() && !value.is_number_unsigned())
    return std::nullopt;
  if (value.is_number_unsigned())
  {
    auto const parsed = value.get<std::uint64_t>();
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return std::nullopt;
    return static_cast<std::int64_t>(parsed);
  }
  auto const parsed = value.get<std::int64_t>();
  if (parsed < 0)
    return std::nullopt;
  return parsed;
}

template <typename Enum, std::size_t N>
std::optional<Enum> parse_fixed(std::string_view value, std::array<Enum, N> const& values)
{
  auto const found = std::ranges::find_if(values, [&](Enum candidate) { return to_string(candidate) == value; });
  if (found == values.end())
    return std::nullopt;
  return *found;
}

std::optional<ComponentClass> parse_component(std::string_view value)
{
  constexpr std::array values{ComponentClass::Mcp,      ComponentClass::Plugin,  ComponentClass::App,  ComponentClass::Storage, ComponentClass::Configuration,
                              ComponentClass::Provider, ComponentClass::Session, ComponentClass::Tool, ComponentClass::Runtime};
  return parse_fixed(value, values);
}

std::optional<FailureCategory> parse_category(std::string_view value)
{
  constexpr std::array values{FailureCategory::Configuration, FailureCategory::Transport,    FailureCategory::Authorization,
                              FailureCategory::Protocol,      FailureCategory::Cancellation, FailureCategory::Internal};
  return parse_fixed(value, values);
}

std::optional<FailureCode> parse_failure_code(std::string_view value)
{
  constexpr std::array values{FailureCode::InvalidRequest,  FailureCode::IoFailure, FailureCode::NotFound,       FailureCode::PermissionDenied,
                              FailureCode::ExternalFailure, FailureCode::Canceled,  FailureCode::InternalFailure};
  return parse_fixed(value, values);
}

std::optional<Retryability> parse_retryability(std::string_view value)
{
  constexpr std::array values{Retryability::Never, Retryability::AfterUserAction, Retryability::Transient};
  return parse_fixed(value, values);
}

std::optional<RecoveryHint> parse_recovery_hint(std::string_view value)
{
  constexpr std::array values{RecoveryHint::VerifyRequest, RecoveryHint::VerifyConfiguration, RecoveryHint::VerifyPermissions, RecoveryHint::RetryOperation,
                              RecoveryHint::ContactSupport};
  return parse_fixed(value, values);
}

std::optional<DoctorCheckKind> parse_check_kind(std::string_view value)
{
  constexpr std::array values{DoctorCheckKind::VersionPlatform,     DoctorCheckKind::ConfigRoot,       DoctorCheckKind::StateRoot,
                              DoctorCheckKind::ModelRegistry,       DoctorCheckKind::DefaultModel,     DoctorCheckKind::AuthMetadata,
                              DoctorCheckKind::PluginConfiguration, DoctorCheckKind::McpConfiguration, DoctorCheckKind::LspConfiguration,
                              DoctorCheckKind::LspBuiltinClangd,    DoctorCheckKind::PermissionRules};
  return parse_fixed(value, values);
}

std::optional<DoctorStatus> parse_doctor_status(std::string_view value)
{
  constexpr std::array values{DoctorStatus::Pass, DoctorStatus::Warning, DoctorStatus::Fail};
  return parse_fixed(value, values);
}

std::optional<DoctorCode> parse_doctor_code(std::string_view value)
{
  constexpr std::array values{DoctorCode::Ready,
                              DoctorCode::BuiltinDefaults,
                              DoctorCode::Present,
                              DoctorCode::MissingOptional,
                              DoctorCode::UnsafeMetadata,
                              DoctorCode::InvalidConfiguration,
                              DoctorCode::DefaultUnavailable,
                              DoctorCode::Partial};
  return parse_fixed(value, values);
}

std::optional<Json> parse_strict(std::string_view json)
{
  if (json.empty() || json.size() > kMaxDiagnosticJsonBytes || ava::core::validate_strict_json(json, 16) != ava::core::StrictJsonStatus::Valid)
    return std::nullopt;
  auto parsed = Json::parse(json.begin(), json.end(), nullptr, false, false);
  if (parsed.is_discarded())
    return std::nullopt;
  return parsed;
}

std::string safe_failure_fields(SafeFailure const& failure)
{
  return "\"component\":\"" + std::string(to_string(failure.component)) + "\",\"category\":\"" + std::string(to_string(failure.category)) + "\",\"code\":\"" +
         std::string(to_string(failure.code)) + "\",\"retryability\":\"" + std::string(to_string(failure.retryability)) + "\",\"recovery_hint\":\"" +
         std::string(to_string(failure.recovery_hint)) + "\"";
}

std::optional<SafeFailure> parse_safe_failure(Json const& object)
{
  for (auto const key : {"component", "category", "code", "retryability", "recovery_hint"})
  {
    if (!object.contains(key) || !object[key].is_string())
      return std::nullopt;
  }
  auto component = parse_component(object["component"].get_ref<std::string const&>());
  auto category = parse_category(object["category"].get_ref<std::string const&>());
  auto code = parse_failure_code(object["code"].get_ref<std::string const&>());
  auto retryability = parse_retryability(object["retryability"].get_ref<std::string const&>());
  auto hint = parse_recovery_hint(object["recovery_hint"].get_ref<std::string const&>());
  if (!component || !category || !code || !retryability || !hint)
    return std::nullopt;
  return SafeFailure{.component = *component, .category = *category, .code = *code, .retryability = *retryability, .recovery_hint = *hint};
}

std::string record_state_json(StoredRecordState state)
{
  return "{\"schema_version\":1,\"state\":\"" + std::string(to_string(state)) + "\"}";
}

}  // namespace

bool DoctorReport::has_failures() const noexcept
{
  return std::ranges::any_of(checks, [](DoctorCheck const& check) { return check.status == DoctorStatus::Fail; });
}

std::string_view to_string(DoctorCheckKind kind) noexcept
{
  switch (kind)
  {
    case DoctorCheckKind::VersionPlatform:
      return "version_platform";
    case DoctorCheckKind::ConfigRoot:
      return "config_root";
    case DoctorCheckKind::StateRoot:
      return "state_root";
    case DoctorCheckKind::ModelRegistry:
      return "model_registry";
    case DoctorCheckKind::DefaultModel:
      return "default_model";
    case DoctorCheckKind::AuthMetadata:
      return "auth_metadata";
    case DoctorCheckKind::PluginConfiguration:
      return "plugin_configuration";
    case DoctorCheckKind::McpConfiguration:
      return "mcp_configuration";
    case DoctorCheckKind::LspConfiguration:
      return "lsp_configuration";
    case DoctorCheckKind::LspBuiltinClangd:
      return "lsp_builtin_clangd";
    case DoctorCheckKind::PermissionRules:
      return "permission_rules";
  }
  return "version_platform";
}

std::string_view to_string(DoctorStatus status) noexcept
{
  switch (status)
  {
    case DoctorStatus::Pass:
      return "pass";
    case DoctorStatus::Warning:
      return "warning";
    case DoctorStatus::Fail:
      return "fail";
  }
  return "fail";
}

std::string_view to_string(DoctorCode code) noexcept
{
  switch (code)
  {
    case DoctorCode::Ready:
      return "ready";
    case DoctorCode::BuiltinDefaults:
      return "builtin_defaults";
    case DoctorCode::Present:
      return "present";
    case DoctorCode::MissingOptional:
      return "missing_optional";
    case DoctorCode::UnsafeMetadata:
      return "unsafe_metadata";
    case DoctorCode::InvalidConfiguration:
      return "invalid_configuration";
    case DoctorCode::DefaultUnavailable:
      return "default_unavailable";
    case DoctorCode::Partial:
      return "partial";
  }
  return "invalid_configuration";
}

std::string_view to_string(StoredRecordState state) noexcept
{
  switch (state)
  {
    case StoredRecordState::Present:
      return "present";
    case StoredRecordState::Absent:
      return "absent";
    case StoredRecordState::Malformed:
      return "malformed";
    case StoredRecordState::Unsafe:
      return "unsafe";
    case StoredRecordState::Unavailable:
      return "unavailable";
  }
  return "unavailable";
}

std::string_view compile_time_os() noexcept
{
#if defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "macos";
#elif defined(_WIN32)
  return "windows";
#elif defined(__FreeBSD__)
  return "freebsd";
#else
  return "unknown";
#endif
}

std::string_view compile_time_arch() noexcept
{
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#else
  return "unknown";
#endif
}

std::string serialize_doctor_report_json(DoctorReport const& report)
{
  std::uint64_t passed = 0;
  std::uint64_t warnings = 0;
  std::uint64_t failed = 0;
  std::string result = "{\"schema_version\":1,\"checks\":[";
  for (std::size_t index = 0; index < report.checks.size(); ++index)
  {
    auto const& check = report.checks[index];
    if (index != 0)
      result += ',';
    result += "{\"label\":\"" + std::string(to_string(check.kind)) + "\",\"status\":\"" + std::string(to_string(check.status)) + "\",\"code\":\"" +
              std::string(to_string(check.code)) + "\",\"items\":" + std::to_string(check.items) + ",\"enabled\":" + std::to_string(check.enabled) +
              ",\"errors\":" + std::to_string(check.errors) + '}';
    passed += check.status == DoctorStatus::Pass ? 1U : 0U;
    warnings += check.status == DoctorStatus::Warning ? 1U : 0U;
    failed += check.status == DoctorStatus::Fail ? 1U : 0U;
  }
  result += "],\"summary\":{\"pass\":" + std::to_string(passed) + ",\"warning\":" + std::to_string(warnings) + ",\"fail\":" + std::to_string(failed) + "}}";
  return result;
}

std::string serialize_doctor_report_human(DoctorReport const& report)
{
  std::uint64_t passed = 0;
  std::uint64_t warnings = 0;
  std::uint64_t failed = 0;
  std::ostringstream out;
  out << "AVA doctor\n";
  for (auto const& check : report.checks)
  {
    out << to_string(check.status) << ' ' << to_string(check.kind) << " [" << to_string(check.code) << "] items=" << check.items << " enabled=" << check.enabled
        << " errors=" << check.errors << '\n';
    passed += check.status == DoctorStatus::Pass ? 1U : 0U;
    warnings += check.status == DoctorStatus::Warning ? 1U : 0U;
    failed += check.status == DoctorStatus::Fail ? 1U : 0U;
  }
  out << "summary pass=" << passed << " warning=" << warnings << " fail=" << failed << '\n';
  return out.str();
}

std::optional<DoctorReport> parse_doctor_report_json(std::string_view json) noexcept
{
  try
  {
    auto parsed = parse_strict(json);
    if (!parsed || !exact_keys(*parsed, {"schema_version", "checks", "summary"}) || !(*parsed)["schema_version"].is_number_integer() ||
        (*parsed)["schema_version"].get<int>() != kDiagnosticSchemaVersion || !(*parsed)["checks"].is_array() ||
        (*parsed)["checks"].size() > kMaxDoctorChecks || !exact_keys((*parsed)["summary"], {"pass", "warning", "fail"}))
      return std::nullopt;

    DoctorReport report;
    std::uint64_t passed = 0;
    std::uint64_t warnings = 0;
    std::uint64_t failed = 0;
    std::unordered_set<int> kinds;
    for (auto const& value : (*parsed)["checks"])
    {
      if (!exact_keys(value, {"label", "status", "code", "items", "enabled", "errors"}) || !value["label"].is_string() || !value["status"].is_string() ||
          !value["code"].is_string())
        return std::nullopt;
      auto kind = parse_check_kind(value["label"].get_ref<std::string const&>());
      auto status = parse_doctor_status(value["status"].get_ref<std::string const&>());
      auto code = parse_doctor_code(value["code"].get_ref<std::string const&>());
      auto items = unsigned_value(value["items"]);
      auto enabled = unsigned_value(value["enabled"]);
      auto errors = unsigned_value(value["errors"]);
      if (!kind || !status || !code || !items || !enabled || !errors || !kinds.insert(static_cast<int>(*kind)).second)
        return std::nullopt;
      report.checks.push_back(DoctorCheck{.kind = *kind, .status = *status, .code = *code, .items = *items, .enabled = *enabled, .errors = *errors});
      passed += *status == DoctorStatus::Pass ? 1U : 0U;
      warnings += *status == DoctorStatus::Warning ? 1U : 0U;
      failed += *status == DoctorStatus::Fail ? 1U : 0U;
    }
    auto summary_pass = unsigned_value((*parsed)["summary"]["pass"]);
    auto summary_warning = unsigned_value((*parsed)["summary"]["warning"]);
    auto summary_fail = unsigned_value((*parsed)["summary"]["fail"]);
    if (!summary_pass || !summary_warning || !summary_fail || *summary_pass != passed || *summary_warning != warnings || *summary_fail != failed)
      return std::nullopt;
    return report;
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::string serialize_last_failure_record(LastFailureRecord const& record)
{
  return "{\"schema_version\":1,\"recorded_at\":" + std::to_string(record.recorded_at) + ',' + safe_failure_fields(record.failure) +
         ",\"occurrences\":" + std::to_string(record.occurrences) + '}';
}

std::optional<LastFailureRecord> parse_last_failure_record(std::string_view json) noexcept
{
  try
  {
    auto parsed = parse_strict(json);
    if (!parsed || !exact_keys(*parsed, {"schema_version", "recorded_at", "component", "category", "code", "retryability", "recovery_hint", "occurrences"}) ||
        !(*parsed)["schema_version"].is_number_integer() || (*parsed)["schema_version"].get<int>() != kDiagnosticSchemaVersion)
      return std::nullopt;
    auto recorded_at = timestamp_value((*parsed)["recorded_at"]);
    auto occurrences = unsigned_value((*parsed)["occurrences"]);
    auto failure = parse_safe_failure(*parsed);
    if (!recorded_at || !occurrences || *occurrences == 0 || !failure)
      return std::nullopt;
    return LastFailureRecord{.recorded_at = *recorded_at, .failure = *failure, .occurrences = *occurrences};
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::string serialize_trace_counter_snapshot(TraceCounterSnapshot const& snapshot)
{
  auto const& health = snapshot.writer_health;
  return "{\"schema_version\":1,\"captured_at\":" + std::to_string(snapshot.captured_at) + ",\"runtime_starts\":" + std::to_string(snapshot.runtime_starts) +
         ",\"provider_requests\":" + std::to_string(snapshot.provider_requests) + ",\"provider_failures\":" + std::to_string(snapshot.provider_failures) +
         ",\"session_failures\":" + std::to_string(snapshot.session_failures) + ",\"plugin_failures\":" + std::to_string(snapshot.plugin_failures) +
         ",\"mcp_failures\":" + std::to_string(snapshot.mcp_failures) + ",\"writer_health\":{\"complete\":" + (health.complete ? "true" : "false") +
         ",\"events_written\":" + std::to_string(health.events_written) + ",\"events_dropped\":" + std::to_string(health.events_dropped) +
         ",\"writer_failures\":" + std::to_string(health.writer_failures) + ",\"bytes_written\":" + std::to_string(health.bytes_written) +
         ",\"delta_summaries_refused\":" + std::to_string(health.delta_summaries_refused) + "}}";
}

std::optional<TraceCounterSnapshot> parse_trace_counter_snapshot(std::string_view json) noexcept
{
  try
  {
    auto parsed = parse_strict(json);
    if (!parsed || !(*parsed)["schema_version"].is_number_integer() || (*parsed)["schema_version"].get<int>() != kDiagnosticSchemaVersion)
      return std::nullopt;
    bool const legacy = exact_keys(*parsed, {"schema_version", "captured_at", "runtime_starts", "provider_requests", "provider_failures", "session_failures",
                                             "plugin_failures", "mcp_failures"});
    bool const current = exact_keys(*parsed, {"schema_version", "captured_at", "runtime_starts", "provider_requests", "provider_failures", "session_failures",
                                              "plugin_failures", "mcp_failures", "writer_health"});
    if (!legacy && !current)
      return std::nullopt;
    auto captured_at = timestamp_value((*parsed)["captured_at"]);
    auto runtime_starts = unsigned_value((*parsed)["runtime_starts"]);
    auto provider_requests = unsigned_value((*parsed)["provider_requests"]);
    auto provider_failures = unsigned_value((*parsed)["provider_failures"]);
    auto session_failures = unsigned_value((*parsed)["session_failures"]);
    auto plugin_failures = unsigned_value((*parsed)["plugin_failures"]);
    auto mcp_failures = unsigned_value((*parsed)["mcp_failures"]);
    if (!captured_at || !runtime_starts || !provider_requests || !provider_failures || !session_failures || !plugin_failures || !mcp_failures)
      return std::nullopt;
    TraceWriterHealth health;
    if (current)
    {
      auto const& value = (*parsed)["writer_health"];
      bool const legacy_health = exact_keys(value, {"complete", "events_written", "events_dropped", "writer_failures", "bytes_written"});
      bool const current_health =
          exact_keys(value, {"complete", "events_written", "events_dropped", "writer_failures", "bytes_written", "delta_summaries_refused"});
      if ((!legacy_health && !current_health) || !value["complete"].is_boolean())
        return std::nullopt;
      auto events_written = unsigned_value(value["events_written"]);
      auto events_dropped = unsigned_value(value["events_dropped"]);
      auto writer_failures = unsigned_value(value["writer_failures"]);
      auto bytes_written = unsigned_value(value["bytes_written"]);
      auto delta_summaries_refused = legacy_health ? std::optional<std::uint64_t>{0} : unsigned_value(value["delta_summaries_refused"]);
      if (!events_written || !events_dropped || !writer_failures || !bytes_written || !delta_summaries_refused)
        return std::nullopt;
      health = {.complete = value["complete"].get<bool>(),
                .events_written = *events_written,
                .events_dropped = *events_dropped,
                .writer_failures = *writer_failures,
                .bytes_written = *bytes_written,
                .delta_summaries_refused = *delta_summaries_refused};
    }
    return TraceCounterSnapshot{.captured_at = *captured_at,
                                .runtime_starts = *runtime_starts,
                                .provider_requests = *provider_requests,
                                .provider_failures = *provider_failures,
                                .session_failures = *session_failures,
                                .plugin_failures = *plugin_failures,
                                .mcp_failures = *mcp_failures,
                                .writer_health = health};
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::string serialize_support_artifact(SupportArtifact const& artifact)
{
  auto const trace_state = artifact.trace.state == StoredRecordState::Present && !artifact.trace.record ? StoredRecordState::Unavailable : artifact.trace.state;
  auto trace = record_state_json(trace_state);
  if (trace_state == StoredRecordState::Present && artifact.trace.record)
  {
    trace = "{\"schema_version\":1,\"state\":\"present\",\"counters\":" + serialize_trace_counter_snapshot(*artifact.trace.record) + '}';
  }
  auto const last_failure_state =
      artifact.last_failure.state == StoredRecordState::Present && !artifact.last_failure.record ? StoredRecordState::Unavailable : artifact.last_failure.state;
  auto last_failure = record_state_json(last_failure_state);
  if (last_failure_state == StoredRecordState::Present && artifact.last_failure.record)
  {
    last_failure = "{\"schema_version\":1,\"state\":\"present\",\"record\":" + serialize_last_failure_record(*artifact.last_failure.record) + '}';
  }
  return "{\"schema_version\":1,\"generated_at\":" + std::to_string(artifact.generated_at) + ",\"ava\":{\"version\":\"" +
         std::string(ava::core::version::kFullVersion) + "\",\"os\":\"" + std::string(compile_time_os()) + "\",\"arch\":\"" + std::string(compile_time_arch()) +
         "\"},\"doctor\":" + serialize_doctor_report_json(artifact.doctor) + ",\"trace\":" + trace + ",\"last_failure\":" + last_failure + '}';
}

}  // namespace ava::diagnostics
