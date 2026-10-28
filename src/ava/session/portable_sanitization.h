#pragma once

#include "ava/session/session_store.h"

#include <string>
#include <string_view>

namespace ava::session {

inline constexpr std::string_view kPublicSessionErrorOmission = "[Session error diagnostics omitted from public projection.]";

// Rebuilds portable records from explicit allowlists so provider-private
// replay material and attachment-storage references never cross export/import
// boundaries.
[[nodiscard]] SessionEntry sanitize_session_entry_for_portable_jsonl_export(SessionEntry entry);

// Historical failed external-tool records are rebuilt from fixed diagnostics.
// The reserved mcp_/plugin_ prefixes are the compatibility identity boundary;
// malformed failed records with either prefix are sanitized rather than replayed.
[[nodiscard]] std::string sanitized_tool_result_data_json(SessionEntry const& entry, bool preserve_output_binding);

// Imported and legacy Error records may contain provider-controlled details.
// Public projections retain no diagnostic payload from those physical records.
[[nodiscard]] SessionEntry sanitize_session_error_for_public_projection(SessionEntry entry);

}  // namespace ava::session
