#include "sys.h"
#include "ava/session/record.h"
#include "ava/session/session_metadata.h"
#include "ava/core/ascii.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <string>
#include <utility>

namespace ava::session {
namespace {

ava::core::Error metadata_error(std::string message, std::string_view field = {})
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  if (!field.empty())
    error.with_context("field", std::string(field));
  return error;
}

ava::core::VoidResult validate_title_field(std::optional<std::string> const& value, std::string_view field, std::size_t max_bytes, bool allow_empty)
{
  if (!value)
    return {};
  if ((!allow_empty && value->empty()) || value->size() > max_bytes || ava::core::has_c0_or_del(*value) || !ava::core::json::is_valid_utf8(*value))
  {
    auto error = metadata_error("session title field is invalid", field);
    error.with_context("max_bytes", std::to_string(max_bytes));
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::VoidResult validate_labels(std::optional<std::vector<std::string>> const& labels)
{
  if (!labels)
    return {};
  if (labels->size() > kMaxSessionLabels)
  {
    auto error = metadata_error("too many session labels", "labels");
    error.with_context("max_labels", std::to_string(kMaxSessionLabels));
    return std::unexpected(std::move(error));
  }
  std::vector<std::string> seen;
  for (auto const& label : *labels)
  {
    if (label.empty() || label.size() > kMaxSessionLabelBytes || ava::core::has_c0_or_del(label))
    {
      auto error = metadata_error("session label is invalid", "labels");
      error.with_context("max_bytes", std::to_string(kMaxSessionLabelBytes));
      return std::unexpected(std::move(error));
    }
    if (std::ranges::find(seen, label) != seen.end())
    {
      auto error = metadata_error("session labels must be unique", "labels");
      error.with_context("label", label);
      return std::unexpected(std::move(error));
    }
    seen.push_back(label);
  }
  return {};
}

ava::core::VoidResult validate_optional_session_id(std::string_view value, std::string_view field)
{
  if (value.empty())
    return {};
  auto valid = validate_session_id(value);
  if (!valid)
  {
    auto error = metadata_error("session metadata contains invalid session id", field);
    error.with_context("session_id", std::string(value));
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::VoidResult validate_optional_entry_id(std::string_view value, std::string_view field)
{
  if (value.empty())
    return {};
  auto valid = validate_parent_id(value, "session_metadata");
  if (!valid)
  {
    auto error = metadata_error("session metadata contains invalid entry id", field);
    error.with_context("entry_id", std::string(value));
    return std::unexpected(std::move(error));
  }
  return {};
}

bool valid_branch_origin(std::string_view origin)
{
  return origin.empty() || origin == "root" || origin == "fork" || origin == "clone" || origin == "manual" || origin == "import";
}

bool is_canonical_absolute_path(std::filesystem::path const& path)
{
  return path.is_absolute() && path.lexically_normal() == path;
}

ava::core::VoidResult validate_update(SessionMetadataUpdate const& update)
{
  if (!update.name && !update.generated_title && !update.labels && !update.archived && update.parent_session_id.empty() && update.source_session_id.empty() &&
      update.branch_from_entry_id.empty() && update.branch_origin.empty() && !update.original_cwd)
  {
    return std::unexpected(metadata_error("session metadata update is empty"));
  }
  if (auto valid = validate_title_field(update.name, "name", kMaxSessionNameBytes, true); !valid)
    return valid;
  if (auto valid = validate_title_field(update.generated_title, "generated_title", kMaxGeneratedSessionTitleBytes, false); !valid)
    return valid;
  if (auto valid = validate_labels(update.labels); !valid)
    return valid;
  if (auto valid = validate_optional_session_id(update.parent_session_id, "parent_session_id"); !valid)
    return valid;
  if (auto valid = validate_optional_session_id(update.source_session_id, "source_session_id"); !valid)
    return valid;
  if (auto valid = validate_optional_entry_id(update.branch_from_entry_id, "branch_from_entry_id"); !valid)
    return valid;
  if (!valid_branch_origin(update.branch_origin))
  {
    auto error = metadata_error("session metadata branch_origin is unsupported", "branch_origin");
    error.with_context("branch_origin", update.branch_origin);
    return std::unexpected(std::move(error));
  }
  if (!update.actor.empty() && (update.actor.size() > kMaxSessionLabelBytes || ava::core::has_c0_or_del(update.actor)))
  {
    return std::unexpected(metadata_error("session metadata actor is invalid", "actor"));
  }
  if (update.original_cwd && (!is_canonical_absolute_path(*update.original_cwd) || ava::core::has_c0_or_del(update.original_cwd->string())))
    return std::unexpected(metadata_error("session metadata original_cwd must be a canonical absolute path", "original_cwd"));
  return {};
}

std::vector<std::string> labels_from_entry(SessionEntry const& entry)
{
  auto const start = ava::core::json::field_value_start(entry.data_json, "labels");
  if (!start)
    return {};
  return ava::core::json::strings_in_array_field(entry.data_json, "labels");
}

std::string labels_json(std::vector<std::string> const& labels)
{
  std::string json = "[";
  for (std::size_t index = 0; index < labels.size(); ++index)
  {
    if (index > 0)
      json += ',';
    json += "\"" + ava::core::json::escape(labels[index]) + "\"";
  }
  json += ']';
  return json;
}

std::optional<bool> bool_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::nullopt;
  if (object.substr(*start, 4) == "true")
    return true;
  if (object.substr(*start, 5) == "false")
    return false;
  return std::nullopt;
}

void append_string_field(std::string& json, bool& first, std::string_view key, std::string_view value)
{
  if (!first)
    json += ',';
  first = false;
  json += "\"" + std::string(key) + "\":\"" + ava::core::json::escape(value) + "\"";
}

void append_bool_field(std::string& json, bool& first, std::string_view key, bool value)
{
  if (!first)
    json += ',';
  first = false;
  json += "\"" + std::string(key) + "\":" + (value ? "true" : "false");
}

}  // namespace

ava::core::Result<SessionMetadataView> session_metadata_from_entries(std::string session_id, std::vector<SessionEntry> const& entries)
{
  SessionMetadataView metadata;
  metadata.session_id = std::move(session_id);
  for (auto const& entry : entries)
  {
    if (entry.type != EntryType::SessionMetadata)
      continue;
    if (!ava::core::json::is_valid_object(entry.data_json))
    {
      return std::unexpected(metadata_error("session metadata entry data is not valid JSON"));
    }
    if (auto const name_start = ava::core::json::field_value_start(entry.data_json, "name"))
    {
      (void)name_start;
      auto name = ava::core::json::string_field(entry.data_json, "name");
      if (!name)
        return std::unexpected(metadata_error("session metadata name must be a string", "name"));
      metadata.name = std::move(*name);
      metadata.has_manual_name = true;
    }
    if (auto const generated_start = ava::core::json::field_value_start(entry.data_json, "generated_title"))
    {
      (void)generated_start;
      auto title = ava::core::json::string_field(entry.data_json, "generated_title");
      if (!title)
        return std::unexpected(metadata_error("session metadata generated_title must be a string", "generated_title"));
      metadata.generated_title = std::move(*title);
    }
    if (ava::core::json::field_value_start(entry.data_json, "labels"))
    {
      metadata.labels = labels_from_entry(entry);
      metadata.labels_updated = entry.timestamp;
    }
    if (ava::core::json::field_value_start(entry.data_json, "archived"))
    {
      auto archived = bool_field(entry.data_json, "archived");
      if (!archived)
        return std::unexpected(metadata_error("session metadata archived must be a boolean", "archived"));
      metadata.archived = *archived;
    }
    if (auto parent = ava::core::json::string_field(entry.data_json, "parent_session_id"); parent && !parent->empty())
    {
      metadata.parent_session_id = std::move(*parent);
    }
    if (auto source = ava::core::json::string_field(entry.data_json, "source_session_id"); source && !source->empty())
    {
      metadata.source_session_id = std::move(*source);
    }
    if (auto branch_from = ava::core::json::string_field(entry.data_json, "branch_from_entry_id"); branch_from && !branch_from->empty())
    {
      metadata.branch_from_entry_id = std::move(*branch_from);
    }
    if (auto origin = ava::core::json::string_field(entry.data_json, "branch_origin"); origin && !origin->empty())
    {
      metadata.branch_origin = std::move(*origin);
    }
    if (auto actor = ava::core::json::string_field(entry.data_json, "actor"); actor && !actor->empty())
    {
      metadata.actor = std::move(*actor);
    }
    if (auto cwd = ava::core::json::string_field(entry.data_json, "original_cwd"); cwd && !cwd->empty())
    {
      std::filesystem::path candidate(*cwd);
      if (!is_canonical_absolute_path(candidate))
        return std::unexpected(metadata_error("session metadata original_cwd is invalid", "original_cwd"));
      if (!metadata.original_cwd.empty() && metadata.original_cwd != candidate)
        return std::unexpected(metadata_error("session metadata original_cwd is immutable", "original_cwd"));
      metadata.original_cwd = std::move(candidate);
    }
  }
  return metadata;
}

//FIXME: the Session that store belongs to must be locked.
ava::core::Result<SessionMetadataView> load_session_metadata(SessionStore const& store)
{
  auto entries = store.load();
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  return session_metadata_from_entries(store.options_r()->session_id, *entries);
}

//FIXME: the Session that store belongs to must be locked.
ava::core::Result<SessionMetadataView> load_session_metadata(SessionStore const& store, SessionLease const& lease)
{
  auto entries = store.load(lease);
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  return session_metadata_from_entries(store.options_r()->session_id, *entries);
}

ava::core::Result<SessionEntry> make_session_metadata_entry(SessionMetadataUpdate update, std::string parent_entry_id)
{
  if (auto valid = validate_update(update); !valid)
    return std::unexpected(std::move(valid.error()));
  auto const entry_id = ava::core::make_id("entry");
  if (auto valid_parent = validate_parent_id(parent_entry_id, entry_id); !valid_parent)
  {
    return std::unexpected(std::move(valid_parent.error()));
  }

  std::string data = "{\"schema_version\":1";
  bool first = false;
  if (update.name)
    append_string_field(data, first, "name", *update.name);
  if (update.generated_title)
    append_string_field(data, first, "generated_title", *update.generated_title);
  if (update.labels)
  {
    if (!first)
      data += ',';
    first = false;
    data += "\"labels\":" + labels_json(*update.labels);
  }
  if (update.archived)
    append_bool_field(data, first, "archived", *update.archived);
  if (!update.parent_session_id.empty())
    append_string_field(data, first, "parent_session_id", update.parent_session_id);
  if (!update.source_session_id.empty())
    append_string_field(data, first, "source_session_id", update.source_session_id);
  if (!update.branch_from_entry_id.empty())
    append_string_field(data, first, "branch_from_entry_id", update.branch_from_entry_id);
  if (!update.branch_origin.empty())
    append_string_field(data, first, "branch_origin", update.branch_origin);
  if (!update.actor.empty())
    append_string_field(data, first, "actor", update.actor);
  if (update.original_cwd)
    append_string_field(data, first, "original_cwd", update.original_cwd->string());
  data += '}';

  return SessionEntry{
      .id = entry_id, .parent_id = std::move(parent_entry_id), .type = EntryType::SessionMetadata, .timestamp = now_timestamp(), .data_json = std::move(data)};
}

namespace {

// FIXME: the Session of session_id and entries must be locked.
template <typename Append>
ava::core::Result<SessionMetadataView> append_session_metadata_impl(std::string session_id, ava::core::Result<std::vector<SessionEntry>> entries,
                                                                    SessionMetadataUpdate update, Append&& append)
{
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  auto parent_entry_id = entries->empty() ? std::string{} : entries->back().id;
  auto entry = make_session_metadata_entry(std::move(update), std::move(parent_entry_id));
  if (!entry)
    return std::unexpected(std::move(entry.error()));
  if (auto appended = append(*entry); !appended)
    return std::unexpected(std::move(appended.error()));
  entries->push_back(std::move(*entry));
  return session_metadata_from_entries(std::move(session_id), *entries);
}

}  // namespace

ava::core::Result<SessionMetadataView> append_session_metadata(SessionStore& store, SessionLease const& lease, SessionMetadataUpdate update)
{
  return append_session_metadata_impl(store.options_session_id_copy(), store.load(lease), std::move(update),
                                      [&](SessionEntry const& entry) { return store.append(lease, entry); });
}

ava::core::Result<SessionMetadataView> append_session_metadata_ephemeral(SessionStore& store, SessionMetadataUpdate update)
{
  return append_session_metadata_impl(store.options_r()->session_id, store.load(), std::move(update),
                                      [&](SessionEntry const& entry) { return store.append_ephemeral(entry); });
}

std::string session_metadata_json(SessionMetadataView const& metadata)
{
  std::string json = "{";
  json += "\"session_id\":\"" + ava::core::json::escape(metadata.session_id) + "\"";
  json += ",\"name\":\"" + ava::core::json::escape(metadata.name) + "\"";
  json += ",\"has_manual_name\":" + std::string(metadata.has_manual_name ? "true" : "false");
  json += ",\"generated_title\":\"" + ava::core::json::escape(metadata.generated_title) + "\"";
  json += ",\"title\":\"" + ava::core::json::escape(metadata.effective_title()) + "\"";
  json += ",\"labels\":" + labels_json(metadata.labels);
  json += ",\"labels_updated\":\"" + ava::core::json::escape(metadata.labels_updated) + "\"";
  json += ",\"archived\":" + std::string(metadata.archived ? "true" : "false");
  json += ",\"parent_session_id\":\"" + ava::core::json::escape(metadata.parent_session_id) + "\"";
  json += ",\"source_session_id\":\"" + ava::core::json::escape(metadata.source_session_id) + "\"";
  json += ",\"branch_from_entry_id\":\"" + ava::core::json::escape(metadata.branch_from_entry_id) + "\"";
  json += ",\"branch_origin\":\"" + ava::core::json::escape(metadata.branch_origin) + "\"";
  json += ",\"actor\":\"" + ava::core::json::escape(metadata.actor) + "\"";
  json += ",\"original_cwd\":\"" + ava::core::json::escape(metadata.original_cwd.string()) + "\"";
  json += '}';
  return json;
}

}  // namespace ava::session
