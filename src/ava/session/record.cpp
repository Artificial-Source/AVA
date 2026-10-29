#include "sys.h"
#include "ava/session/record.h"
#include "ava/core/json.h"
#include "ava/core/strict_json.h"
#include "ava/core/utc_timestamp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <istream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ava::session {
namespace {

int hex_value(char ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

void append_utf8(std::string& out, int codepoint)
{
  if (codepoint <= 0x7F)
  {
    out.push_back(static_cast<char>(codepoint));
  }
  else if (codepoint <= 0x7FF)
  {
    out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else
  {
    out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

void append_utf8_codepoint(std::string& out, int codepoint)
{
  if (codepoint <= 0xFFFF)
  {
    append_utf8(out, codepoint);
  }
  else
  {
    out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

bool is_json_value_delimiter(char ch)
{
  return ch == ',' || ch == '}' || std::isspace(static_cast<unsigned char>(ch)) != 0;
}

bool requires_session_v4(EntryType type) noexcept
{
  return type == EntryType::AssistantOutputItem || type == EntryType::AssistantTurnCommit;
}

ava::core::VoidResult validate_entry_type_version(EntryType type, long long version)
{
  if (version < 0 || version > kCurrentSessionEntryVersion)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "unsupported session entry version");
    error.with_context("entry_type", std::string(to_string(type)));
    error.with_context("version", std::to_string(version));
    error.with_context("supported_version", std::to_string(kCurrentSessionEntryVersion));
    return std::unexpected(std::move(error));
  }
  if (!requires_session_v4(type) || version >= 4)
    return {};

  auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry type requires version 4");
  error.with_context("entry_type", std::string(to_string(type)));
  error.with_context("version", std::to_string(version));
  return std::unexpected(std::move(error));
}

ava::core::Result<std::optional<long long>> extract_entry_version(std::string_view line)
{
  auto const start = ava::core::json::field_value_start(line, "version");
  if (!start)
    return std::optional<long long>{};
  if (*start >= line.size())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "invalid session entry version");
    error.with_context("reason", "missing version value");
    return std::unexpected(std::move(error));
  }

  std::size_t index = *start;
  if (line[index] == '-')
    ++index;
  auto const digits_start = index;
  while (index < line.size() && std::isdigit(static_cast<unsigned char>(line[index])) != 0) ++index;
  if (index == digits_start || (index - digits_start > 1 && line[digits_start] == '0') || (index < line.size() && !is_json_value_delimiter(line[index])))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "invalid session entry version");
    error.with_context("reason", "version must be an integer");
    return std::unexpected(std::move(error));
  }

  try
  {
    return std::stoll(std::string(line.substr(*start, index - *start)));
  }
  catch (...)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "invalid session entry version");
    error.with_context("reason", "version is outside supported integer range");
    return std::unexpected(std::move(error));
  }
}

ava::core::Result<long long> read_supported_entry_version(std::string_view line, std::filesystem::path const& path)
{
  auto version = extract_entry_version(line);
  if (!version)
  {
    auto error = std::move(version.error());
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  if (!*version)
    return 0;
  if (**version >= 1 && **version <= kCurrentSessionEntryVersion)
    return **version;

  auto error = ava::core::Error(ava::core::ErrorCategory::Session, "unsupported session entry version");
  error.with_context("path", path.string());
  error.with_context("version", std::to_string(**version));
  error.with_context("supported_version", std::to_string(kCurrentSessionEntryVersion));
  return std::unexpected(std::move(error));
}

std::string extract_json_string(std::string_view line, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(line, key);
  if (!start || *start >= line.size() || line[*start] != '"')
  {
    return {};
  }
  auto const value_start = *start + 1;
  std::string result;
  bool escaped = false;
  bool closed = false;
  for (auto index = value_start; index < line.size(); ++index)
  {
    char const ch = line[index];
    if (escaped)
    {
      switch (ch)
      {
        case '"':
          result.push_back('"');
          break;
        case '\\':
          result.push_back('\\');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'u':
          if (index + 4 < line.size())
          {
            int const a = hex_value(line[index + 1]);
            int const b = hex_value(line[index + 2]);
            int const c = hex_value(line[index + 3]);
            int const d = hex_value(line[index + 4]);
            if (a >= 0 && b >= 0 && c >= 0 && d >= 0)
            {
              int const codepoint = (a << 12) | (b << 8) | (c << 4) | d;
              if (codepoint >= 0xD800 && codepoint <= 0xDBFF && index + 10 < line.size() && line[index + 5] == '\\' && line[index + 6] == 'u')
              {
                int const e = hex_value(line[index + 7]);
                int const f = hex_value(line[index + 8]);
                int const g = hex_value(line[index + 9]);
                int const h = hex_value(line[index + 10]);
                if (e >= 0 && f >= 0 && g >= 0 && h >= 0)
                {
                  int const low = (e << 12) | (f << 8) | (g << 4) | h;
                  if (low >= 0xDC00 && low <= 0xDFFF)
                  {
                    append_utf8_codepoint(result, ((codepoint - 0xD800) << 10) + (low - 0xDC00) + 0x10000);
                    index += 10;
                  }
                  else
                  {
                    append_utf8(result, 0xFFFD);
                    index += 4;
                  }
                }
                else
                {
                  append_utf8(result, 0xFFFD);
                  index += 4;
                }
              }
              else if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
              {
                append_utf8(result, 0xFFFD);
                index += 4;
              }
              else
              {
                append_utf8(result, codepoint);
                index += 4;
              }
            }
            else
            {
              return {};
            }
          }
          else
          {
            return {};
          }
          break;
        case '/':
          result.push_back('/');
          break;
        default:
          return {};
      }
      escaped = false;
      continue;
    }
    if (ch == '\\')
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
    {
      closed = true;
      break;
    }
    result.push_back(ch);
  }
  if (escaped || !closed)
  {
    return {};
  }
  return result;
}

std::string extract_json_object(std::string_view line, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(line, key);
  if (!start)
  {
    return "{}";
  }
  auto const value_start = *start;
  if (value_start >= line.size() || line[value_start] != '{')
  {
    return "{}";
  }

  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (auto index = value_start; index < line.size(); ++index)
  {
    char const ch = line[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\')
    {
      escaped = in_string;
      continue;
    }
    if (ch == '"')
    {
      in_string = !in_string;
      continue;
    }
    if (in_string)
    {
      continue;
    }
    if (ch == '{')
    {
      ++depth;
    }
    else if (ch == '}')
    {
      --depth;
      if (depth == 0)
      {
        return std::string(line.substr(value_start, index - value_start + 1));
      }
    }
  }
  return "{}";
}

}  // namespace

ava::core::Result<bool> read_limited_session_line(std::istream& file, std::string& line)
{
  line.clear();
  bool saw_character = false;
  char ch = '\0';
  while (file.get(ch))
  {
    saw_character = true;
    if (ch == '\n')
    {
      if (!line.empty() && line.back() == '\r')
      {
        line.pop_back();
      }
      return true;
    }
    if (line.size() + 1 >= kMaxSessionLineBytes)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry line is too large");
      error.with_context("max_bytes", std::to_string(kMaxSessionLineBytes));
      return std::unexpected(std::move(error));
    }
    line.push_back(ch);
  }
  if (!file.eof() && file.fail())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed while reading session file");
    return std::unexpected(std::move(error));
  }
  if (saw_character && !line.empty() && line.back() == '\r')
  {
    line.pop_back();
  }
  return saw_character;
}

ava::core::VoidResult validate_session_id(std::string_view session_id)
{
  if (session_id.empty() || session_id == "." || session_id == "..")
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid session id");
    error.with_context("reason", "empty or reserved path segment");
    return std::unexpected(std::move(error));
  }

  for (char const ch : session_id)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (ch == '/' || ch == '\\' || byte < 0x20 || byte == 0x7F)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid session id");
      error.with_context("reason", "contains a path separator or control character");
      return std::unexpected(std::move(error));
    }
  }

  return {};
}

ava::core::VoidResult validate_parent_id(std::string_view parent_id, std::string_view entry_id)
{
  if (parent_id.empty())
    return {};
  if (parent_id == "." || parent_id == "..")
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid session parent_id");
    error.with_context("entry_id", std::string(entry_id));
    error.with_context("reason", "reserved path segment");
    return std::unexpected(std::move(error));
  }

  for (char const ch : parent_id)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (ch == '/' || ch == '\\' || byte < 0x20 || byte == 0x7F)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid session parent_id");
      error.with_context("entry_id", std::string(entry_id));
      error.with_context("reason", "contains a path separator or control character");
      return std::unexpected(std::move(error));
    }
  }

  return {};
}

bool is_unsupported_session_version_error(ava::core::Error const& error)
{
  return error.category() == ava::core::ErrorCategory::Session && error.message() == "unsupported session entry version";
}

ava::core::Result<std::string> serialize_session_entry_line(SessionEntry const& entry)
{
  if (auto valid_type_version = validate_entry_type_version(entry.type, entry.version); !valid_type_version)
    return std::unexpected(std::move(valid_type_version.error()));

  // Version 0 is represented canonically by an absent member. Older session
  // readers reject an explicit `version: 0`, so preserve the wire form when
  // copying, branching, or exporting a parsed legacy entry.
  std::string line = "{";
  if (entry.version != 0)
    line += "\"version\":" + std::to_string(entry.version) + ",";
  line += "\"id\":\"" + json_escape(entry.id) + "\",";
  line += "\"parent_id\":\"" + json_escape(entry.parent_id) + "\",";
  line += "\"type\":\"" + std::string(to_string(entry.type)) + "\",";
  line += "\"timestamp\":\"" + json_escape(entry.timestamp) + "\",";
  line += "\"data\":" + entry.data_json + "}";
  if (line.size() >= kMaxSessionLineBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry line is too large");
    error.with_context("max_bytes", std::to_string(kMaxSessionLineBytes));
    error.with_context("entry_type", std::string(to_string(entry.type)));
    return std::unexpected(std::move(error));
  }
  return line;
}

ava::core::Result<SessionEntry> parse_session_entry_line(std::string_view line, std::filesystem::path const& path)
{
  // Direct import/compatibility callers must not bypass the strict snapshot
  // reader's UTF-8 gate before field extraction.
  if (!ava::core::json::is_valid_utf8(line))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry is not valid UTF-8");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto version = read_supported_entry_version(line, path);
  if (!version)
  {
    return std::unexpected(std::move(version.error()));
  }
  auto const id = extract_json_string(line, "id");
  auto const type_text = extract_json_string(line, "type");
  auto const timestamp = extract_json_string(line, "timestamp");
  if (id.empty() || type_text.empty() || timestamp.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "malformed session entry");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto const data_start = ava::core::json::field_value_start(line, "data");
  if (!data_start || *data_start >= line.size() || line[*data_start] != '{')
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry data must be a JSON object");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto type = parse_entry_type(type_text);
  if (!type)
  {
    return std::unexpected(type.error());
  }
  if (auto valid_type_version = validate_entry_type_version(*type, *version); !valid_type_version)
  {
    auto error = std::move(valid_type_version.error());
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto const parent_id = extract_json_string(line, "parent_id");
  if (auto valid_parent_id = validate_parent_id(parent_id, id); !valid_parent_id)
  {
    auto error = std::move(valid_parent_id.error());
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  return SessionEntry{
      .id = id,
      .parent_id = parent_id,
      .type = *type,
      .timestamp = timestamp,
      .data_json = extract_json_object(line, "data"),
      .version = *version,
  };
}

std::string json_escape(std::string_view value)
{
  std::string result;
  result.reserve(value.size());
  for (char const ch : value)
  {
    switch (ch)
    {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      case '\b':
        result += "\\b";
        break;
      case '\f':
        result += "\\f";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20)
        {
          std::ostringstream escaped;
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(ch));
          result += escaped.str();
        }
        else
        {
          result.push_back(ch);
        }
        break;
    }
  }
  return result;
}

std::string to_string(EntryType type)
{
  switch (type)
  {
    case EntryType::SessionStart:
      return "session_start";
    case EntryType::SessionMetadata:
      return "session_metadata";
    case EntryType::UserMessage:
      return "user_message";
    case EntryType::AssistantMessage:
      return "assistant_message";
    case EntryType::ToolCall:
      return "tool_call";
    case EntryType::ToolResult:
      return "tool_result";
    case EntryType::PermissionDecision:
      return "permission_decision";
    case EntryType::ModeChange:
      return "mode_change";
    case EntryType::ModelChange:
      return "model_change";
    case EntryType::ReasoningBlock:
      return "reasoning_block";
    case EntryType::ReasoningChange:
      return "reasoning_change";
    case EntryType::AssistantOutputItem:
      return "assistant_output_item";
    case EntryType::AssistantTurnCommit:
      return "assistant_turn_commit";
    case EntryType::Compaction:
      return "compaction";
    case EntryType::BranchSummary:
      return "branch_summary";
    case EntryType::Error:
      return "error";
    case EntryType::Cancel:
      return "cancel";
  }
  return "error";
}

ava::core::Result<EntryType> parse_entry_type(std::string_view value)
{
  if (value == "session_start")
    return EntryType::SessionStart;
  if (value == "session_metadata")
    return EntryType::SessionMetadata;
  if (value == "user_message")
    return EntryType::UserMessage;
  if (value == "assistant_message")
    return EntryType::AssistantMessage;
  if (value == "tool_call")
    return EntryType::ToolCall;
  if (value == "tool_result")
    return EntryType::ToolResult;
  if (value == "permission_decision")
    return EntryType::PermissionDecision;
  if (value == "mode_change")
    return EntryType::ModeChange;
  if (value == "model_change")
    return EntryType::ModelChange;
  if (value == "reasoning_block")
    return EntryType::ReasoningBlock;
  if (value == "reasoning_change")
    return EntryType::ReasoningChange;
  if (value == "assistant_output_item")
    return EntryType::AssistantOutputItem;
  if (value == "assistant_turn_commit")
    return EntryType::AssistantTurnCommit;
  if (value == "compaction")
    return EntryType::Compaction;
  if (value == "branch_summary")
    return EntryType::BranchSummary;
  if (value == "error")
    return EntryType::Error;
  if (value == "cancel")
    return EntryType::Cancel;

  auto error = ava::core::Error(ava::core::ErrorCategory::Session, "unknown session entry type");
  error.with_context("type", std::string(value));
  return std::unexpected(std::move(error));
}

ava::core::Result<std::optional<SyntheticDeliveryProvenance>> parse_synthetic_delivery_provenance(SessionEntry const& entry)
{
  if (entry.type != EntryType::UserMessage)
    return std::optional<SyntheticDeliveryProvenance>{};
  if (ava::core::validate_strict_json(entry.data_json, ava::core::json::kMaxNestingDepth) != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "user message provenance requires strict JSON"));
  auto const provenance_start = ava::core::json::field_value_start(entry.data_json, "provenance");
  if (!provenance_start)
    return std::optional<SyntheticDeliveryProvenance>{};
  if (*provenance_start >= entry.data_json.size() || entry.data_json[*provenance_start] != '{')
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "synthetic delivery provenance must be an object"));
  std::size_t end = *provenance_start;
  std::size_t depth = 0;
  std::size_t member_count = 0;
  bool in_string = false;
  bool escaped = false;
  for (; end < entry.data_json.size(); ++end)
  {
    char const ch = entry.data_json[end];
    if (in_string)
    {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        in_string = false;
      continue;
    }
    if (ch == '"')
      in_string = true;
    else if (ch == '{')
      ++depth;
    else if (ch == '}')
    {
      if (--depth == 0)
        break;
    }
    else if (ch == ':' && depth == 1)
      ++member_count;
  }
  if (end >= entry.data_json.size() || member_count != 3)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "synthetic delivery provenance has missing or unknown fields"));
  auto const provenance = std::string_view(entry.data_json).substr(*provenance_start, end - *provenance_start + 1);
  auto source = ava::core::json::string_field(provenance, "source");
  auto delivery_id = ava::core::json::string_field(provenance, "delivery_id");
  auto prompt_fingerprint = ava::core::json::string_field(provenance, "prompt_fingerprint");
  if (!source || !delivery_id || !prompt_fingerprint)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "synthetic delivery provenance fields must be strings"));
  SyntheticDeliveryProvenance parsed{
      .delivery_id = std::move(*delivery_id), .prompt_fingerprint = std::move(*prompt_fingerprint), .source = std::move(*source)};
  auto valid_id = [](std::string_view value) {
    return !value.empty() && value.size() <= kMaxSyntheticDeliveryProvenanceIdBytes && std::ranges::all_of(value, [](char ch) {
      auto const byte = static_cast<unsigned char>(ch);
      return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == ':';
    });
  };
  if (parsed.source != kSyntheticSubagentDeliverySource || !valid_id(parsed.delivery_id) || !valid_id(parsed.prompt_fingerprint))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "synthetic delivery provenance is unsupported or exceeds its bounds"));
  return std::optional<SyntheticDeliveryProvenance>(std::move(parsed));
}

bool is_internal_replay_user_message(SessionEntry const& entry)
{
  if (entry.type != EntryType::UserMessage)
    return false;
  auto const start = ava::core::json::field_value_start(entry.data_json, "internal_replay");
  return start && entry.data_json.substr(*start, 4) == "true";
}

std::string now_timestamp()
{
  auto const now = std::chrono::system_clock::now();
  auto const time = std::chrono::system_clock::to_time_t(now);
  return ava::core::format_utc_timestamp(time);
}

}  // namespace ava::session
