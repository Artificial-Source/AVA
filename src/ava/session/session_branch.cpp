#include "sys.h"
#include "ava/session/assistant_output.h"
#include "ava/session/attachments.h"
#include "ava/session/record.h"
#include "ava/session/session_branch.h"
#include "ava/session/validation.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <utility>

namespace ava::session {
namespace {

ava::core::Error branch_error(ava::core::ErrorCategory category, std::string message)
{
  return ava::core::Error(category, std::move(message));
}

using ava::core::normalized_absolute_path;

void append_json_string_field(std::string& json, std::string_view key, std::string_view value)
{
  json += ",\"";
  json += key;
  json += "\":\"";
  json += ava::core::json::escape(value);
  json += '"';
}

bool has_control_byte_except_summary_whitespace(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return (byte < 0x20 && ch != '\n' && ch != '\t') || byte == 0x7F;
  });
}

bool has_control_byte(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7F;
  });
}

std::optional<bool> bool_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::nullopt;
  auto const value = object.substr(*start);
  if (value.starts_with("true") && (value.size() == 4 || value[4] == ',' || value[4] == '}' || std::isspace(static_cast<unsigned char>(value[4])) != 0))
    return true;
  if (value.starts_with("false") && (value.size() == 5 || value[5] == ',' || value[5] == '}' || std::isspace(static_cast<unsigned char>(value[5])) != 0))
    return false;
  return std::nullopt;
}

ava::core::VoidResult copy_branch_image_attachments(SessionStore const& source, SessionStore const& created, std::vector<SessionEntry> const& entries,
                                                    std::size_t copy_count)
{
  for (std::size_t index = 0; index < copy_count; ++index)
  {
    auto const& entry = entries[index];
    if (entry.type != EntryType::UserMessage)
      continue;
    auto const sanitized = sanitized_message_data_json(entry.data_json);
    for (auto const& attachment : ava::core::json::objects_in_array_field(sanitized, "attachments"))
    {
      if (bool_field(attachment, "redacted").value_or(false))
        continue;
      auto const byte_size = ava::core::json::integer_field(attachment, "byte_size").value_or(0);
      auto metadata = ImageAttachmentRef{.id = ava::core::json::string_field(attachment, "id").value_or(""),
                                         .mime_type = ava::core::json::string_field(attachment, "mime_type").value_or(""),
                                         .storage_path = ava::core::json::string_field(attachment, "storage_path").value_or(""),
                                         .sha256 = ava::core::json::string_field(attachment, "sha256").value_or(""),
                                         .byte_size = byte_size > 0 ? static_cast<std::size_t>(byte_size) : 0};
      if (auto copied = copy_image_attachment(source, created, metadata); !copied)
      {
        return std::unexpected(std::move(copied.error()));
      }
    }
  }
  return {};
}

ava::core::VoidResult validate_required_text(std::string_view value, std::string_view field, std::size_t max_bytes, bool allow_summary_whitespace)
{
  bool const has_invalid_control = allow_summary_whitespace ? has_control_byte_except_summary_whitespace(value) : has_control_byte(value);
  if (value.empty() || value.size() > max_bytes || has_invalid_control)
  {
    auto error = branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary field is invalid");
    error.with_context("field", std::string(field));
    error.with_context("max_bytes", std::to_string(max_bytes));
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::Result<std::pair<std::size_t, std::size_t>> resolve_branch_summary_range(std::vector<SessionEntry> const& entries, std::string_view root_entry_id,
                                                                                    std::string_view tip_entry_id)
{
  auto const root = std::ranges::find_if(entries, [&](SessionEntry const& entry) { return entry.id == root_entry_id; });
  if (root == entries.end())
  {
    auto error = branch_error(ava::core::ErrorCategory::NotFound, "branch summary root entry not found");
    error.with_context("branch_root_entry_id", std::string(root_entry_id));
    return std::unexpected(std::move(error));
  }
  auto const tip = std::ranges::find_if(entries, [&](SessionEntry const& entry) { return entry.id == tip_entry_id; });
  if (tip == entries.end())
  {
    auto error = branch_error(ava::core::ErrorCategory::NotFound, "branch summary tip entry not found");
    error.with_context("branch_tip_entry_id", std::string(tip_entry_id));
    return std::unexpected(std::move(error));
  }
  auto const root_index = static_cast<std::size_t>(std::distance(entries.begin(), root));
  auto const tip_index = static_cast<std::size_t>(std::distance(entries.begin(), tip));
  if (root_index > tip_index)
  {
    auto error = branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary root must not be after tip");
    error.with_context("branch_root_entry_id", std::string(root_entry_id));
    error.with_context("branch_tip_entry_id", std::string(tip_entry_id));
    return std::unexpected(std::move(error));
  }
  return std::pair<std::size_t, std::size_t>{root_index, tip_index};
}

SessionEntry const* find_matching_branch_summary(std::vector<SessionEntry> const& entries, std::string_view source_session_id, std::string_view root_entry_id,
                                                 std::string_view tip_entry_id)
{
  auto const matching = std::ranges::find_if(entries, [&](SessionEntry const& entry) {
    return entry.type == EntryType::BranchSummary && ava::core::json::string_field(entry.data_json, "source_session_id") == source_session_id &&
           ava::core::json::string_field(entry.data_json, "branch_root_entry_id") == root_entry_id &&
           ava::core::json::string_field(entry.data_json, "branch_tip_entry_id") == tip_entry_id;
  });
  return matching == entries.end() ? nullptr : &*matching;
}

bool same_entries(std::vector<SessionEntry> const& lhs, std::vector<SessionEntry> const& rhs)
{
  return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](SessionEntry const& left, SessionEntry const& right) {
           return left.id == right.id && left.parent_id == right.parent_id && left.type == right.type && left.timestamp == right.timestamp &&
                  left.data_json == right.data_json && left.version == right.version;
         });
}

ava::core::Result<std::size_t> copy_count_for_branch(std::vector<SessionEntry> const& entries, std::string_view branch_from_entry_id, SessionBranchMode mode,
                                                     std::string& resolved_branch_from_entry_id)
{
  resolved_branch_from_entry_id = std::string(branch_from_entry_id);
  if (entries.empty())
  {
    if (resolved_branch_from_entry_id.empty())
      return std::size_t{0};
    auto error = branch_error(ava::core::ErrorCategory::NotFound, "branch source entry not found");
    error.with_context("branch_from_entry_id", resolved_branch_from_entry_id);
    return std::unexpected(std::move(error));
  }

  if (resolved_branch_from_entry_id.empty())
    resolved_branch_from_entry_id = entries.back().id;
  auto const found = std::ranges::find_if(entries, [&](SessionEntry const& entry) { return entry.id == resolved_branch_from_entry_id; });
  if (found == entries.end())
  {
    auto error = branch_error(ava::core::ErrorCategory::NotFound, "branch source entry not found");
    error.with_context("branch_from_entry_id", resolved_branch_from_entry_id);
    return std::unexpected(std::move(error));
  }

  if (mode == SessionBranchMode::Clone)
    return entries.size();
  return static_cast<std::size_t>(std::distance(entries.begin(), found)) + 1;
}

}  // namespace

void rollback_created_session_with_context(SessionStore const& store, SessionLease const& lease, ava::core::Error& error)
{
  error.with_context("created_session_id", store.options_r()->session_id);
  if (auto removed = store.remove_created_file(lease); !removed)
  {
    error.with_context("rollback_path", store.session_path().string());
    error.with_context("rollback_cause", removed.error().format());
  }

  auto const attachment_path = attachment_storage_root(store);
  std::error_code inspection_error;
  auto const attachment_status = std::filesystem::symlink_status(attachment_path, inspection_error);
  if (inspection_error)
  {
    if (inspection_error != std::errc::no_such_file_or_directory)
    {
      error.with_context("rollback_attachment_path", attachment_path.string());
      error.with_context("rollback_attachment_disposition", "preserved");
      error.with_context("rollback_attachment_inspection_cause", inspection_error.message());
    }
  }
  else if (std::filesystem::exists(attachment_status))
  {
    error.with_context("rollback_attachment_path", attachment_path.string());
    error.with_context("rollback_attachment_disposition", "preserved");
  }
}

std::string_view branch_summary_eligibility_reason_text(BranchSummaryEligibilityReason reason) noexcept
{
  switch (reason)
  {
    case BranchSummaryEligibilityReason::Eligible:
      return "eligible";
    case BranchSummaryEligibilityReason::ForkEntryNotFound:
      return "fork entry is not present in the source session";
    case BranchSummaryEligibilityReason::NoSubstantiveEntriesAfterFork:
      return "source session has no substantive entries after the fork";
    case BranchSummaryEligibilityReason::ExistingSummary:
      return "this exact source range already has a branch summary";
  }
  return "branch summary is unavailable";
}

BranchSummaryCoverage inspect_branch_summary_coverage(std::vector<SessionEntry> const& entries, std::string_view source_session_id,
                                                      std::string_view fork_entry_id)
{
  BranchSummaryCoverage coverage{.source_session_id = std::string(source_session_id),
                                 .fork_entry_id = std::string(fork_entry_id),
                                 .branch_root_entry_id = {},
                                 .branch_tip_entry_id = {},
                                 .prompt_entry_indices = {},
                                 .existing_summary = std::nullopt,
                                 .reason = BranchSummaryEligibilityReason::Eligible};
  auto const fork = std::ranges::find_if(entries, [&](SessionEntry const& entry) { return entry.id == fork_entry_id; });
  if (fork == entries.end())
  {
    coverage.reason = BranchSummaryEligibilityReason::ForkEntryNotFound;
    return coverage;
  }

  auto const fork_index = static_cast<std::size_t>(std::distance(entries.begin(), fork));
  for (std::size_t index = fork_index + 1; index < entries.size(); ++index)
  {
    if (entries[index].type == EntryType::BranchSummary)
      continue;
    if (coverage.prompt_entry_indices.empty())
      coverage.branch_root_entry_id = entries[index].id;
    coverage.branch_tip_entry_id = entries[index].id;
    coverage.prompt_entry_indices.push_back(index);
  }
  if (coverage.prompt_entry_indices.empty())
  {
    coverage.reason = BranchSummaryEligibilityReason::NoSubstantiveEntriesAfterFork;
    return coverage;
  }

  if (auto const* existing = find_matching_branch_summary(entries, source_session_id, coverage.branch_root_entry_id, coverage.branch_tip_entry_id))
  {
    coverage.existing_summary = *existing;
    coverage.reason = BranchSummaryEligibilityReason::ExistingSummary;
  }
  return coverage;
}

ava::core::Result<std::vector<SessionEntry>> extract_branch_summary_prompt_range(std::vector<SessionEntry> const& entries,
                                                                                 BranchSummaryCoverage const& coverage)
{
  if (coverage.reason != BranchSummaryEligibilityReason::Eligible && coverage.reason != BranchSummaryEligibilityReason::ExistingSummary)
    return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary coverage has no substantive prompt range"));
  if (coverage.prompt_entry_indices.empty())
    return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary prompt range is empty"));
  auto const expected = inspect_branch_summary_coverage(entries, coverage.source_session_id, coverage.fork_entry_id);
  if (expected.branch_root_entry_id != coverage.branch_root_entry_id || expected.branch_tip_entry_id != coverage.branch_tip_entry_id ||
      expected.prompt_entry_indices != coverage.prompt_entry_indices || expected.reason != coverage.reason)
  {
    return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary coverage no longer matches loaded entries"));
  }

  std::vector<SessionEntry> selected;
  selected.reserve(coverage.prompt_entry_indices.size());
  for (std::size_t const index : coverage.prompt_entry_indices)
  {
    if (index >= entries.size() || entries[index].type == EntryType::BranchSummary)
      return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary prompt range no longer matches loaded entries"));
    selected.push_back(entries[index]);
  }
  if (selected.front().id != coverage.branch_root_entry_id || selected.back().id != coverage.branch_tip_entry_id)
    return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary prompt range identity no longer matches loaded entries"));
  return selected;
}

ava::core::Result<SessionBranchResult> create_session_branch(SessionBranchOptions options)
{
  if (options.source_session_id.empty())
  {
    return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "source session id is required"));
  }
  if (auto valid_source = validate_session_id(options.source_session_id); !valid_source)
  {
    return std::unexpected(std::move(valid_source.error()));
  }
  if (options.mode == SessionBranchMode::Clone && !options.branch_from_entry_id.empty())
  {
    auto error = branch_error(ava::core::ErrorCategory::InvalidArgument, "clone session branch does not support branch_from_entry_id");
    error.with_context("branch_from_entry_id", options.branch_from_entry_id);
    return std::unexpected(std::move(error));
  }
  if (auto valid_entry = validate_parent_id(options.branch_from_entry_id, "session_branch"); !valid_entry)
  {
    return std::unexpected(std::move(valid_entry.error()));
  }

  auto source = SessionStore::open(options.workspace_dir, options.source_session_id, options.root_dir);
  if (!source)
    return std::unexpected(std::move(source.error()));
  std::optional<SessionLease> owned_source_lease;
  SessionLease const* source_lease = options.source_lease;
  if (source_lease == nullptr)
  {
    auto acquired = SessionLease::acquire(source->session_path());
    if (!acquired)
      return std::unexpected(std::move(acquired.error()));
    owned_source_lease.emplace(std::move(*acquired));
    source_lease = &*owned_source_lease;
  }
  if (!source_lease->active() || source_lease->canonical_path() != normalized_absolute_path(source->session_path()))
  {
    auto error = branch_error(ava::core::ErrorCategory::InvalidArgument, "source branch lease does not exactly match the source session");
    error.with_context("source_session_id", options.source_session_id);
    return std::unexpected(std::move(error));
  }
  auto source_entries = source->load_bounded(*source_lease, options.read_limits.value_or(legacy_unbounded_session_read_limits()));
  if (!source_entries)
    return std::unexpected(std::move(source_entries.error()));
  auto source_metadata = session_metadata_from_entries({}, *source_entries);
  if (!source_metadata)
    return std::unexpected(std::move(source_metadata.error()));

  std::string resolved_branch_from_entry_id;
  auto copy_count = copy_count_for_branch(*source_entries, options.branch_from_entry_id, options.mode, resolved_branch_from_entry_id);
  if (!copy_count)
    return std::unexpected(std::move(copy_count.error()));

  std::vector<SessionEntry> const prefix(source_entries->begin(), source_entries->begin() + static_cast<std::ptrdiff_t>(*copy_count));
  auto const output_projection = classify_assistant_output(prefix);
  if (!output_projection.diagnostics.empty())
  {
    auto error = branch_error(ava::core::ErrorCategory::Session,
                              "branch prefix contains an assistant-output diagnostic; choose a committed boundary or recover the source session");
    error.with_context("source_session_id", options.source_session_id);
    error.with_context("diagnostic_kind", std::string(to_string(output_projection.diagnostics.front().kind)));
    error.with_context("diagnostic_entry_id", output_projection.diagnostics.front().entry_id);
    return std::unexpected(std::move(error));
  }

  auto created = SessionStore::create(options.workspace_dir, options.root_dir);
  if (!created)
    return std::unexpected(std::move(created.error()));
  auto destination_lease = SessionLease::create_and_acquire(created->session_path());
  if (!destination_lease)
  {
    auto error = std::move(destination_lease.error());
    error.with_context("created_session_id", created->options_r()->session_id);
    return std::unexpected(std::move(error));
  }

  if (auto copied = created->append_validated_copy(*destination_lease, prefix); !copied)
  {
    auto error = std::move(copied.error());
    error.with_context("source_session_id", options.source_session_id);
    rollback_created_session_with_context(*created, *destination_lease, error);
    return std::unexpected(std::move(error));
  }

  if (auto copied_attachments = copy_branch_image_attachments(*source, *created, *source_entries, *copy_count); !copied_attachments)
  {
    auto error = std::move(copied_attachments.error());
    error.with_context("source_session_id", options.source_session_id);
    rollback_created_session_with_context(*created, *destination_lease, error);
    return std::unexpected(std::move(error));
  }

  SessionMetadataUpdate metadata_update;
  metadata_update.name = std::move(options.name);
  if (!metadata_update.name && source_metadata->has_manual_name)
    metadata_update.name = source_metadata->name;
  if (!source_metadata->generated_title.empty())
    metadata_update.generated_title = source_metadata->generated_title;
  metadata_update.labels = std::move(options.labels);
  metadata_update.parent_session_id = options.source_session_id;
  metadata_update.source_session_id = options.source_session_id;
  metadata_update.branch_from_entry_id = resolved_branch_from_entry_id;
  metadata_update.branch_origin = options.mode == SessionBranchMode::Clone ? "clone" : "fork";
  metadata_update.actor = std::move(options.actor);
  auto metadata = append_session_metadata(*created, *destination_lease, std::move(metadata_update));
  if (!metadata)
  {
    auto error = std::move(metadata.error());
    error.with_context("source_session_id", options.source_session_id);
    rollback_created_session_with_context(*created, *destination_lease, error);
    return std::unexpected(std::move(error));
  }

  return SessionBranchResult{.store = std::move(*created),
                             .lease = std::move(*destination_lease),
                             .source_session_id = std::move(options.source_session_id),
                             .branch_from_entry_id = std::move(resolved_branch_from_entry_id),
                             .copied_entry_count = *copy_count,
                             .metadata = std::move(*metadata)};
}

ava::core::Result<BranchSummaryResult> prepare_branch_summary(BranchSummaryOptions options)
{
  if (!options.read_limits)
    return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary requires explicit session read limits"));
  if (options.source_session_id.empty())
    return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "source session id is required"));
  if (auto valid_source = validate_session_id(options.source_session_id); !valid_source)
    return std::unexpected(std::move(valid_source.error()));
  if (auto valid_root = validate_parent_id(options.branch_root_entry_id, "branch_summary"); !valid_root)
    return std::unexpected(std::move(valid_root.error()));
  if (auto valid_tip = validate_parent_id(options.branch_tip_entry_id, "branch_summary"); !valid_tip)
    return std::unexpected(std::move(valid_tip.error()));
  if (auto valid = validate_required_text(options.summary, "summary", 8192, true); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto valid = validate_required_text(options.provider, "provider", 256, false); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto valid = validate_required_text(options.model, "model", 256, false); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto valid = validate_required_text(options.reason, "reason", 1024, false); !valid)
    return std::unexpected(std::move(valid.error()));
  if (!options.actor.empty())
  {
    if (auto valid = validate_required_text(options.actor, "actor", 64, false); !valid)
      return std::unexpected(std::move(valid.error()));
  }

  auto source = SessionStore::open(options.workspace_dir, options.source_session_id, options.root_dir);
  if (!source)
    return std::unexpected(std::move(source.error()));
  std::optional<SessionLease> owned_source_lease;
  SessionLease const* source_lease = options.source_lease;
  if (source_lease == nullptr)
  {
    auto acquired = SessionLease::acquire(source->session_path());
    if (!acquired)
      return std::unexpected(std::move(acquired.error()));
    owned_source_lease.emplace(std::move(*acquired));
    source_lease = &*owned_source_lease;
  }
  if (!source_lease->active() || source_lease->canonical_path() != normalized_absolute_path(source->session_path()))
  {
    auto error = branch_error(ava::core::ErrorCategory::InvalidArgument, "source branch summary lease does not exactly match the source session");
    error.with_context("source_session_id", options.source_session_id);
    return std::unexpected(std::move(error));
  }

  auto validate_snapshot = [&](std::vector<SessionEntry> const& snapshot) -> ava::core::VoidResult {
    if (snapshot.empty())
      return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "source session has no entries"));
    auto const assistant_output = classify_assistant_output(snapshot);
    if (!assistant_output.diagnostics.empty())
    {
      auto error = branch_error(ava::core::ErrorCategory::Session,
                                "branch summary source contains an assistant-output diagnostic; recover or commit the staged turn before appending");
      error.with_context("source_session_id", options.source_session_id)
          .with_context("diagnostic_kind", std::string(to_string(assistant_output.diagnostics.front().kind)))
          .with_context("diagnostic_entry_id", assistant_output.diagnostics.front().entry_id);
      return std::unexpected(std::move(error));
    }
    auto range = resolve_branch_summary_range(snapshot, options.branch_root_entry_id, options.branch_tip_entry_id);
    if (!range)
      return std::unexpected(std::move(range.error()));
    return {};
  };

  auto entries = source->load_bounded(*source_lease, *options.read_limits, options.cancel_requested);
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  if (auto valid = validate_snapshot(*entries); !valid)
    return std::unexpected(std::move(valid.error()));

  // Revalidate the exact bounded content immediately before constructing an
  // owner-routed append. The retained lease keeps both reads on one inode.
  auto latest_entries = source->load_bounded(*source_lease, *options.read_limits, options.cancel_requested);
  if (!latest_entries)
    return std::unexpected(std::move(latest_entries.error()));
  if (auto valid = validate_snapshot(*latest_entries); !valid)
    return std::unexpected(std::move(valid.error()));
  if (!same_entries(*entries, *latest_entries))
  {
    auto error = branch_error(ava::core::ErrorCategory::Session, "source session changed while appending branch summary");
    error.with_context("source_session_id", options.source_session_id);
    return std::unexpected(std::move(error));
  }

  if (auto const* existing =
          find_matching_branch_summary(*latest_entries, options.source_session_id, options.branch_root_entry_id, options.branch_tip_entry_id))
  {
    return BranchSummaryResult{
        .source_session_id = std::move(options.source_session_id), .entry = *existing, .disposition = BranchSummaryDisposition::Existing};
  }

  std::string data = "{\"schema_version\":1";
  append_json_string_field(data, "summary", options.summary);
  append_json_string_field(data, "source_session_id", options.source_session_id);
  append_json_string_field(data, "branch_root_entry_id", options.branch_root_entry_id);
  append_json_string_field(data, "branch_tip_entry_id", options.branch_tip_entry_id);
  append_json_string_field(data, "provider", options.provider);
  append_json_string_field(data, "model", options.model);
  append_json_string_field(data, "reason", options.reason);
  if (!options.actor.empty())
    append_json_string_field(data, "actor", options.actor);
  data += '}';

  auto entry = SessionEntry{.id = ava::core::make_id("entry"),
                            .parent_id = latest_entries->back().id,
                            .type = EntryType::BranchSummary,
                            .timestamp = now_timestamp(),
                            .data_json = std::move(data)};
  return BranchSummaryResult{
      .source_session_id = std::move(options.source_session_id), .entry = std::move(entry), .disposition = BranchSummaryDisposition::Appended};
}

ava::core::Result<BranchSummaryResult> append_branch_summary(BranchSummaryOptions options)
{
  if (!options.read_limits)
    return std::unexpected(branch_error(ava::core::ErrorCategory::InvalidArgument, "branch summary requires explicit session read limits"));
  auto source = SessionStore::open(options.workspace_dir, options.source_session_id, options.root_dir);
  if (!source)
    return std::unexpected(std::move(source.error()));

  std::optional<SessionLease> owned_source_lease;
  SessionLease const* source_lease = options.source_lease;
  if (source_lease == nullptr)
  {
    auto acquired = SessionLease::acquire(source->session_path());
    if (!acquired)
      return std::unexpected(std::move(acquired.error()));
    owned_source_lease.emplace(std::move(*acquired));
    source_lease = &*owned_source_lease;
    options.source_lease = source_lease;
  }
  auto const read_limits = *options.read_limits;
  auto const cancel_requested = options.cancel_requested;

  auto prepared = prepare_branch_summary(std::move(options));
  if (!prepared)
    return std::unexpected(std::move(prepared.error()));
  if (prepared->disposition == BranchSummaryDisposition::Existing)
    return prepared;

  auto target = SessionAppendTarget::create_persistent(*source, *source_lease, read_limits, cancel_requested);
  if (!target)
    return std::unexpected(std::move(target.error()));
  auto appended = (*target)->append_branch_summary_if_absent(prepared->entry, cancel_requested);
  if (!appended)
    return std::unexpected(std::move(appended.error()));
  prepared->entry = std::move(appended->entry);
  prepared->disposition = appended->disposition == SessionAppendDisposition::Existing ? BranchSummaryDisposition::Existing : BranchSummaryDisposition::Appended;
  return prepared;
}

}  // namespace ava::session
