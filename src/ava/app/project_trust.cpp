#include "sys.h"
#include "ava/app/project_trust.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/atomic_file.h"
#include "ava/core/error.h"
#include "ava/core/fd_io.h"
#include "ava/core/json.h"
#include "ava/core/path.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>
#include <utility>
#include <fcntl.h>

namespace ava::app {
namespace {

using ava::core::normalized_absolute_path;

constexpr std::size_t kMaxTrustFileBytes = 256 * 1024;

struct TrustRecord
{
  std::filesystem::path path;
  bool trusted = false;
};

struct ParsedTrustRecords
{
  std::vector<TrustRecord> records;
  std::string diagnostic;
};

bool path_exists(std::filesystem::path const& path)
{
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

void add_resource_if_present(std::vector<ProjectTrustResource>& resources, std::string kind, std::filesystem::path const& path)
{
  if (!path_exists(path))
    return;
  resources.push_back(ProjectTrustResource{.kind = std::move(kind), .path = ava::core::normalized_absolute_path(path)});
}

std::vector<ProjectTrustResource> discover_protected_resources(std::filesystem::path const& workspace_dir)
{
  std::vector<ProjectTrustResource> resources;
  if (workspace_dir.empty())
    return resources;
  auto const workspace = normalized_absolute_path(workspace_dir);
  add_resource_if_present(resources, "prompt_commands", workspace / ".ava" / "commands");
  add_resource_if_present(resources, "prompt_commands", workspace / ".ava" / "command");
  add_resource_if_present(resources, "skills", workspace / ".ava" / "skills");
  add_resource_if_present(resources, "skills", workspace / ".agents" / "skills");
  add_resource_if_present(resources, "skills", workspace / ".claude" / "skills");
  add_resource_if_present(resources, "agents", workspace / ".ava" / "agents");
  add_resource_if_present(resources, "agents", workspace / ".ava" / "agent");
  add_resource_if_present(resources, "agents", workspace / ".agents" / "agents");
  add_resource_if_present(resources, "agents", workspace / ".agents" / "agent");
  add_resource_if_present(resources, "agents", workspace / ".claude" / "agents");
  add_resource_if_present(resources, "agents", workspace / ".claude" / "agent");
  add_resource_if_present(resources, "plugins", workspace / ".ava" / "plugins");
  add_resource_if_present(resources, "mcp_config", workspace / ".ava" / "mcp.json");
  add_resource_if_present(resources, "lsp_config", workspace / ".ava" / "lsp.json");
  add_resource_if_present(resources, "system_prompt", workspace / ".ava" / "SYSTEM.md");
  add_resource_if_present(resources, "system_prompt", workspace / ".ava" / "APPEND_SYSTEM.md");
  return resources;
}

std::optional<bool> bool_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::nullopt;
  auto const valid_terminator = [](std::string_view value, std::size_t offset) {
    while (offset < value.size() && std::isspace(static_cast<unsigned char>(value[offset])) != 0)
      ++offset;
    return offset >= value.size() || value[offset] == ',' || value[offset] == '}';
  };
  if (object.substr(*start, 4) == "true" && valid_terminator(object, *start + 4))
    return true;
  if (object.substr(*start, 5) == "false" && valid_terminator(object, *start + 5))
    return false;
  return std::nullopt;
}

ava::core::Result<std::string> read_trust_file(std::filesystem::path const& path)
{
  if (path.empty() || !path_exists(path))
    return std::string{};
  // Preserve configured symlink resolution, but inspect and bound the exact
  // opened file rather than trusting a pathname size check before an unbounded read.
  ava::core::UniqueFd file(ava::core::restart_on_eintr([&path] { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK); }));
  if (!file)
  {
    auto const error_number = errno;
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to open project trust file")
                               .with_context("path", path.string())
                               .with_context("cause", std::strerror(error_number)));
  }
  auto metadata = ava::core::prepare_regular_descriptor(file.get());
  if (!metadata)
  {
    auto error = ava::core::open_regular_error(metadata.error(), "project trust file");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto content = ava::core::read_descriptor_bounded(file.get(), kMaxTrustFileBytes);
  if (!content && content.error() == EFBIG)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "project trust file is too large")
                               .with_context("path", path.string())
                               .with_context("max_bytes", std::to_string(kMaxTrustFileBytes)));
  }
  if (!content)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read project trust file")
                               .with_context("path", path.string())
                               .with_context("cause", std::strerror(content.error())));
  }
  return std::move(*content);
}

std::string malformed_trust_file_diagnostic(std::filesystem::path const& path, std::string reason)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "malformed project trust file")
      .with_context("path", path.string())
      .with_context("cause", std::move(reason))
      .format();
}

ParsedTrustRecords parse_trust_records(std::string_view content, bool content_expected)
{
  ParsedTrustRecords parsed;
  if (content.empty())
  {
    if (content_expected)
      parsed.diagnostic = "file is empty";
    return parsed;
  }
  if (!ava::core::json::is_valid_object(content))
  {
    parsed.diagnostic = "invalid JSON object";
    return parsed;
  }

  auto const decisions_start = ava::core::json::field_value_start(content, "decisions");
  if (!decisions_start)
  {
    parsed.diagnostic = "missing decisions array";
    return parsed;
  }
  if (*decisions_start >= content.size() || content[*decisions_start] != '[')
  {
    parsed.diagnostic = "decisions must be an array";
    return parsed;
  }

  std::size_t skipped_records = 0;
  for (auto const& object : ava::core::json::objects_in_array_field(content, "decisions"))
  {
    auto path = ava::core::json::string_field(object, "path");
    auto trusted = bool_field(object, "trusted");
    if (!path || !trusted)
    {
      ++skipped_records;
      continue;
    }
    parsed.records.push_back(TrustRecord{.path = normalized_absolute_path(*path), .trusted = *trusted});
  }
  if (skipped_records > 0)
  {
    parsed.diagnostic = "ignored " + std::to_string(skipped_records) + " malformed project trust decision" + (skipped_records == 1 ? "" : "s");
  }
  return parsed;
}

std::string trust_records_json(std::vector<TrustRecord> const& records)
{
  std::string json = "{\"schema_version\":1,\"decisions\":[";
  for (std::size_t index = 0; index < records.size(); ++index)
  {
    if (index > 0)
      json += ",";
    json += "{\"path\":\"";
    json += ava::core::json::escape(records[index].path.string());
    json += "\",\"trusted\":";
    json += (records[index].trusted ? std::string("true") : std::string("false"));
    json += "}";
  }
  json += "]}\n";
  return json;
}

bool path_is_ancestor_or_same(std::filesystem::path const& ancestor, std::filesystem::path const& path)
{
  if (ancestor.empty())
    return false;
  if (ancestor == path)
    return true;
  std::error_code relative_error;
  auto const relative = std::filesystem::relative(path, ancestor, relative_error);
  if (relative_error || relative.empty())
    return false;
  return *relative.begin() != "..";
}

std::optional<TrustRecord> closest_matching_record(std::vector<TrustRecord> const& records, std::filesystem::path const& workspace_dir)
{
  std::optional<TrustRecord> best;
  auto const workspace = normalized_absolute_path(workspace_dir);
  for (auto const& record : records)
  {
    if (!path_is_ancestor_or_same(record.path, workspace))
      continue;
    if (!best || record.path.string().size() > best->path.string().size())
      best = record;
  }
  return best;
}

ava::core::VoidResult write_trust_records(std::filesystem::path const& path, std::vector<TrustRecord> const& records)
{
  return ava::core::write_text_file_atomic(path, trust_records_json(records), "project trust file");
}

ava::core::Result<std::vector<TrustRecord>> load_records_for_write(std::filesystem::path const& path)
{
  auto content = read_trust_file(path);
  if (!content)
    return std::unexpected(std::move(content.error()));
  auto parsed = parse_trust_records(*content, path_exists(path));
  return std::move(parsed.records);
}

}  // namespace

struct StagedProjectTrustMutation::Impl
{
  std::filesystem::path trust_file;
  std::vector<TrustRecord> records;
  ProjectTrustState effective_state;
  bool write_required = false;
  bool committed = false;
};

StagedProjectTrustMutation::StagedProjectTrustMutation(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

StagedProjectTrustMutation::~StagedProjectTrustMutation() = default;
StagedProjectTrustMutation::StagedProjectTrustMutation(StagedProjectTrustMutation&& other) noexcept = default;
StagedProjectTrustMutation& StagedProjectTrustMutation::operator=(StagedProjectTrustMutation&& other) noexcept = default;

ProjectTrustState const& StagedProjectTrustMutation::effective_state() const
{
  return impl_->effective_state;
}

ava::core::VoidResult StagedProjectTrustMutation::commit()
{
  if (!impl_ || impl_->committed)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "project trust mutation is unavailable"));
  if (impl_->write_required)
  {
    if (auto written = write_trust_records(impl_->trust_file, impl_->records); !written)
      return written;
  }
  impl_->committed = true;
  return {};
}

namespace {

ProjectTrustState effective_staged_state(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir,
                                         std::vector<TrustRecord> const& records)
{
  ProjectTrustState state;
  state.workspace_dir = normalized_absolute_path(workspace_dir);
  state.trust_file = project_trust_file(paths);
  state.protected_resources = discover_protected_resources(state.workspace_dir);
  if (auto matched = closest_matching_record(records, state.workspace_dir))
  {
    state.decision = matched->trusted ? ProjectTrustDecision::Trusted : ProjectTrustDecision::Denied;
    state.matched_path = matched->path;
  }
  return state;
}

}  // namespace

std::string_view to_string(ProjectTrustDecision decision)
{
  switch (decision)
  {
    case ProjectTrustDecision::Unknown:
      return "unknown";
    case ProjectTrustDecision::Trusted:
      return "trusted";
    case ProjectTrustDecision::Denied:
      return "denied";
  }
  return "unknown";
}

bool project_resources_trusted(ProjectTrustState const& state)
{
  return state.decision == ProjectTrustDecision::Trusted;
}

std::filesystem::path project_trust_file(ava::config::XdgPaths const& paths)
{
  return paths.ava_state_dir / "project-trust.json";
}

ProjectTrustState load_project_trust_state(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir)
{
  ProjectTrustState state;
  state.workspace_dir = normalized_absolute_path(workspace_dir);
  state.trust_file = project_trust_file(paths);
  state.protected_resources = discover_protected_resources(state.workspace_dir);

  auto const trust_file_exists = path_exists(state.trust_file);
  auto content = read_trust_file(state.trust_file);
  if (!content)
  {
    state.diagnostic = content.error().format();
    return state;
  }
  auto parsed = parse_trust_records(*content, trust_file_exists);
  if (!parsed.diagnostic.empty())
    state.diagnostic = malformed_trust_file_diagnostic(state.trust_file, std::move(parsed.diagnostic));
  if (auto matched = closest_matching_record(parsed.records, state.workspace_dir))
  {
    state.decision = matched->trusted ? ProjectTrustDecision::Trusted : ProjectTrustDecision::Denied;
    state.matched_path = matched->path;
  }
  return state;
}

ava::core::Result<StagedProjectTrustMutation> stage_set_project_trust_decision(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir,
                                                                               bool trusted)
{
  auto const trust_path = project_trust_file(paths);
  auto records = load_records_for_write(trust_path);
  if (!records)
    return std::unexpected(std::move(records.error()));
  auto const workspace = normalized_absolute_path(workspace_dir);
  auto existing = std::ranges::find_if(*records, [&](TrustRecord const& record) { return record.path == workspace; });
  if (existing == records->end())
    records->push_back(TrustRecord{.path = workspace, .trusted = trusted});
  else
    existing->trusted = trusted;
  std::ranges::sort(*records, [](TrustRecord const& left, TrustRecord const& right) { return left.path.string() < right.path.string(); });

  auto impl = std::make_unique<StagedProjectTrustMutation::Impl>();
  impl->trust_file = trust_path;
  impl->effective_state = effective_staged_state(paths, workspace, *records);
  impl->records = std::move(*records);
  impl->write_required = true;
  return StagedProjectTrustMutation(std::move(impl));
}

ava::core::Result<StagedProjectTrustMutation> stage_clear_project_trust_decision(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir)
{
  auto const trust_path = project_trust_file(paths);
  auto records = load_records_for_write(trust_path);
  if (!records)
    return std::unexpected(std::move(records.error()));
  auto const workspace = normalized_absolute_path(workspace_dir);
  auto const old_size = records->size();
  records->erase(std::remove_if(records->begin(), records->end(), [&](TrustRecord const& record) { return record.path == workspace; }), records->end());

  auto impl = std::make_unique<StagedProjectTrustMutation::Impl>();
  impl->trust_file = trust_path;
  impl->effective_state = effective_staged_state(paths, workspace, *records);
  impl->records = std::move(*records);
  impl->write_required = impl->records.size() != old_size;
  return StagedProjectTrustMutation(std::move(impl));
}

ava::core::VoidResult set_project_trust_decision(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir, bool trusted)
{
  auto staged = stage_set_project_trust_decision(paths, workspace_dir, trusted);
  if (!staged)
    return std::unexpected(std::move(staged.error()));
  return staged->commit();
}

ava::core::VoidResult clear_project_trust_decision(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir)
{
  auto staged = stage_clear_project_trust_decision(paths, workspace_dir);
  if (!staged)
    return std::unexpected(std::move(staged.error()));
  return staged->commit();
}

}  // namespace ava::app
