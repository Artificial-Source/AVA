#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ava::event {

// Extracted tool-related slice of an Event, carrying the call identifiers, arguments, (structured) result, status, error fields, diff, changed paths and the
// byte/line accounting produced by file and search tools.
struct ToolPayload
{
  ava::core::NoLogString text;
  std::string call_id;
  std::string tool;
  ava::core::NoLogString args_json;
  ava::core::NoLogString result_json;
  ava::core::NoLogString structured_result_json;
  std::string status;
  std::string error_category;
  std::string error_code;
  ava::core::NoLogString error_message;
  ava::core::NoLogString error_details;
  std::string content_type;
  ava::core::NoLogString diff;
  std::vector<ava::core::NoLogString> changed_paths;
  std::vector<std::string> permission_request_ids;
  ava::core::NoLogString spill_path;
  bool diff_truncated = false;
  bool truncated = false;
  bool byte_limited = false;
  bool line_limited = false;
  bool spill_truncated = false;
  std::size_t output_bytes = 0;
  std::size_t total_bytes = 0;
  std::size_t output_lines = 0;
  std::size_t total_lines = 0;
  std::size_t start_line = 0;
  std::size_t end_line = 0;
  std::size_t next_offset_line = 0;
  std::size_t omitted_bytes = 0;
  std::size_t omitted_lines = 0;
  std::size_t visible_matches = 0;
  std::size_t total_matches = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::event
