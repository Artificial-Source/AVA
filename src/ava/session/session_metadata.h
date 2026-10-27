#pragma once

#include "ava/session/session_store.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::session {

inline constexpr std::size_t kMaxSessionNameBytes = 256;
inline constexpr std::size_t kMaxGeneratedSessionTitleBytes = 160;
inline constexpr std::size_t kMaxSessionLabels = 32;
inline constexpr std::size_t kMaxSessionLabelBytes = 64;

struct SessionMetadataView
{
  // The session ID that this view was built from, captured atomically with the
  // metadata entries so callers never pair a stale ID with a fresh view.
  std::string session_id;
  // `name` remains the latest manual value. Presence is tracked separately so
  // a historical name:"" durably suppresses generated titles.
  ava::core::NoLogString name;
  bool has_manual_name = false;
  ava::core::NoLogString generated_title = {};
  std::vector<ava::core::NoLogString> labels;
  std::string labels_updated = {};
  bool archived = false;
  std::string parent_session_id = {};
  std::string source_session_id = {};
  std::string branch_from_entry_id = {};
  std::string branch_origin = {};
  std::string actor = {};
  // Canonical cwd captured once when the session is created. This is runtime
  // metadata, not an ACP binding; adapters may use it without rebasing the
  // session on client-selected state.
  std::filesystem::path original_cwd = {};

  [[nodiscard]] std::string const& effective_title() const noexcept { return has_manual_name || !name.empty() ? name : generated_title; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionMetadataUpdate
{
  std::optional<ava::core::NoLogString> name = std::nullopt;
  std::optional<std::vector<ava::core::NoLogString>> labels = {};
  std::optional<bool> archived = {};
  std::string parent_session_id = {};
  std::string source_session_id = {};
  std::string branch_from_entry_id = {};
  std::string branch_origin = {};
  std::string actor = "rpc";
  std::optional<std::filesystem::path> original_cwd = std::nullopt;
  std::optional<ava::core::NoLogString> generated_title = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<SessionMetadataView> session_metadata_from_entries(std::string session_id, std::vector<SessionEntry> const& entries);
[[nodiscard]] ava::core::Result<SessionMetadataView> load_session_metadata(SessionStore const& store);
[[nodiscard]] ava::core::Result<SessionMetadataView> load_session_metadata(SessionStore const& store, SessionLease const& lease);
[[nodiscard]] ava::core::Result<SessionEntry> make_session_metadata_entry(SessionMetadataUpdate update, std::string parent_entry_id = {});
[[nodiscard]] ava::core::Result<SessionMetadataView> append_session_metadata(SessionStore& store, SessionLease const& lease, SessionMetadataUpdate update);
[[nodiscard]] ava::core::Result<SessionMetadataView> append_session_metadata_ephemeral(SessionStore& store, SessionMetadataUpdate update);

[[nodiscard]] std::string session_metadata_json(SessionMetadataView const& metadata);

}  // namespace ava::session
