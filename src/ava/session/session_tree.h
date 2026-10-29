#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/session/session_metadata.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ava::session {

struct SessionTreeNode
{
  SessionSummary summary;
  SessionMetadataView metadata;
  std::vector<std::string> children;
  bool current = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionTreeIndex
{
  std::string current_session_id;
  std::vector<SessionTreeNode> sessions;
  std::vector<std::string> roots;
  std::vector<std::string> leaves;
  std::vector<std::string> current_path;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<SessionTreeIndex> build_session_tree(std::filesystem::path const& workspace_dir, std::filesystem::path const& root_dir,
                                                                     std::string_view current_session_id = {});

// Retargets only the value-owned current marker and branch path. No session file
// or metadata is reopened, reparsed, or otherwise consulted.
void retarget_session_tree(SessionTreeIndex& index, std::string_view current_session_id);

// Replaces one value-owned node after an authoritative single-session read.
// Parent changes are topology mutations and are rejected for a full rebuild.
[[nodiscard]] bool refresh_session_tree_node(SessionTreeIndex& index, SessionSummary summary, SessionMetadataView metadata);

}  // namespace ava::session
