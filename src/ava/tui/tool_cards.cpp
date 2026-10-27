#include "sys.h"
#include "ava/agent/todo.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/tool_card_task_job.h"
#include "ava/tui/tool_cards.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>

namespace ava::tui {

using TodoStatus = agent::TodoStatus;

namespace {

constexpr auto kBlockMinWidth = std::size_t{32};
constexpr auto kRichShellPreviewLines = std::size_t{5};
constexpr auto kRichFilePreviewLines = std::size_t{10};
constexpr auto kRichSearchPreviewLines = std::size_t{15};
constexpr auto kRichListingPreviewLines = std::size_t{20};
constexpr auto kRichGenericPreviewLines = std::size_t{12};
constexpr auto kExpandedOutputPreviewLines = std::size_t{200};
constexpr auto kExpandedDiffPreviewLines = std::size_t{200};
constexpr auto kExactOutputPreviewLineCountMaxBytes = std::size_t{64 * 1024};

bool wide_blocks(std::size_t width)
{
  return width >= kBlockMinWidth;
}

std::string lower_ascii(std::string_view text)
{
  std::string lowered;
  lowered.reserve(text.size());
  for (char const ch : text) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return lowered;
}

bool bool_json_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return false;
  auto value = object.substr(*start);
  while (!value.empty())
  {
    auto const byte = static_cast<unsigned char>(value.front());
    if (byte != ' ' && byte != '\n' && byte != '\r' && byte != '\t')
      break;
    value.remove_prefix(1);
  }
  return value.starts_with("true");
}

std::optional<std::string> first_json_string(std::string_view object, std::initializer_list<std::string_view> keys)
{
  for (auto const key : keys)
  {
    if (auto value = ava::core::json::string_field(object, key); value && !value->empty())
      return value;
  }
  return std::nullopt;
}

std::optional<long long> first_json_integer(std::string_view object, std::initializer_list<std::string_view> keys)
{
  for (auto const key : keys)
  {
    if (auto value = ava::core::json::integer_field(object, key))
      return value;
  }
  return std::nullopt;
}

std::optional<std::size_t> csi_sequence_end(std::string_view text, std::size_t index)
{
  for (auto scan = index; scan < text.size(); ++scan)
  {
    auto const byte = static_cast<unsigned char>(text[scan]);
    if (byte >= 0x40 && byte <= 0x7E)
      return scan + 1;
  }
  return std::nullopt;
}

std::optional<std::size_t> terminal_control_sequence_end(std::string_view text, std::size_t index)
{
  auto const byte = static_cast<unsigned char>(text[index]);
  if (byte != 0x1B || index + 1 >= text.size())
    return std::nullopt;

  auto const introducer = text[index + 1];
  if (introducer == '[')
    return csi_sequence_end(text, index + 2);
  if (introducer != ']')
    return std::nullopt;

  for (auto scan = index + 2; scan < text.size(); ++scan)
  {
    auto const current = static_cast<unsigned char>(text[scan]);
    if (current == '\a')
      return scan + 1;
    if (current == 0x1B && scan + 1 < text.size() && text[scan + 1] == '\\')
      return scan + 2;
  }
  return std::nullopt;
}

std::string sanitize_copy_text(std::string_view text)
{
  std::string stripped;
  stripped.reserve(text.size());
  for (std::size_t index = 0; index < text.size();)
  {
    if (auto end = terminal_control_sequence_end(text, index))
    {
      index = *end;
      continue;
    }
    stripped.push_back(text[index]);
    ++index;
  }
  return sanitize_terminal_text(stripped);
}

bool ascii_whitespace(char ch)
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

std::string sanitized_payload_identity(std::string_view text)
{
  std::string stripped;
  stripped.reserve(text.size());
  for (std::size_t index = 0; index < text.size();)
  {
    if (auto end = terminal_control_sequence_end(text, index))
    {
      index = *end;
      continue;
    }
    stripped.push_back(text[index]);
    ++index;
  }
  while (!stripped.empty() && ascii_whitespace(stripped.back())) stripped.pop_back();

  std::string sanitized;
  sanitized.reserve(stripped.size());
  std::size_t start = 0;
  for (std::size_t index = 0; index < stripped.size(); ++index)
  {
    if (!ascii_whitespace(stripped[index]) || stripped[index] == ' ')
      continue;
    sanitized += sanitize_terminal_text(std::string_view(stripped).substr(start, index - start));
    sanitized.push_back(stripped[index]);
    start = index + 1;
  }
  sanitized += sanitize_terminal_text(std::string_view(stripped).substr(start));
  return sanitized;
}

bool same_payload(std::string_view lhs, std::string_view rhs)
{
  auto const left = sanitized_payload_identity(lhs);
  return !left.empty() && left == sanitized_payload_identity(rhs);
}

std::string format_duration_ms(long long milliseconds)
{
  if (milliseconds < 0)
    return {};
  if (milliseconds < 1000)
    return std::to_string(milliseconds) + "ms";
  if (milliseconds < 60'000)
  {
    auto const whole = milliseconds / 1000;
    auto const tenths = (milliseconds % 1000) / 100;
    return std::to_string(whole) + "." + std::to_string(tenths) + "s";
  }
  auto const minutes = milliseconds / 60'000;
  auto const seconds = (milliseconds % 60'000) / 1000;
  return std::to_string(minutes) + "m" + std::to_string(seconds) + "s";
}

std::optional<std::string> duration_text(ToolTimelineItem const& item)
{
  auto const value = first_json_integer(item.result_json, {"duration_ms", "elapsed_ms", "runtime_ms"});
  if (!value)
    return std::nullopt;
  auto formatted = format_duration_ms(*value);
  if (formatted.empty())
    return std::nullopt;
  return formatted;
}

std::optional<std::string> command_from_summary(std::string_view summary)
{
  constexpr std::string_view marker = "command=";
  auto const marker_offset = summary.find(marker);
  if (marker_offset == std::string_view::npos)
    return std::nullopt;
  auto value = summary.substr(marker_offset + marker.size());
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escaped = false;
  for (std::size_t index = 0; index + 1 < value.size(); ++index)
  {
    auto const ch = value[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\' && !in_single_quote)
    {
      escaped = true;
      continue;
    }
    if (ch == '\'' && !in_double_quote)
    {
      in_single_quote = !in_single_quote;
      continue;
    }
    if (ch == '"' && !in_single_quote)
    {
      in_double_quote = !in_double_quote;
      continue;
    }
    if (!in_single_quote && !in_double_quote && ch == ',' && value[index + 1] == ' ')
    {
      value = value.substr(0, index);
      break;
    }
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.remove_suffix(1);
  if (value.empty())
    return std::nullopt;
  return std::string(value);
}

std::optional<std::string> command_text(ToolTimelineItem const& item)
{
  if (auto command = first_json_string(item.arguments_json, {"command", "cmd", "shell_command"}))
    return command;
  if (auto command = first_json_string(item.result_json, {"command", "cmd", "shell_command"}))
    return command;
  if (auto command = command_from_summary(item.argument_summary))
    return command;
  auto const lowered = lower_ascii(item.name);
  if ((lowered == "bash" || lowered == "shell" || lowered == "run_command") && !item.argument_summary.empty())
  {
    return item.argument_summary;
  }
  return std::nullopt;
}

bool shell_tool(ToolTimelineItem const& item)
{
  auto const lowered_name = lower_ascii(item.name);
  if (lowered_name == "bash" || lowered_name == "shell" || lowered_name == "run_command" || lowered_name == "shell_run")
  {
    return true;
  }
  if (auto tool = first_json_string(item.arguments_json, {"tool", "type"}))
  {
    auto const lowered_tool = lower_ascii(*tool);
    if (lowered_tool == "bash" || lowered_tool == "shell")
      return true;
  }
  if (auto tool = first_json_string(item.result_json, {"tool", "type"}))
  {
    auto const lowered_tool = lower_ascii(*tool);
    if (lowered_tool == "bash" || lowered_tool == "shell")
      return true;
  }
  return command_text(item).has_value() && lowered_name.find("bash") != std::string::npos;
}

bool permission_audit_payload(ToolTimelineItem const& item, std::string_view text)
{
  if (item.permissions.empty())
    return false;

  bool has_permission_request_id = false;
  for (auto const& id : item.permission_request_ids)
  {
    has_permission_request_id = has_permission_request_id || (!id.empty() && text.find(id) != std::string_view::npos);
  }
  for (auto const& audit : item.permissions)
  {
    has_permission_request_id =
        has_permission_request_id || (!audit.permission_request_id.empty() && text.find(audit.permission_request_id) != std::string_view::npos);
  }
  if (!has_permission_request_id)
    return false;

  auto const lowered = lower_ascii(text);
  if (lowered.find("permission_denied") != std::string::npos)
    return true;

  return lowered.find("action:") != std::string::npos || lowered.find("request_id:") != std::string::npos || lowered.find("inspect:") != std::string::npos ||
         lowered.find("decision:") != std::string::npos || lowered.find("resolution:") != std::string::npos ||
         lowered.find("operation:") != std::string::npos || lowered.find("risk:") != std::string::npos || lowered.find("reason:") != std::string::npos ||
         lowered.find("target:") != std::string::npos || lowered.find("command:") != std::string::npos ||
         lowered.find("/permissions audit") != std::string::npos || lowered.find("/permissions diagnose") != std::string::npos;
}

std::optional<std::string> exit_status_text(ToolTimelineItem const& item)
{
  if (item.status == ToolTimelineStatus::Running)
    return std::string("running");
  if (bool_json_field(item.result_json, "canceled"))
    return std::string("canceled");
  if (bool_json_field(item.result_json, "timed_out"))
    return std::string("timed out");
  if (auto exit_code = first_json_integer(item.result_json, {"exit_code", "status_code"}))
  {
    return "exit " + std::to_string(*exit_code);
  }
  if (!item.result_summary.empty())
    return item.result_summary;
  return to_string(item.status);
}

bool directory_listing_tool(ToolTimelineItem const& item)
{
  auto const name = lower_ascii(item.name);
  return name == "list_directory";
}

std::string directory_listing_summary(ToolTimelineItem const& item)
{
  auto const entries = ava::core::json::objects_in_array_field(item.result_json, "entries");
  auto const total = first_json_integer(item.result_json, {"total_entries"}).value_or(static_cast<long long>(entries.size()));
  auto summary = std::to_string(std::max(0LL, total)) + " entries";
  if (bool_json_field(item.result_json, "truncated"))
    summary += " (truncated)";
  return summary;
}

std::optional<std::string> directory_listing_preview_text(ToolTimelineItem const& item)
{
  if (!directory_listing_tool(item))
    return std::nullopt;

  std::string entries_text;
  for (auto const& entry : ava::core::json::objects_in_array_field(item.result_json, "entries"))
  {
    auto const name = ava::core::json::string_field(entry, "name");
    if (!name || name->empty())
      continue;
    if (!entries_text.empty())
      entries_text += '\n';
    entries_text += *name;
    if (ava::core::json::string_field(entry, "type") == "directory")
      entries_text += '/';
  }
  if (entries_text.empty())
    return std::nullopt;
  return entries_text;
}

std::optional<std::string> output_preview_text(ToolTimelineItem const& item)
{
  if (auto entries = directory_listing_preview_text(item))
    return entries;
  if (auto output = first_json_string(item.result_json, {"output", "stdout", "stderr", "content", "preview"}))
  {
    return output;
  }
  if (item.result_summary.find('\n') != std::string::npos || item.result_summary.find('\r') != std::string::npos)
  {
    return item.result_summary;
  }
  return std::nullopt;
}

struct OutputPreviewLines
{
  std::vector<std::string_view> visible = {};
  bool has_more = false;
  std::optional<std::size_t> exact_total_lines = std::nullopt;
};

OutputPreviewLines output_preview_lines(std::string_view text, std::size_t max_visible, bool favor_tail)
{
  OutputPreviewLines preview;
  if (text.empty() || max_visible == 0)
    return preview;
  auto const count_exact = favor_tail || text.size() <= kExactOutputPreviewLineCountMaxBytes;
  std::deque<std::string_view> selected;
  std::size_t total_lines = 0;
  std::size_t start = 0;
  auto append_line = [&](std::size_t end) {
    ++total_lines;
    if (favor_tail)
    {
      if (selected.size() == max_visible)
        selected.pop_front();
      selected.push_back(text.substr(start, end - start));
      preview.has_more = total_lines > max_visible;
      return;
    }
    if (selected.size() < max_visible)
      selected.push_back(text.substr(start, end - start));
    else
      preview.has_more = true;
  };
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    if (text[index] != '\n' && text[index] != '\r')
      continue;

    append_line(index);
    if (preview.has_more && !count_exact)
      break;
    if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n')
      ++index;
    start = index + 1;
  }
  if (!preview.has_more || count_exact)
  {
    if (start < text.size())
      append_line(text.size());
    preview.exact_total_lines = total_lines;
  }
  preview.visible.assign(selected.begin(), selected.end());
  return preview;
}

struct BoundedWrappedPreview
{
  std::deque<std::string> rows = {};
  bool truncated = false;
};

BoundedWrappedPreview bounded_wrapped_preview(std::vector<std::string_view> const& logical_lines, std::size_t width, std::size_t max_rows, bool favor_tail)
{
  BoundedWrappedPreview preview;
  if (max_rows == 0)
    return preview;
  auto const row_byte_limit = std::max<std::size_t>(64, width * 4 + 16);
  bool retain_more = true;
  auto retain_row = [&](std::string row) {
    if (favor_tail)
    {
      if (preview.rows.size() == max_rows)
      {
        preview.rows.pop_front();
        preview.truncated = true;
      }
      preview.rows.push_back(std::move(row));
      return true;
    }
    if (preview.rows.size() == max_rows)
    {
      preview.truncated = true;
      return false;
    }
    preview.rows.push_back(std::move(row));
    return true;
  };

  for (auto const raw : logical_lines)
  {
    if (!retain_more)
    {
      preview.truncated = true;
      break;
    }
    std::string row;
    row.reserve(std::min(row_byte_limit, raw.size()));
    std::size_t columns = 0;
    auto append_unit = [&](std::string_view unit, std::size_t unit_columns) {
      if ((!row.empty() && columns + unit_columns > width) || (!row.empty() && row.size() + unit.size() > row_byte_limit))
      {
        if (!retain_row(std::move(row)))
          return false;
        row.clear();
        row.reserve(std::min(row_byte_limit, raw.size()));
        columns = 0;
      }
      row.append(unit);
      columns += unit_columns;
      return true;
    };

    for (std::size_t index = 0; index < raw.size();)
    {
      auto const byte = static_cast<unsigned char>(raw[index]);
      if (byte < 0x20 || byte == 0x7F)
      {
        if (byte == '\t')
        {
          if (!append_unit(" ", 1) || !append_unit(" ", 1))
          {
            retain_more = false;
            break;
          }
        }
        else if (!append_unit("?", 1))
        {
          retain_more = false;
          break;
        }
        ++index;
        continue;
      }

      auto const length = detail::utf8_sequence_length(byte);
      char32_t codepoint = 0;
      if (!detail::decode_utf8_codepoint(raw, index, length, codepoint))
      {
        if (!append_unit("?", 1))
        {
          retain_more = false;
          break;
        }
        ++index;
        continue;
      }
      if (codepoint >= 0x80 && codepoint <= 0x9F)
      {
        if (!append_unit("?", 1))
        {
          retain_more = false;
          break;
        }
      }
      else if (!append_unit(raw.substr(index, length), detail::codepoint_columns(codepoint)))
      {
        retain_more = false;
        break;
      }
      index += length;
    }
    if (retain_more && !retain_row(std::move(row)))
      retain_more = false;
  }
  return preview;
}

std::optional<std::size_t> exact_total_output_lines(ToolTimelineItem const& item, OutputPreviewLines const& preview)
{
  auto const local_total = preview.exact_total_lines;
  if (item.total_lines)
    return std::max(*item.total_lines, local_total.value_or(preview.visible.size()));
  if (!local_total)
    return std::nullopt;
  return *local_total + item.omitted_lines.value_or(0);
}

void append_output_preview_lines(std::vector<std::string>& lines, ToolTimelineItem const& item, std::string_view label, std::string const& text,
                                 std::size_t width, std::size_t max_visible, bool favor_tail, bool expansion_hint)
{
  if (text.empty() || max_visible == 0)
    return;
  auto preview = output_preview_lines(text, max_visible, favor_tail);
  if (preview.visible.size() == 1 && preview.visible.front().empty())
    return;

  auto const prefix = wide_blocks(width) ? std::string("  │     ") : std::string("      ");
  auto const content_prefix = wide_blocks(width) ? std::string("  │       ") : std::string("        ");
  auto const content_width = width > detail::terminal_text_columns(content_prefix) ? width - detail::terminal_text_columns(content_prefix) : std::size_t{1};
  auto const preview_width = std::max<std::size_t>(1, content_width > 4 ? content_width - 4 : content_width);
  auto rendered = bounded_wrapped_preview(preview.visible, preview_width, max_visible, favor_tail);

  auto const exact_total = exact_total_output_lines(item, preview);
  std::optional<std::size_t> exact_hidden;
  if (exact_total && !rendered.truncated && *exact_total >= preview.visible.size())
    exact_hidden = *exact_total - preview.visible.size();
  auto const has_hidden =
      rendered.truncated || preview.has_more || item.omitted_lines.value_or(0) > 0 || (exact_total && *exact_total > preview.visible.size());

  std::string header = prefix + std::string(detail::kSgrDim) + std::string(label) + ":" + std::string(detail::kSgrReset);
  lines.push_back(detail::fit_line_preserving_sgr(std::move(header), width));
  for (auto const& row : rendered.rows)
  {
    lines.push_back(detail::fit_line_preserving_sgr(content_prefix + std::string(detail::kSgrMuted) + row + std::string(detail::kSgrReset), width));
  }
  if (has_hidden)
  {
    auto hint = prefix + std::string(detail::kSgrDim) + "… ";
    if (exact_hidden && *exact_hidden > 0)
      hint += std::to_string(*exact_hidden) + " " + (*exact_hidden == 1 ? "line" : "lines") + " hidden";
    else
      hint += "more output hidden";
    if (expansion_hint)
      hint += " · Ctrl+O or /details";
    hint += std::string(detail::kSgrReset);
    lines.push_back(detail::fit_line_preserving_sgr(std::move(hint), width));
  }
}

void append_tool_detail_lines(std::vector<std::string>& lines, std::string_view label, std::string const& text, std::size_t width)
{
  if (text.empty())
    return;
  auto const prefix = wide_blocks(width) ? std::string("  │     ") : std::string("      ");
  auto const label_prefix = prefix + std::string(detail::kSgrDim) + std::string(label) + ": " + std::string(detail::kSgrReset);
  for (auto const& raw_line : split_lines(text))
  {
    lines.push_back(detail::fit_line_preserving_sgr(label_prefix + sanitize_terminal_text(raw_line), width));
  }
}

void append_task_launch_lines(std::vector<std::string>& lines, std::string const& text, std::size_t width)
{
  if (text.empty())
    return;
  auto const prefix = wide_blocks(width) ? std::string("  │     ") : std::string("      ");
  auto const label = std::string("launch: ");
  auto const first_prefix = prefix + label;
  auto const continuation_prefix = prefix + std::string(label.size(), ' ');
  auto const first_columns = detail::terminal_text_columns(first_prefix);
  auto const body_width = width > first_columns ? width - first_columns : std::size_t{1};
  auto wrapped = detail::wrap_transcript_text(sanitize_terminal_text(text), body_width);
  if (wrapped.empty())
    return;
  for (std::size_t index = 0; index < wrapped.size(); ++index)
  {
    auto row = (index == 0 ? first_prefix : continuation_prefix) + wrapped[index];
    lines.push_back(detail::fit_line_preserving_sgr(std::string(detail::kSgrDim) + std::move(row) + std::string(detail::kSgrReset), width));
  }
}

std::vector<std::string> display_changed_paths(ToolTimelineItem const& item)
{
  auto paths = item.changed_paths;
  auto const append_path = [&paths](std::string path) {
    if (path.empty())
      return;
    if (std::ranges::find(paths, path) == paths.end())
      paths.push_back(std::move(path));
  };
  for (auto const& path : ava::core::json::strings_in_array_field(item.result_json, "changed_paths")) append_path(path);
  for (auto const& path : ava::core::json::strings_in_array_field(item.result_json, "changed_files")) append_path(path);
  for (auto const& edit : ava::core::json::objects_in_array_field(item.result_json, "edits"))
    append_path(ava::core::json::string_field(edit, "path").value_or(""));
  if (item.name == "write" || item.name == "write_file" || item.name == "edit_file" || item.name == "apply_patch")
  {
    if (auto path = first_json_string(item.result_json, {"path", "target_path"}))
      append_path(*path);
    if (auto path = first_json_string(item.arguments_json, {"path", "target_path"}))
      append_path(*path);
  }

  return paths;
}

bool proven_relative_path_alias(std::string_view absolute, std::string_view relative)
{
  return absolute.starts_with('/') && !relative.empty() && !relative.starts_with('/') && absolute.size() > relative.size() && absolute.ends_with(relative) &&
         absolute[absolute.size() - relative.size() - 1] == '/';
}

std::optional<std::string> mutation_argument_path(ToolTimelineItem const& item)
{
  auto const mutation = item.name == "write" || item.name == "write_file" || item.name == "edit_file" || item.name == "apply_patch";
  if (!mutation || item.argument_summary.empty())
    return std::nullopt;

  auto path = std::string_view(item.argument_summary);
  if (path.starts_with("path="))
  {
    path.remove_prefix(std::string_view("path=").size());
    if (auto const comma = path.find(','); comma != std::string_view::npos)
      path = path.substr(0, comma);
    while (!path.empty() && path.front() == ' ') path.remove_prefix(1);
    while (!path.empty() && path.back() == ' ') path.remove_suffix(1);
  }
  else if (item.name != "write")
  {
    return std::nullopt;
  }

  if (path.empty() || path.starts_with('/') || path.find_first_of("\r\n") != std::string_view::npos)
    return std::nullopt;
  return std::string(path);
}

std::string project_display_path_aliases(ToolTimelineItem const& item, std::string_view text)
{
  auto const paths = display_changed_paths(item);
  auto const argument_path = mutation_argument_path(item);
  if (!argument_path || paths.size() != 1 || !proven_relative_path_alias(paths.front(), *argument_path))
    return std::string(text);

  std::string projected(text);
  for (auto offset = projected.find(paths.front()); offset != std::string::npos; offset = projected.find(paths.front(), offset + argument_path->size()))
  {
    projected.replace(offset, paths.front().size(), *argument_path);
  }
  return projected;
}

std::vector<std::string> projected_display_changed_paths(ToolTimelineItem const& item)
{
  std::vector<std::string> projected;
  for (auto const& path : display_changed_paths(item))
  {
    auto alias = project_display_path_aliases(item, path);
    if (std::ranges::find(projected, alias) == projected.end())
      projected.push_back(std::move(alias));
  }
  return projected;
}

std::string changed_paths_summary(ToolTimelineItem const& item, bool project_aliases = false)
{
  auto const paths = project_aliases ? projected_display_changed_paths(item) : display_changed_paths(item);
  if (paths.empty())
    return {};

  std::string summary;
  for (std::size_t index = 0; index < paths.size() && index < 4; ++index)
  {
    if (index > 0)
      summary += ", ";
    summary += sanitize_terminal_text(paths[index]);
  }
  if (paths.size() > 4)
    summary += ", +" + std::to_string(paths.size() - 4) + " more";
  return summary;
}

void append_diff_lines(std::vector<std::string>& lines, ToolTimelineItem const& item, std::size_t width, std::size_t max_lines)
{
  if (item.diff.empty())
    return;
  auto const prefix = wide_blocks(width) ? std::string("  │     ") : std::string("      ");
  auto label = std::string("diff");
  auto const paths = projected_display_changed_paths(item);
  if (!paths.empty())
  {
    label += " ";
    for (std::size_t index = 0; index < paths.size() && index < 3; ++index)
    {
      if (index > 0)
        label += ", ";
      label += sanitize_terminal_text(paths[index]);
    }
    if (paths.size() > 3)
      label += ", +" + std::to_string(paths.size() - 3) + " more";
  }
  lines.push_back(detail::fit_line_preserving_sgr(prefix + std::string(detail::kSgrDim) + std::move(label) + ":" + std::string(detail::kSgrReset), width));
  auto const content_prefix = wide_blocks(width) ? std::string("  │       ") : std::string("        ");
  auto const diff = project_display_path_aliases(item, item.diff);
  auto diff_lines = detail::render_unified_diff_body(diff, item.diff_truncated, width, content_prefix, max_lines);
  lines.insert(lines.end(), diff_lines.begin(), diff_lines.end());
}

std::string truncation_summary(ToolTimelineItem const& item)
{
  if (!item.truncated)
    return {};

  std::string summary;
  if (item.truncated)
  {
    if (item.start_line && item.end_line && item.total_lines)
    {
      summary = "truncated lines " + std::to_string(*item.start_line) + "-" + std::to_string(*item.end_line) + "/" + std::to_string(*item.total_lines);
    }
    else if (item.output_lines && item.total_lines)
    {
      summary = "truncated " + std::to_string(*item.output_lines) + "/" + std::to_string(*item.total_lines) + " lines";
    }
    else if (item.visible_matches && item.total_matches)
    {
      summary = "truncated " + std::to_string(*item.visible_matches) + "/" + std::to_string(*item.total_matches) + " matches";
    }
    else if (item.output_bytes && item.total_bytes)
    {
      summary = "truncated " + std::to_string(*item.output_bytes) + "/" + std::to_string(*item.total_bytes) + " bytes";
    }
    else
    {
      summary = "truncated output";
    }
    if (item.next_offset_line)
      summary += "; next offset " + std::to_string(*item.next_offset_line);
    std::vector<std::string> omitted;
    if (item.omitted_bytes)
      omitted.push_back(std::to_string(*item.omitted_bytes) + " bytes");
    if (item.omitted_lines)
      omitted.push_back(std::to_string(*item.omitted_lines) + " lines");
    if (!omitted.empty())
    {
      summary += "; omitted ";
      for (std::size_t index = 0; index < omitted.size(); ++index)
      {
        if (index > 0)
          summary += ", ";
        summary += omitted[index];
      }
    }
  }
  return summary;
}

std::string byte_retention_summary(ToolTimelineItem const& item)
{
  if (!item.output_bytes || !item.total_bytes)
    return {};
  return std::to_string(*item.output_bytes) + "/" + std::to_string(*item.total_bytes) + " bytes retained";
}

std::string permission_decision_label(ToolPermissionAuditItem const& audit)
{
  auto const decision = lower_ascii(audit.decision);
  if (decision == "allow" || decision == "allowed")
    return "allow";
  if (decision == "allow_session" || decision == "allow_session_grant" || decision == "session_grant")
    return "allow session";
  if (decision == "allow_remember")
    return "allow always";
  if (decision == "deny" || decision == "denied")
    return "deny";
  return "checked";
}

std::string permission_state_label(ToolTimelineItem const& item, ToolPermissionAuditItem const& audit)
{
  if (audit.decision.empty() && item.status == ToolTimelineStatus::Running)
    return "required";
  return permission_decision_label(audit);
}

bool permission_decision_denied(ToolPermissionAuditItem const& audit)
{
  return permission_decision_label(audit) == "deny";
}

std::string permission_audit_detail(ToolTimelineItem const& item, ToolPermissionAuditItem const& audit)
{
  std::vector<std::string> parts;
  parts.push_back(permission_state_label(item, audit));
  if (!audit.risk.empty())
    parts.push_back("risk " + audit.risk);
  if (!audit.permission_request_id.empty())
    parts.push_back("id " + audit.permission_request_id);
  if (!audit.resolver_request_id.empty())
    parts.push_back("resolver " + audit.resolver_request_id);
  if (!audit.reason.empty())
    parts.push_back("reason " + audit.reason);
  if (!audit.resolution_reason.empty())
    parts.push_back("resolution " + audit.resolution_reason);
  if (!audit.operation.empty())
    parts.push_back("operation " + audit.operation);
  if (!audit.tool_name.empty())
    parts.push_back("tool " + audit.tool_name);
  if (!audit.target.empty())
    parts.push_back("target " + audit.target);
  if (!audit.command.empty())
    parts.push_back("command " + audit.command);

  std::string detail;
  for (std::size_t index = 0; index < parts.size(); ++index)
  {
    if (index > 0)
      detail += " · ";
    detail += parts[index];
  }
  return detail;
}

std::string permission_audit_show_command(ToolPermissionAuditItem const& audit)
{
  if (audit.permission_request_id.empty())
    return {};
  return "/permissions audit show " + audit.permission_request_id;
}

std::string permission_diagnose_command(ToolPermissionAuditItem const& audit)
{
  if (audit.permission_request_id.empty() || !permission_decision_denied(audit))
    return {};
  return "/permissions diagnose " + audit.permission_request_id;
}

std::string denied_reason(ToolTimelineItem const& item)
{
  for (auto const& audit : item.permissions)
  {
    if (!permission_decision_denied(audit))
      continue;
    auto reason = sanitize_terminal_text(!audit.reason.empty() ? audit.reason : audit.resolution_reason);
    auto const lowered = lower_ascii(reason);
    for (auto const prefix : {std::string_view("permission denied: "), std::string_view("permission deny · reason "), std::string_view("reason ")})
    {
      if (lowered.starts_with(prefix))
      {
        reason.erase(0, prefix.size());
        break;
      }
    }
    if (!reason.empty())
      return reason;
  }
  return {};
}

std::string status_marker(ToolTimelineStatus status)
{
  switch (status)
  {
    case ToolTimelineStatus::Running:
      return "~";
    case ToolTimelineStatus::Success:
      return "+";
    case ToolTimelineStatus::Canceled:
      return "-";
    case ToolTimelineStatus::Error:
      return "x";
  }
  return "?";
}

std::string_view status_sgr(ToolTimelineStatus status)
{
  switch (status)
  {
    case ToolTimelineStatus::Running:
      return detail::kSgrWarning;
    case ToolTimelineStatus::Success:
      return detail::kSgrSuccess;
    case ToolTimelineStatus::Canceled:
      return detail::kSgrDim;
    case ToolTimelineStatus::Error:
      return detail::kSgrError;
  }
  return detail::kSgrDim;
}

bool name_is(ToolTimelineItem const& item, std::initializer_list<std::string_view> names)
{
  auto const lowered = lower_ascii(item.name);
  return std::ranges::any_of(names, [&](std::string_view name) { return lowered == name; });
}

std::string argument_value(ToolTimelineItem const& item, std::initializer_list<std::string_view> keys)
{
  if (auto value = first_json_string(item.arguments_json, keys))
    return sanitize_terminal_text(*value);
  return {};
}

std::string safe_argument_summary(ToolTimelineItem const& item)
{
  if (permission_audit_payload(item, item.argument_summary))
    return {};
  auto text = sanitize_terminal_text(item.argument_summary);
  if (directory_listing_tool(item) && text == "arguments provided")
    return {};
  for (auto const& id : item.permission_request_ids)
  {
    if (!id.empty() && text.find(id) != std::string::npos)
      return {};
  }
  return text;
}

std::string human_call_text(ToolTimelineItem const& item)
{
  if (shell_tool(item))
  {
    if (permission_audit_payload(item, item.argument_summary))
      return argument_value(item, {"command", "cmd", "shell_command"});
    return command_text(item).value_or(safe_argument_summary(item));
  }
  if (name_is(item, {"question"}))
    return argument_value(item, {"question"});

  auto const path = argument_value(item, {"path", "target_path", "directory", "root"});
  auto const pattern = argument_value(item, {"pattern", "query", "glob"});
  if (name_is(item, {"read", "read_file"}))
  {
    auto call = path.empty() ? safe_argument_summary(item) : path;
    auto const start = first_json_integer(item.arguments_json, {"offset", "start_line", "line"});
    auto const count = first_json_integer(item.arguments_json, {"limit", "line_count", "count"});
    if (start)
      call += " · from line " + std::to_string(*start);
    if (count)
      call += " · " + std::to_string(*count) + " lines";
    return call;
  }
  if (name_is(item, {"write", "write_file"}))
    return path.empty() ? safe_argument_summary(item) : path;
  if (name_is(item, {"edit", "edit_file", "apply_patch"}))
  {
    auto call = path.empty() ? safe_argument_summary(item) : path;
    auto const edits = ava::core::json::objects_in_array_field(item.arguments_json, "edits").size();
    if (edits > 0)
      call += " · " + std::to_string(edits) + (edits == 1 ? " edit" : " edits");
    return call;
  }
  if (name_is(item, {"grep", "search", "websearch"}))
  {
    auto call = pattern;
    if (!path.empty())
      call += (call.empty() ? std::string{} : " · in ") + path;
    return call.empty() ? safe_argument_summary(item) : call;
  }
  if (directory_listing_tool(item))
    return path.empty() ? std::string(".") : path;
  if (name_is(item, {"glob", "find"}))
  {
    auto call = pattern;
    if (!path.empty())
      call += (call.empty() ? std::string{} : " · in ") + path;
    return call.empty() ? safe_argument_summary(item) : call;
  }
  if (auto summary = safe_argument_summary(item); !summary.empty())
    return summary;
  auto safe = sanitize_terminal_text(std::string_view(item.arguments_json).substr(0, std::min(item.arguments_json.size(), std::size_t{512})));
  if (item.arguments_json.size() > 512)
    safe += "…";
  return safe;
}

void append_wrapped_call(std::vector<std::string>& lines, ToolTimelineItem const& item, std::string const& call, std::size_t width)
{
  if (call.empty())
    return;
  auto const prefix = wide_blocks(width) ? std::string("  │     ") : std::string("      ");
  auto const content_width = width > detail::terminal_text_columns(prefix) ? width - detail::terminal_text_columns(prefix) : std::size_t{1};
  auto wrapped = detail::wrap_transcript_text((shell_tool(item) ? "$ " : "") + sanitize_terminal_text(call), content_width);
  for (auto const& part : wrapped)
  {
    auto const style = shell_tool(item) ? std::string(detail::kSgrBold) + std::string(detail::kSgrAccent) : std::string(detail::kSgrText);
    lines.push_back(detail::fit_line_preserving_sgr(prefix + style + part + std::string(detail::kSgrReset), width));
  }
}

std::size_t rich_output_cap(ToolTimelineItem const& item)
{
  if (shell_tool(item))
    return kRichShellPreviewLines;
  if (name_is(item, {"read", "read_file", "write", "write_file", "edit", "edit_file", "apply_patch"}))
    return kRichFilePreviewLines;
  if (name_is(item, {"grep", "search", "websearch"}))
    return kRichSearchPreviewLines;
  if (name_is(item, {"glob", "find"}) || directory_listing_tool(item))
    return kRichListingPreviewLines;
  return kRichGenericPreviewLines;
}

std::optional<std::string> question_answer_text(ToolTimelineItem const& item)
{
  auto const answer = ava::core::json::object_field(item.result_json, "answer");
  if (!answer)
    return std::nullopt;
  auto selected = ava::core::json::strings_in_array_field(*answer, "selected_options");
  auto custom = ava::core::json::string_field(*answer, "custom_text").value_or("");
  std::string text;
  for (std::size_t index = 0; index < selected.size(); ++index)
  {
    if (index > 0)
      text += ", ";
    text += selected[index];
  }
  if (!custom.empty())
  {
    if (!text.empty())
      text += " · ";
    text += custom;
  }
  return text.empty() ? std::optional<std::string>{} : std::optional<std::string>{std::move(text)};
}

}  // namespace

std::string_view to_string(ToolPresentation presentation) noexcept
{
  switch (presentation)
  {
    case ToolPresentation::Compact:
      return "compact";
    case ToolPresentation::Rich:
      return "rich";
    case ToolPresentation::Expanded:
      return "expanded";
  }
  return "rich";
}

std::string to_string(ToolLifecycleState state)
{
  switch (state)
  {
    case ToolLifecycleState::ProviderAnnounced:
      return "announced";
    case ToolLifecycleState::ArgumentsStreaming:
      return "arguments streaming";
    case ToolLifecycleState::ArgumentsComplete:
      return "arguments complete";
    case ToolLifecycleState::ExecutionStarted:
      return "executing";
    case ToolLifecycleState::Progress:
      return "progress";
    case ToolLifecycleState::Complete:
      return "complete";
    case ToolLifecycleState::Canceled:
      return "canceled";
    case ToolLifecycleState::Error:
      return "error";
  }
  return "unknown";
}

namespace detail {

void append_copy_block(std::string& output, std::string_view label, std::string const& text)
{
  if (text.empty())
    return;
  auto const lines = split_lines(text);
  if (lines.empty())
    return;
  if (lines.size() == 1)
  {
    output += std::string(label) + ": " + sanitize_copy_text(lines.front()) + "\n";
    return;
  }
  output += std::string(label) + ":\n";
  for (auto const& line : lines) output += "  " + sanitize_copy_text(line) + "\n";
}

ToolPresentation tool_card_presentation(ToolTimelineItem const& item, ToolPresentation inherited)
{
  if (!item.details_visible)
    return inherited;
  if (*item.details_visible)
    return ToolPresentation::Expanded;
  return inherited == ToolPresentation::Compact ? ToolPresentation::Compact : ToolPresentation::Rich;
}

bool tool_card_details_visible(ToolTimelineItem const& item, ToolPresentation inherited)
{
  return tool_card_presentation(item, inherited) == ToolPresentation::Expanded;
}

bool tool_card_details_visible(ToolTimelineItem const& item, bool global_details_visible)
{
  return tool_card_details_visible(item, global_details_visible ? ToolPresentation::Expanded : ToolPresentation::Compact);
}

bool permission_matches_copy_query(ToolPermissionAuditItem const& permission, std::string_view query)
{
  if (query.empty())
    return true;
  auto const needle = lower_ascii(query);
  auto const matches = [&needle](std::string_view value) { return !value.empty() && lower_ascii(value).find(needle) != std::string::npos; };
  return matches(permission.permission_request_id) || matches(permission.resolver_request_id) || matches(permission.decision) ||
         matches(permission.operation) || matches(permission.tool_name) || matches(permission.risk) || matches(permission.reason) ||
         matches(permission.target) || matches(permission.command) || matches(permission.resolution_reason) ||
         matches(permission_audit_show_command(permission)) || matches(permission_diagnose_command(permission));
}

bool tool_card_matches_copy_query(ToolTimelineItem const& item, std::string_view query)
{
  if (task_job_card_is_quiet_poll(item))
    return false;
  if (query.empty())
    return true;
  if (is_task_or_job_tool(item))
    return task_job_card_matches_query(item, task_job_card_presentation(item), query);

  auto const needle = lower_ascii(query);
  auto const matches = [&needle](std::string_view value) { return !value.empty() && lower_ascii(value).find(needle) != std::string::npos; };

  if (matches(item.name) || matches(item.argument_summary) || matches(item.result_summary) || matches(item.arguments_json) || matches(item.result_json) ||
      matches(item.call_id) || matches(item.request_id) || matches(item.correlation_id) || matches(item.diff) || matches(item.spill_path))
    return true;
  for (auto const& path : display_changed_paths(item))
    if (matches(path))
      return true;
  for (auto const& id : item.permission_request_ids)
    if (matches(id))
      return true;
  for (auto const& permission : item.permissions)
  {
    if (permission_matches_copy_query(permission, query))
      return true;
  }
  return false;
}

std::string tool_card_diff_copy_text(ToolTimelineItem const& item)
{
  if (item.diff.empty())
    return {};
  std::string output;
  for (auto const& line : split_lines(item.diff))
  {
    if (!output.empty())
      output += '\n';
    output += sanitize_copy_text(line);
  }
  if (item.diff_truncated)
  {
    if (!output.empty())
      output += '\n';
    output += "[diff truncated]";
  }
  return output;
}

std::string tool_card_permission_copy_text(ToolTimelineItem const& item, std::string_view query)
{
  std::string output;
  for (auto const& audit : item.permissions)
  {
    if (!permission_matches_copy_query(audit, query))
      continue;
    append_copy_block(output, "permission", permission_audit_detail(item, audit));
    append_copy_block(output, "inspect", permission_audit_show_command(audit));
    append_copy_block(output, "diagnose", permission_diagnose_command(audit));
  }
  if (!output.empty() && output.back() == '\n')
    output.pop_back();
  return output;
}

std::string tool_card_copy_text(ToolTimelineItem const& item)
{
  if (is_task_or_job_tool(item))
  {
    auto const presentation = task_job_card_presentation(item);
    return task_job_card_copy_text(item, presentation);
  }

  std::string output;
  append_copy_block(output, "tool", item.name.empty() ? std::string("unknown") : item.name);
  append_copy_block(output, "status", ava::tui::to_string(item.status));
  auto const call = human_call_text(item);
  append_copy_block(output, shell_tool(item) ? "command" : "call", call);
  if (auto answer = question_answer_text(item))
    append_copy_block(output, "answer", *answer);
  auto const shell_status = shell_tool(item) ? exit_status_text(item) : std::optional<std::string>{};
  if (shell_status && !permission_audit_payload(item, *shell_status))
    append_copy_block(output, "shell status", *shell_status);
  if (!permission_audit_payload(item, item.result_summary) && (!shell_status || !same_payload(item.result_summary, *shell_status)))
    append_copy_block(output, "result", item.result_summary);
  if (auto preview = output_preview_text(item); preview && !permission_audit_payload(item, *preview) && !same_payload(*preview, item.result_summary))
    append_copy_block(output, "output", *preview);
  append_copy_block(output, "truncation", truncation_summary(item));
  append_copy_block(output, "changed", changed_paths_summary(item));
  append_copy_block(output, "full output", item.spill_path);
  if (item.spill_truncated)
    append_copy_block(output, "spill incomplete", "true");
  append_copy_block(output, "diff", item.diff);
  if (!output.empty() && output.back() == '\n')
    output.pop_back();
  return output;
}

namespace {

std::optional<std::size_t> absolute_path_start(std::string_view text)
{
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    if (text[index] != '/')
      continue;
    if (index == 0 || text[index - 1] == ' ' || text[index - 1] == '=' || text[index - 1] == '\'' || text[index - 1] == '"')
      return index;
  }
  return std::nullopt;
}

bool contains_absolute_path(std::string_view text)
{
  return absolute_path_start(text).has_value();
}

std::string resting_changed_paths_summary(ToolTimelineItem const& item)
{
  for (auto const& path : display_changed_paths(item))
  {
    auto const sanitized = sanitize_terminal_text(path);
    if (!sanitized.empty() && sanitized == path && !contains_absolute_path(sanitized) && sanitized.find_first_of("\r\n") == std::string::npos)
      return sanitized;
  }
  return {};
}

std::string resting_text(ToolTimelineItem const& item, std::string_view text)
{
  if (permission_audit_payload(item, text))
    return {};
  auto sanitized = sanitize_terminal_text(text);
  if (contains_absolute_path(sanitized))
    return {};
  for (auto const id : {std::string_view(item.call_id), std::string_view(item.request_id), std::string_view(item.correlation_id)})
  {
    if (!id.empty() && sanitized.find(id) != std::string::npos)
      return {};
  }
  for (auto const& id : item.permission_request_ids)
  {
    if (!id.empty() && sanitized.find(id) != std::string::npos)
      return {};
  }
  for (auto const& audit : item.permissions)
  {
    if ((!audit.permission_request_id.empty() && sanitized.find(audit.permission_request_id) != std::string::npos) ||
        (!audit.resolver_request_id.empty() && sanitized.find(audit.resolver_request_id) != std::string::npos))
      return {};
  }
  if (sanitized.find("/permissions audit") != std::string::npos || sanitized.find("/permissions diagnose") != std::string::npos)
    return {};
  return sanitized;
}

std::string resting_result_text(ToolTimelineItem const& item)
{
  if (permission_audit_payload(item, item.result_summary))
    return {};
  auto result = sanitize_terminal_text(item.result_summary);
  if (directory_listing_tool(item) && (result.empty() || result == "ok"))
    result = directory_listing_summary(item);
  if (auto const path_start = absolute_path_start(result))
  {
    result.resize(*path_start);
    while (!result.empty() && result.back() == ' ') result.pop_back();
    if (result.ends_with(" to"))
    {
      result.resize(result.size() - 3);
      while (!result.empty() && result.back() == ' ') result.pop_back();
    }
  }
  return resting_text(item, result);
}

std::optional<TodoStatus> parse_card_todo_status(std::string_view value)
{
  if (value == "pending")
    return TodoStatus::Pending;
  if (value == "in_progress")
    return TodoStatus::InProgress;
  if (value == "completed")
    return TodoStatus::Completed;
  return std::nullopt;
}

std::string todo_card_marker(TodoStatus status)
{
  switch (status)
  {
    case TodoStatus::Pending:
      return "○";
    case TodoStatus::InProgress:
      return "◉";
    case TodoStatus::Completed:
      return "✓";
  }
  return "?";
}

std::string_view todo_card_status_sgr(TodoStatus status)
{
  switch (status)
  {
    case TodoStatus::Pending:
      return detail::kSgrDim;
    case TodoStatus::InProgress:
      return detail::kSgrWarning;
    case TodoStatus::Completed:
      return detail::kSgrSuccess;
  }
  return detail::kSgrDim;
}

bool bool_json_true(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return false;
  auto value = object.substr(*start);
  while (!value.empty() && (value.front() == ' ' || value.front() == '\n' || value.front() == '\r' || value.front() == '\t')) value.remove_prefix(1);
  return value.starts_with("true");
}

std::optional<std::vector<agent::TodoItem>> parse_todowrite_card_todos(std::string_view result_json)
{
  if (!bool_json_true(result_json, "ok"))
    return std::nullopt;
  auto const tool = ava::core::json::string_field(result_json, "tool");
  if (!tool || *tool != "todowrite")
    return std::nullopt;
  auto const objects = ava::core::json::strict_objects_in_array_field(result_json, "todos", 50);
  if (!objects)
    return std::nullopt;
  std::vector<agent::TodoItem> todos;
  todos.reserve(objects->size());
  for (auto const& object : *objects)
  {
    auto const id = ava::core::json::string_field(object, "id");
    auto const content = ava::core::json::string_field(object, "content");
    auto const status_text = ava::core::json::string_field(object, "status");
    if (!id || !content || !status_text)
      return std::nullopt;
    auto const status = parse_card_todo_status(*status_text);
    if (!status)
      return std::nullopt;
    todos.push_back(agent::TodoItem{.id = *id, .content = *content, .status = *status});
  }
  return todos;
}

std::string todowrite_primary_summary(ToolTimelineItem const& item)
{
  if (item.status == ToolTimelineStatus::Error)
    return item.result_summary.empty() ? std::string("failed") : sanitize_terminal_text(item.result_summary);
  auto todos = parse_todowrite_card_todos(item.result_json);
  if (!todos)
    return item.result_summary.empty() ? std::string("todo list updated") : sanitize_terminal_text(item.result_summary);
  if (todos->empty())
    return "todos cleared";
  std::size_t completed = 0;
  std::size_t in_progress = 0;
  std::size_t pending = 0;
  for (auto const& todo : *todos)
  {
    switch (todo.status)
    {
      case TodoStatus::Completed:
        ++completed;
        break;
      case TodoStatus::InProgress:
        ++in_progress;
        break;
      case TodoStatus::Pending:
        ++pending;
        break;
    }
  }
  return std::to_string(completed) + "/" + std::to_string(todos->size()) + " completed · " + std::to_string(in_progress) + " in progress · " +
         std::to_string(pending) + " pending";
}

void append_todowrite_checklist(std::vector<std::string>& lines, ToolTimelineItem const& item, std::size_t width, ToolPresentation presentation)
{
  auto todos = parse_todowrite_card_todos(item.result_json);
  if (!todos)
    return;
  if (todos->empty())
  {
    auto const prefix = wide_blocks(width) ? std::string("  │     ") : std::string("      ");
    lines.push_back(detail::fit_line_preserving_sgr(prefix + std::string(detail::kSgrDim) + "todos cleared" + std::string(detail::kSgrReset), width));
    return;
  }
  auto const prefix = wide_blocks(width) ? std::string("  │     ") : std::string("      ");
  auto const content_width = width > detail::terminal_text_columns(prefix + "✓ #id ") ? width - detail::terminal_text_columns(prefix) : std::size_t{1};
  auto const max_items = presentation == ToolPresentation::Compact    ? std::size_t{0}
                         : presentation == ToolPresentation::Expanded ? todos->size()
                                                                      : std::min<std::size_t>(todos->size(), 8);
  if (max_items == 0)
    return;
  auto const visible = std::min(todos->size(), max_items);
  for (std::size_t index = 0; index < visible; ++index)
  {
    auto const& todo = (*todos)[index];
    auto line = std::string(todo_card_status_sgr(todo.status)) + todo_card_marker(todo.status) + std::string(detail::kSgrReset) + " #" +
                sanitize_terminal_text(todo.id) + " " + sanitize_terminal_text(todo.content);
    lines.push_back(detail::fit_line_preserving_sgr(prefix + detail::fit_line_preserving_sgr(std::move(line), content_width), width));
  }
  if (todos->size() > visible)
  {
    lines.push_back(detail::fit_line_preserving_sgr(
        prefix + std::string(detail::kSgrDim) + "+" + std::to_string(todos->size() - visible) + " more" + std::string(detail::kSgrReset), width));
  }
}

std::string tool_primary_summary(ToolTimelineItem const& item, bool suppress_result_summary, bool is_shell, std::optional<std::string> const& command,
                                 std::optional<std::string> const& shell_status, std::optional<std::string> const& duration, std::string const& truncation)
{
  if (name_is(item, {"todowrite"}))
  {
    auto text = todowrite_primary_summary(item);
    if (item.status != ToolTimelineStatus::Running && duration && !text.empty() && text.find(*duration) == std::string::npos)
      text += " · " + *duration;
    return text;
  }
  auto const with_duration = [&](std::string text) {
    if (item.status != ToolTimelineStatus::Running && duration && !text.empty() && text.find(*duration) == std::string::npos)
      text += " · " + *duration;
    return text;
  };
  auto const argument_or_command = [&]() {
    if (auto summary = safe_argument_summary(item); !summary.empty())
      return summary;
    if (command)
      return resting_text(item, *command);
    return std::string{};
  };

  auto target = item.status == ToolTimelineStatus::Running ? argument_or_command() : resting_changed_paths_summary(item);
  if (target.empty())
    target = argument_or_command();
  if (item.status == ToolTimelineStatus::Running)
  {
    auto const pending = item.lifecycle == ToolLifecycleState::ProviderAnnounced || item.lifecycle == ToolLifecycleState::ArgumentsStreaming ||
                         item.lifecycle == ToolLifecycleState::ArgumentsComplete;
    auto state = pending ? std::string("pending") : std::string("running");
    return target.empty() ? state : state + " · " + target;
  }
  if (is_shell && shell_status && (!suppress_result_summary || sanitize_terminal_text(*shell_status) != sanitize_terminal_text(item.result_summary)))
  {
    auto text = resting_text(item, *shell_status);
    if (!text.empty())
    {
      if (item.status == ToolTimelineStatus::Canceled && text.find("canceled") == std::string::npos)
        text = "canceled · " + text;
      return with_duration(std::move(text));
    }
  }
  auto const result = suppress_result_summary ? std::string{} : resting_result_text(item);
  auto const sanitized_target = sanitize_terminal_text(target);
  auto const sanitized_result = sanitize_terminal_text(result);
  auto const target_value_offset = sanitized_target.find('=');
  auto const target_value = target_value_offset == std::string::npos ? std::string_view{} : std::string_view(sanitized_target).substr(target_value_offset + 1);
  auto const result_contains_target =
      sanitized_result.find(sanitized_target) != std::string::npos || (!target_value.empty() && sanitized_result.find(target_value) != std::string::npos);
  if (!target.empty() && !result.empty() && sanitized_target != sanitized_result && !result_contains_target)
    return with_duration(target + " · " + result);
  if (!result.empty())
    return with_duration(result);
  if (!target.empty())
    return with_duration(target);
  if (!truncation.empty())
    return with_duration(truncation);
  if (item.status == ToolTimelineStatus::Canceled)
    return "canceled";
  if (item.status == ToolTimelineStatus::Error)
    return "failed";
  return {};
}

}  // namespace

ToolCardRenderResult render_tool_card_rows(ToolTimelineItem const& item, std::size_t width, ToolPresentation inherited, bool suppress_result_summary)
{
  if (task_job_card_is_quiet_poll(item))
    return {};

  auto const outer_width = width;
  auto const has_outer_margins = width >= 4;
  if (has_outer_margins)
    width = outer_width - 4;

  std::vector<std::string> lines;
  auto const presentation = tool_card_presentation(item, inherited);
  auto const marker = status_marker(item.status);
  auto const task_job = task_job_card_presentation(item);
  auto const is_task_job = task_job.kind != TaskJobToolKind::None;
  auto name_raw = sanitize_terminal_text(is_task_job ? task_job.display_name : (item.name.empty() ? "unknown" : item.name));
  auto const is_shell = shell_tool(item);
  auto const command = command_text(item);
  auto const shell_status = is_shell ? exit_status_text(item) : std::optional<std::string>{};
  auto const duration = duration_text(item);
  auto const truncation = truncation_summary(item);
  auto primary = is_task_job ? task_job.primary : tool_primary_summary(item, suppress_result_summary, is_shell, command, shell_status, duration, truncation);
  if (!is_task_job)
  {
    if (auto denied = denied_reason(item); !denied.empty())
      primary = std::move(denied);
  }

  auto const prefix_text = wide_blocks(width) ? std::string("  │ ") : std::string("  ");
  auto const complete_primary_raw = sanitize_terminal_text(primary);
  auto primary_raw = complete_primary_raw;
  auto const marker_prefix_columns = terminal_text_columns(prefix_text + marker + " ");
  auto const primary_separator_columns = primary_raw.empty() ? std::size_t{0} : terminal_text_columns(" · ");
  auto const full_columns = marker_prefix_columns + terminal_text_columns(name_raw) + primary_separator_columns + terminal_text_columns(primary_raw);
  if (full_columns > width)
  {
    auto const fixed_columns = marker_prefix_columns + terminal_text_columns(name_raw);
    if (!primary_raw.empty() && width > fixed_columns + primary_separator_columns)
      primary_raw = fit_line(std::move(primary_raw), width - fixed_columns - primary_separator_columns);
    else
      primary_raw.clear();
    if (fixed_columns > width && width > marker_prefix_columns)
      name_raw = fit_line(std::move(name_raw), width - marker_prefix_columns);
  }

  std::string header = prefix_text + std::string(status_sgr(item.status)) + marker + std::string(kSgrReset) + " " + std::string(kSgrBold) +
                       std::string(kSgrAccent) + name_raw + std::string(kSgrReset);
  if (!primary_raw.empty())
    header += " " + std::string(kSgrDim) + "· " + primary_raw + std::string(kSgrReset);
  lines.push_back(fit_line_preserving_sgr(std::move(header), width));
  auto const private_rows_first = lines.size();
  if (task_job.kind == TaskJobToolKind::Task)
    append_task_launch_lines(lines, task_job.launch_display, width);
  auto const private_rows_past_last = lines.size();

  if (presentation != ToolPresentation::Compact)
  {
    if (is_task_job)
    {
      // Task/job Rich cards stay single-line. Expanded may show bounded plain task_result only.
      if (presentation == ToolPresentation::Expanded && !task_job.expanded_detail.empty() && !suppress_result_summary)
        append_tool_detail_lines(lines, "result", task_job.expanded_detail, width);
    }
    else if (name_is(item, {"todowrite"}) && item.status == ToolTimelineStatus::Success)
    {
      append_todowrite_checklist(lines, item, width, presentation);
    }
    else
    {
      auto call = human_call_text(item);
      if (name_is(item, {"question"}))
        call = first_json_string(item.result_json, {"question"}).value_or(call);
      append_wrapped_call(lines, item, call, width);
      if (auto answer = question_answer_text(item))
      {
        auto const prefix = wide_blocks(width) ? std::string("  │     ") : std::string("      ");
        auto const content_width = width > terminal_text_columns(prefix + "answer: ") ? width - terminal_text_columns(prefix + "answer: ") : std::size_t{1};
        auto wrapped = wrap_transcript_text(sanitize_terminal_text(*answer), content_width);
        for (std::size_t index = 0; index < wrapped.size(); ++index)
        {
          auto label = index == 0 ? std::string("answer: ") : std::string("        ");
          lines.push_back(fit_line_preserving_sgr(prefix + std::string(kSgrSuccess) + label + wrapped[index] + std::string(kSgrReset), width));
        }
      }

      auto const output = output_preview_text(item);
      auto const output_repeats_result = output && same_payload(*output, item.result_summary);
      auto const sanitized_result = sanitize_terminal_text(item.result_summary);
      auto const result_visible_in_primary =
          primary_raw == complete_primary_raw && !sanitized_result.empty() && complete_primary_raw.find(sanitized_result) != std::string::npos;
      auto const generic_listing_result = directory_listing_tool(item) && item.result_summary == "ok";
      if (!suppress_result_summary && !item.result_summary.empty() && !generic_listing_result && !permission_audit_payload(item, item.result_summary) &&
          !output_repeats_result && !same_payload(item.result_summary, primary) && !result_visible_in_primary)
      {
        append_tool_detail_lines(lines, "result", project_display_path_aliases(item, item.result_summary), width);
      }
      if (output && !permission_audit_payload(item, *output) && (!output_repeats_result || item.truncated))
      {
        auto displayed = project_display_path_aliases(item, *output);
        auto const cap = presentation == ToolPresentation::Expanded ? kExpandedOutputPreviewLines : rich_output_cap(item);
        append_output_preview_lines(lines, item, "output", displayed, width, cap, is_shell, presentation != ToolPresentation::Expanded);
      }

      append_tool_detail_lines(lines, "changed", changed_paths_summary(item, true), width);
      if (!truncation.empty())
        append_tool_detail_lines(lines, "truncation", truncation, width);
      if (presentation == ToolPresentation::Expanded)
      {
        auto const retained = is_shell ? byte_retention_summary(item) : std::string{};
        append_tool_detail_lines(lines, "bytes", retained, width);
        if (!item.spill_path.empty())
          append_tool_detail_lines(lines, "full output", item.spill_path, width);
        if (item.spill_truncated)
          append_tool_detail_lines(lines, "spill incomplete", "true", width);
        append_diff_lines(lines, item, width, kExpandedDiffPreviewLines);
      }
      else
      {
        if (!item.spill_path.empty())
          append_tool_detail_lines(lines, "more", "expanded view includes retained output at " + item.spill_path, width);
        if (!item.diff.empty())
          append_diff_lines(lines, item, width, kRichFilePreviewLines);
      }
    }  // non-task/job, non-todowrite detail body
  }

  for (auto& line : lines)
  {
    if (has_outer_margins && line.starts_with("  "))
      line.erase(0, 2);
    line = tool_surface_line(std::move(line), width);
    if (has_outer_margins)
      line = "  " + std::move(line) + "  ";
    line = fit_line_preserving_sgr(std::move(line), outer_width);
  }
  std::vector<bool> presentation_private_rows(lines.size(), false);
  std::fill(presentation_private_rows.begin() + static_cast<std::ptrdiff_t>(private_rows_first),
            presentation_private_rows.begin() + static_cast<std::ptrdiff_t>(private_rows_past_last), true);
  return ToolCardRenderResult{.lines = std::move(lines), .presentation_private_rows = std::move(presentation_private_rows)};
}

std::vector<std::string> render_tool_card(ToolTimelineItem const& item, std::size_t width, ToolPresentation inherited, bool suppress_result_summary)
{
  return render_tool_card_rows(item, width, inherited, suppress_result_summary).lines;
}

std::vector<std::string> render_tool_card(ToolTimelineItem const& item, std::size_t width, bool global_details_visible, bool suppress_result_summary)
{
  return render_tool_card(item, width, global_details_visible ? ToolPresentation::Expanded : ToolPresentation::Compact, suppress_result_summary);
}

}  // namespace detail
}  // namespace ava::tui
