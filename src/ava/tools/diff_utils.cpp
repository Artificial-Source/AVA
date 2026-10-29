#include "sys.h"
#include "ava/tools/diff_utils.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace ava::tools {
namespace {

struct LogicalLine
{
  std::string_view text;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct Alignment
{
  std::size_t old_index = 0;
  std::size_t new_index = 0;
  bool found = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

std::vector<LogicalLine> split_lines(std::string_view text)
{
  std::vector<LogicalLine> lines;
  std::size_t start = 0;
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    if (text[index] != '\n')
      continue;
    auto end = index;
    if (end > start && text[end - 1] == '\r')
      --end;
    lines.push_back(LogicalLine{.text = text.substr(start, end - start)});
    start = index + 1;
  }
  if (start < text.size())
  {
    lines.push_back(LogicalLine{.text = text.substr(start)});
  }
  return lines;
}

void append_bounded(DiffPreview& preview, std::string_view text, std::size_t max_bytes)
{
  if (preview.text.size() >= max_bytes)
  {
    preview.truncated = true;
    return;
  }
  auto const remaining = max_bytes - preview.text.size();
  if (text.size() <= remaining)
  {
    preview.text.append(text);
    return;
  }
  preview.text.append(text.substr(0, remaining));
  preview.truncated = true;
}

void append_diff_line(DiffPreview& preview, char prefix, std::string_view line, std::size_t max_bytes)
{
  char prefix_text[] = {prefix, '\0'};
  append_bounded(preview, std::string_view(prefix_text, 1), max_bytes);
  append_bounded(preview, line, max_bytes);
  append_bounded(preview, "\n", max_bytes);
}

Alignment next_alignment(std::vector<LogicalLine> const& old_lines, std::vector<LogicalLine> const& new_lines, std::size_t old_start, std::size_t old_end,
                         std::size_t new_start, std::size_t new_end)
{
  constexpr std::size_t kAlignmentLookahead = 64;
  auto const old_limit = std::min(old_end, old_start + kAlignmentLookahead);
  auto const new_limit = std::min(new_end, new_start + kAlignmentLookahead);
  for (std::size_t old_index = old_start; old_index < old_limit; ++old_index)
  {
    for (std::size_t new_index = new_start; new_index < new_limit; ++new_index)
    {
      if (old_lines[old_index].text == new_lines[new_index].text)
      {
        return Alignment{.old_index = old_index, .new_index = new_index, .found = true};
      }
    }
  }
  return {};
}

void append_changed_region(DiffPreview& preview, std::vector<LogicalLine> const& old_lines, std::vector<LogicalLine> const& new_lines, std::size_t old_start,
                           std::size_t old_end, std::size_t new_start, std::size_t new_end, std::size_t max_bytes)
{
  auto old_index = old_start;
  auto new_index = new_start;
  while (old_index < old_end || new_index < new_end)
  {
    if (old_index < old_end && new_index < new_end && old_lines[old_index].text == new_lines[new_index].text)
    {
      append_diff_line(preview, ' ', old_lines[old_index].text, max_bytes);
      ++old_index;
      ++new_index;
      continue;
    }

    auto const alignment = next_alignment(old_lines, new_lines, old_index, old_end, new_index, new_end);
    auto const old_stop = alignment.found ? alignment.old_index : old_end;
    auto const new_stop = alignment.found ? alignment.new_index : new_end;
    for (; old_index < old_stop; ++old_index) append_diff_line(preview, '-', old_lines[old_index].text, max_bytes);
    for (; new_index < new_stop; ++new_index) append_diff_line(preview, '+', new_lines[new_index].text, max_bytes);
  }
}

std::string hunk_header(std::size_t old_start, std::size_t old_count, std::size_t new_start, std::size_t new_count)
{
  return "@@ -" + std::to_string(old_count == 0 ? old_start : old_start + 1) + "," + std::to_string(old_count) + " +" +
         std::to_string(new_count == 0 ? new_start : new_start + 1) + "," + std::to_string(new_count) + " @@\n";
}

}  // namespace

DiffPreview unified_diff(std::string_view old_content, std::string_view new_content, std::filesystem::path const& old_path,
                         std::filesystem::path const& new_path, std::size_t max_bytes)
{
  DiffPreview preview;
  if (old_content == new_content || max_bytes == 0)
    return preview;

  auto const old_lines = split_lines(old_content);
  auto const new_lines = split_lines(new_content);

  std::size_t prefix = 0;
  while (prefix < old_lines.size() && prefix < new_lines.size() && old_lines[prefix].text == new_lines[prefix].text)
  {
    ++prefix;
  }

  std::size_t suffix = 0;
  while (suffix + prefix < old_lines.size() && suffix + prefix < new_lines.size() &&
         old_lines[old_lines.size() - 1 - suffix].text == new_lines[new_lines.size() - 1 - suffix].text)
  {
    ++suffix;
  }

  constexpr std::size_t kContextLines = 3;
  auto const old_change_end = old_lines.size() - suffix;
  auto const new_change_end = new_lines.size() - suffix;
  auto const hunk_start = prefix > kContextLines ? prefix - kContextLines : 0;
  auto const old_hunk_end = std::min(old_lines.size(), old_change_end + kContextLines);
  auto const new_hunk_end = std::min(new_lines.size(), new_change_end + kContextLines);

  append_bounded(preview, "--- " + old_path.generic_string() + "\n", max_bytes);
  append_bounded(preview, "+++ " + new_path.generic_string() + "\n", max_bytes);
  append_bounded(preview, hunk_header(hunk_start, old_hunk_end - hunk_start, hunk_start, new_hunk_end - hunk_start), max_bytes);

  for (std::size_t index = hunk_start; index < prefix; ++index)
  {
    append_diff_line(preview, ' ', old_lines[index].text, max_bytes);
  }
  append_changed_region(preview, old_lines, new_lines, prefix, old_change_end, prefix, new_change_end, max_bytes);
  for (std::size_t old_index = old_change_end, new_index = new_change_end; old_index < old_hunk_end && new_index < new_hunk_end; ++old_index, ++new_index)
  {
    append_diff_line(preview, ' ', old_lines[old_index].text, max_bytes);
  }

  return preview;
}

}  // namespace ava::tools
