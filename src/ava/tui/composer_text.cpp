#include "sys.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/text_wrap.h"
#include "ava/core/utf8.h"

#include <algorithm>
#include <climits>
#include <cwchar>

namespace ava::tui {

std::string sanitize_terminal_text(std::string_view text)
{
  std::string sanitized;
  sanitized.reserve(text.size());
  for (std::size_t index = 0; index < text.size();)
  {
    auto const byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x20 || byte == 0x7F)
    {
      if (byte == '\t')
      {
        sanitized += "  ";
      }
      else
      {
        sanitized.push_back('?');
      }
      ++index;
      continue;
    }

    auto const length = detail::utf8_sequence_length(byte);
    char32_t codepoint = 0;
    if (!detail::decode_utf8_codepoint(text, index, length, codepoint))
    {
      sanitized.push_back('?');
      ++index;
      continue;
    }

    if (codepoint >= 0x80 && codepoint <= 0x9F)
    {
      sanitized.push_back('?');
    }
    else
    {
      sanitized.append(text.substr(index, length));
    }
    index += length;
  }
  return sanitized;
}

std::vector<std::string> split_lines(std::string_view text)
{
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t index = 0; index <= text.size(); ++index)
  {
    if (index == text.size())
    {
      lines.emplace_back(text.substr(start, index - start));
      break;
    }
    if (text[index] != '\n' && text[index] != '\r')
      continue;

    lines.emplace_back(text.substr(start, index - start));
    if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n')
    {
      ++index;
    }
    start = index + 1;
  }
  if (lines.empty())
    lines.emplace_back();
  return lines;
}

namespace detail {

bool is_utf8_continuation(unsigned char byte)
{
  return (byte & 0xC0U) == 0x80U;
}

std::size_t utf8_sequence_length(unsigned char byte)
{
  return ava::core::utf8_expected_width(byte);
}

bool decode_utf8_codepoint(std::string_view text, std::size_t start, std::size_t length, char32_t& codepoint)
{
  auto const decoded = ava::core::decode_utf8_scalar(text, start);
  if (!decoded || decoded->length != length)
    return false;
  codepoint = static_cast<char32_t>(decoded->scalar);
  return true;
}

bool is_wide_codepoint(char32_t codepoint)
{
  return (codepoint >= 0x1100 && codepoint <= 0x115F) || (codepoint >= 0x2329 && codepoint <= 0x232A) || (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
         (codepoint >= 0xAC00 && codepoint <= 0xD7A3) || (codepoint >= 0xF900 && codepoint <= 0xFAFF) || (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
         (codepoint >= 0xFE30 && codepoint <= 0xFE6F) || (codepoint >= 0xFF00 && codepoint <= 0xFF60) || (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) ||
         (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) || (codepoint >= 0x20000 && codepoint <= 0x3FFFD);
}

bool is_regional_indicator(char32_t codepoint)
{
  return codepoint >= 0x1F1E6 && codepoint <= 0x1F1FF;
}

bool is_emoji_modifier(char32_t codepoint)
{
  return codepoint >= 0x1F3FB && codepoint <= 0x1F3FF;
}

bool is_variation_selector(char32_t codepoint)
{
  return (codepoint >= 0xFE00 && codepoint <= 0xFE0F) || (codepoint >= 0xE0100 && codepoint <= 0xE01EF);
}

bool is_emoji_cluster_start(char32_t codepoint)
{
  return (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) || (codepoint >= 0x2600 && codepoint <= 0x26FF) || codepoint == 0x2705;
}

bool is_zero_width_codepoint(char32_t codepoint)
{
  return (codepoint >= 0x0300 && codepoint <= 0x036F) || (codepoint >= 0x0483 && codepoint <= 0x0489) || (codepoint >= 0x0591 && codepoint <= 0x05BD) ||
         codepoint == 0x05BF || (codepoint >= 0x05C1 && codepoint <= 0x05C2) || (codepoint >= 0x05C4 && codepoint <= 0x05C5) || codepoint == 0x05C7 ||
         (codepoint >= 0x0610 && codepoint <= 0x061A) || (codepoint >= 0x064B && codepoint <= 0x065F) || codepoint == 0x0670 ||
         (codepoint >= 0x06D6 && codepoint <= 0x06DC) || (codepoint >= 0x06DF && codepoint <= 0x06E4) || (codepoint >= 0x06E7 && codepoint <= 0x06E8) ||
         (codepoint >= 0x06EA && codepoint <= 0x06ED) || (codepoint >= 0x0711 && codepoint <= 0x0711) || (codepoint >= 0x0730 && codepoint <= 0x074A) ||
         (codepoint >= 0x07A6 && codepoint <= 0x07B0) || (codepoint >= 0x07EB && codepoint <= 0x07F3) || (codepoint >= 0x0816 && codepoint <= 0x0819) ||
         (codepoint >= 0x081B && codepoint <= 0x0823) || (codepoint >= 0x0825 && codepoint <= 0x0827) || (codepoint >= 0x0829 && codepoint <= 0x082D) ||
         (codepoint >= 0x0859 && codepoint <= 0x085B) || (codepoint >= 0x08D3 && codepoint <= 0x08E1) || (codepoint >= 0x08E3 && codepoint <= 0x0903) ||
         (codepoint >= 0x093A && codepoint <= 0x093C) || (codepoint >= 0x0941 && codepoint <= 0x0948) || codepoint == 0x094D ||
         (codepoint >= 0x0951 && codepoint <= 0x0957) || (codepoint >= 0x0962 && codepoint <= 0x0963) || codepoint == 0x200C || codepoint == 0x200D ||
         (codepoint >= 0x20D0 && codepoint <= 0x20FF) || (codepoint >= 0xFE00 && codepoint <= 0xFE0F) || (codepoint >= 0xFE20 && codepoint <= 0xFE2F) ||
         (codepoint >= 0xE0100 && codepoint <= 0xE01EF);
}

std::size_t codepoint_columns(char32_t codepoint)
{
  if (is_zero_width_codepoint(codepoint))
    return 0;
  if (codepoint == 0 || codepoint > static_cast<char32_t>(WCHAR_MAX))
    return 1;
  auto const width = ::wcwidth(static_cast<wchar_t>(codepoint));
  if (width >= 0)
    return static_cast<std::size_t>(width);
  return is_wide_codepoint(codepoint) ? std::size_t{2} : std::size_t{1};
}

struct DecodedCodepoint
{
  char32_t codepoint = 0;
  std::size_t length = 0;
};

std::optional<DecodedCodepoint> decode_next_codepoint(std::string_view text, std::size_t index)
{
  if (index >= text.size())
    return std::nullopt;
  auto const length = utf8_sequence_length(static_cast<unsigned char>(text[index]));
  char32_t codepoint = 0;
  if (!decode_utf8_codepoint(text, index, length, codepoint))
    return std::nullopt;
  return DecodedCodepoint{.codepoint = codepoint, .length = length};
}

bool is_cluster_extending_mark(char32_t codepoint)
{
  // Trailing marks that bind to a preceding base for cursor/editing atomicity.
  // ZWJ/ZWNJ are handled only inside emoji sequences, not after ordinary bases.
  if (codepoint == 0x200C || codepoint == 0x200D)
    return false;
  return is_zero_width_codepoint(codepoint) || is_variation_selector(codepoint);
}

std::optional<std::size_t> emoji_cluster_length(std::string_view text, std::size_t index, char32_t first, std::size_t first_length)
{
  if (is_regional_indicator(first))
  {
    auto length = first_length;
    if (auto const next = decode_next_codepoint(text, index + length); next && is_regional_indicator(next->codepoint))
      length += next->length;
    return length;
  }

  if (!is_emoji_cluster_start(first))
    return std::nullopt;

  auto length = first_length;
  while (auto const next = decode_next_codepoint(text, index + length))
  {
    if (is_variation_selector(next->codepoint) || is_emoji_modifier(next->codepoint))
    {
      length += next->length;
      continue;
    }
    if (next->codepoint == 0x200D)
    {
      auto const joined = decode_next_codepoint(text, index + length + next->length);
      if (!joined)
        break;
      length += next->length + joined->length;
      continue;
    }
    break;
  }
  return length;
}

// Compact cluster boundaries shared by rendering width and composer cursor/delete.
// Not full UAX #29: base+marks, regional-indicator pairs, emoji modifiers, ZWJ emoji only.
std::size_t terminal_text_cluster_bytes(std::string_view text, std::size_t index)
{
  if (index >= text.size())
    return 0;
  auto const length = utf8_sequence_length(static_cast<unsigned char>(text[index]));
  char32_t codepoint = 0;
  if (!decode_utf8_codepoint(text, index, length, codepoint))
    return 1;
  if (auto const cluster_length = emoji_cluster_length(text, index, codepoint, length))
    return *cluster_length;

  auto bytes = length;
  // Orphan combining/variation marks stay single-codepoint clusters.
  if (!is_cluster_extending_mark(codepoint))
  {
    while (auto const next = decode_next_codepoint(text, index + bytes))
    {
      if (!is_cluster_extending_mark(next->codepoint))
        break;
      bytes += next->length;
    }
  }
  return bytes;
}

TerminalTextCell terminal_text_cell(std::string_view text, std::size_t index)
{
  if (index >= text.size())
    return {};
  auto const length = utf8_sequence_length(static_cast<unsigned char>(text[index]));
  char32_t codepoint = 0;
  if (!decode_utf8_codepoint(text, index, length, codepoint))
    return TerminalTextCell{.bytes = 1, .columns = 1, .valid = false};
  auto const cluster_bytes = terminal_text_cluster_bytes(text, index);
  if (emoji_cluster_length(text, index, codepoint, length))
    return TerminalTextCell{.bytes = cluster_bytes, .columns = 2, .valid = true};
  return TerminalTextCell{.bytes = cluster_bytes, .columns = codepoint_columns(codepoint), .valid = true};
}

bool skip_sgr_sequence(std::string_view text, std::size_t& index)
{
  if (index + 1 >= text.size() || text[index] != '\x1b' || text[index + 1] != '[')
  {
    return false;
  }
  auto end = index + 2;
  while (end < text.size() && text[end] != 'm')
  {
    ++end;
  }
  if (end >= text.size())
  {
    return false;
  }
  index = end + 1;
  return true;
}

bool skip_osc_sequence(std::string_view text, std::size_t& index)
{
  if (index + 1 >= text.size() || text[index] != '\x1b' || text[index + 1] != ']')
  {
    return false;
  }
  auto end = index + 2;
  while (end < text.size())
  {
    if (text[end] == '\a')
    {
      index = end + 1;
      return true;
    }
    if (text[end] == '\x1b' && end + 1 < text.size() && text[end + 1] == '\\')
    {
      index = end + 2;
      return true;
    }
    ++end;
  }
  return false;
}

std::size_t terminal_text_columns(std::string_view text)
{
  std::size_t columns = 0;
  for (std::size_t index = 0; index < text.size();)
  {
    if (skip_sgr_sequence(text, index) || skip_osc_sequence(text, index))
    {
      continue;
    }
    auto const cell = terminal_text_cell(text, index);
    index += cell.bytes;
    columns += cell.columns;
  }
  return columns;
}

std::string fit_line(std::string text, std::size_t width)
{
  if (width == 0)
    return {};
  text = sanitize_terminal_text(text);
  if (terminal_text_columns(text) <= width)
    return text;
  if (width <= 3)
  {
    std::string output;
    std::size_t visible = 0;
    for (std::size_t index = 0; index < text.size() && visible < width;)
    {
      auto const cell = terminal_text_cell(text, index);
      if (cell.valid)
      {
        if (visible + cell.columns > width)
          break;
        output.append(text.substr(index, cell.bytes));
        index += cell.bytes;
        visible += cell.columns;
      }
      else
      {
        output.push_back('?');
        ++index;
        ++visible;
      }
    }
    return output;
  }

  auto const visible_budget = width - 3;
  std::string output;
  std::size_t visible = 0;
  for (std::size_t index = 0; index < text.size() && visible < visible_budget;)
  {
    auto const cell = terminal_text_cell(text, index);
    if (cell.valid)
    {
      if (visible + cell.columns > visible_budget)
        break;
      output.append(text.substr(index, cell.bytes));
      index += cell.bytes;
      visible += cell.columns;
    }
    else
    {
      output.push_back('?');
      ++index;
      ++visible;
    }
  }
  output += "...";
  return output;
}

std::string fit_line_preserving_sgr(std::string text, std::size_t width)
{
  if (width == 0)
    return {};
  auto const cols = terminal_text_columns(text);
  if (cols <= width)
    return text;
  if (width <= 3)
  {
    return std::string(width, '.');
  }

  auto const visible_budget = width - 3;
  std::size_t visible = 0;
  bool emitted_sgr = false;
  bool emitted_osc = false;
  std::string output;
  for (std::size_t index = 0; index < text.size() && visible < visible_budget;)
  {
    auto const before_sgr = index;
    if (skip_sgr_sequence(text, index))
    {
      output.append(text.substr(before_sgr, index - before_sgr));
      emitted_sgr = true;
      continue;
    }
    auto const before_osc = index;
    if (skip_osc_sequence(text, index))
    {
      output.append(text.substr(before_osc, index - before_osc));
      emitted_osc = true;
      continue;
    }

    auto const cell = terminal_text_cell(text, index);
    if (cell.valid)
    {
      if (visible + cell.columns > visible_budget)
        break;
      output.append(text.substr(index, cell.bytes));
      index += cell.bytes;
      visible += cell.columns;
    }
    else
    {
      output.push_back('?');
      ++index;
      ++visible;
    }
  }
  if (emitted_osc)
    output += "\x1b]8;;\x1b\\";
  output += "...";
  if (emitted_sgr)
    output += kSgrReset;
  return output;
}

std::string surface_line(std::string_view background_sgr, std::string line, std::size_t width)
{
  auto const background = std::string(background_sgr);
  std::string painted;
  painted.reserve(background.size() + line.size());
  painted += background;
  for (std::size_t index = 0; index < line.size();)
  {
    if (line.compare(index, kSgrReset.size(), kSgrReset) == 0)
    {
      painted += std::string(kSgrReset);
      painted += background;
      index += kSgrReset.size();
      continue;
    }
    painted.push_back(line[index]);
    ++index;
  }

  line = fit_line_preserving_sgr(std::move(painted), width);
  auto const cols = terminal_text_columns(line);
  if (cols < width)
  {
    line += background + std::string(width - cols, ' ');
  }
  line += std::string(kSgrReset);
  return line;
}

std::string screen_surface_line(std::string line, std::size_t width)
{
  return surface_line(kSgrScreenBg, std::move(line), width);
}

std::string composer_surface_line(std::string line, std::size_t width)
{
  return surface_line(kSgrComposerBg, std::move(line), width);
}

std::string tool_surface_line(std::string line, std::size_t width)
{
  return surface_line(kSgrToolBg, std::move(line), width);
}

std::string question_surface_line(std::string line, std::size_t width)
{
  return surface_line(kSgrQuestionBg, std::move(line), width);
}

std::vector<std::string> wrap_transcript_text(std::string_view text, std::size_t width)
{
  auto const sanitized = sanitize_terminal_text(text);
  auto const content_width = std::max<std::size_t>(1, width > 4 ? width - 4 : width);
  return wrap_ansi_text(sanitized, content_width);
}

}  // namespace detail
}  // namespace ava::tui
