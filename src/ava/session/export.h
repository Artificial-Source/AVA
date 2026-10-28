#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/session/portable_sanitization.h"
#include "ava/core/result.h"

#include <string>
#include <vector>

namespace ava::session {

struct ExportOptions
{
  bool include_tool_details = false;
  bool include_metadata = false;
  bool include_compactions = true;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<std::string> format_session_portable_jsonl_checked(std::vector<SessionEntry> const& entries);
[[nodiscard]] ava::core::Result<std::string> format_session_markdown_checked(std::vector<SessionEntry> const& entries, ExportOptions const& options = {});
[[nodiscard]] ava::core::Result<std::string> format_session_html_checked(std::vector<SessionEntry> const& entries, ExportOptions const& options = {});
// Compatibility wrappers cannot propagate a projection error. Command paths use
// the checked variants above; wrappers return an empty string on failure rather
// than rendering a partial session.
[[nodiscard]] std::string format_session_markdown(std::vector<SessionEntry> const& entries, ExportOptions const& options = {});
[[nodiscard]] std::string format_session_html(std::vector<SessionEntry> const& entries, ExportOptions const& options = {});

}  // namespace ava::session
