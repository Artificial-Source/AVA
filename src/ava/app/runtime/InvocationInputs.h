#pragma once

#include "PromptOverrides.h"
#include "ava/debug/print_members_on.h"
#include "ava/agent/tool_visibility.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/session_store.h"

#include <optional>
#include <string>

namespace ava::app::runtime {

// Invocation inputs.
//
// Mirrors of OpenContext describing how this session was opened. None of
// these are restored from the store on resume; each is sourced from the
// OpenContext of the current process invocation.
//
struct InvocationInputs
{
  // Invocation-requested visibility is retained separately so a replacement can re-resolve primary policy from the original CLI boundary.
  ava::agent::ToolVisibilityOptions requested_tool_visibility = {};
  ava::agent::ToolVisibilityOptions tool_visibility = {};
  // Keep bounded selection intent separately from the currently permitted, provenance-carrying canonical selection.
  std::optional<std::string> requested_primary_agent = std::nullopt;
  ava::config::XdgPaths paths;
  bool sessionless;
  bool is_offline_ = false;
  // Resolved once at open time and reused by every runtime history reader.
  ava::session::SessionReadLimits session_read_limits = ava::session::legacy_unbounded_session_read_limits();
  PromptOverrides prompt_overrides = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::app::runtime
