#pragma once
#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace ava::tools {

struct ProviderToolCall
{
  std::string id;
  std::string name;
  std::string arguments_json;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class ToolResultStatus
{
  Success,
  Error,
  Canceled,
};

struct ToolResultPayload
{
  ToolResultStatus status = ToolResultStatus::Success;
  std::string summary;
  std::string content;
  std::string content_type;
  std::string error_category;
  std::string error_code;
  std::string error_message;
  std::string error_details;
  std::string diff;
  std::vector<std::string> changed_paths;
  std::vector<std::string> permission_request_ids;
  bool diff_truncated = false;
  bool truncated = false;
  bool byte_limited = false;
  bool line_limited = false;
  std::optional<std::size_t> output_bytes = std::nullopt;
  std::optional<std::size_t> total_bytes = std::nullopt;
  std::optional<std::size_t> output_lines = std::nullopt;
  std::optional<std::size_t> total_lines = std::nullopt;
  std::optional<std::size_t> start_line = std::nullopt;
  std::optional<std::size_t> end_line = std::nullopt;
  std::optional<std::size_t> next_offset_line = std::nullopt;
  std::optional<std::size_t> omitted_bytes = std::nullopt;
  std::optional<std::size_t> omitted_lines = std::nullopt;
  std::optional<std::size_t> visible_matches = std::nullopt;
  std::optional<std::size_t> total_matches = std::nullopt;
  std::string spill_path;
  bool spill_truncated = false;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ToolDispatchResult
{
  std::string call_id;
  std::string name;
  bool success = false;
  std::string result_text;
  ToolResultPayload payload = {};
  // Model/provider-only one-shot denial guidance. Never enters result_text,
  // ToolResultPayload, timeline, events, audits, RPC, ACP, or Error::format.
  std::string provider_user_guidance = {};

  // provider_user_guidance must never appear in debug/log representations.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::tools
