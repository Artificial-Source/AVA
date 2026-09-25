#include "sys.h"
#include "ava/tui/command_output.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/mermaid_projection.h"
#include "ava/tui/runtime_transcript_selection_internal.h"
#include "ava/tui/theme.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdio>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <curses.h>

namespace ava::tui {
namespace {

using detail::kSgrAccent;
using detail::kSgrBold;
using detail::kSgrDim;
using detail::kSgrError;
using detail::kSgrReset;
using detail::kSgrSuccess;
using detail::kSgrWarning;
using detail::NcursesColorRole;

constexpr short kPairText = 1;
constexpr short kPairMuted = 2;
constexpr short kPairSuccess = 3;
constexpr short kPairWarning = 4;
constexpr short kPairError = 5;
constexpr short kPairAccent = 6;
constexpr short kPairScreen = 7;
constexpr short kPairComposer = 8;
constexpr short kPairComposerText = 9;
constexpr short kPairComposerMuted = 10;
constexpr short kPairComposerSuccess = 11;
constexpr short kPairComposerWarning = 12;
constexpr short kPairComposerError = 13;
constexpr short kPairComposerAccent = 14;
constexpr short kPairToolText = 15;
constexpr short kPairToolMuted = 16;
constexpr short kPairToolSuccess = 17;
constexpr short kPairToolWarning = 18;
constexpr short kPairToolError = 19;
constexpr short kPairToolAccent = 20;
constexpr short kPairQuestionText = 21;
constexpr short kPairQuestionMuted = 22;
constexpr short kPairQuestionSuccess = 23;
constexpr short kPairQuestionWarning = 24;
constexpr short kPairQuestionError = 25;
constexpr short kPairQuestionAccent = 26;

constexpr short kColorComposerBg = 17;
constexpr short kColorMuted = 18;
constexpr short kColorAccent = 19;
constexpr short kColorToolBg = 20;
constexpr short kColorQuestionBg = 21;

std::vector<std::size_t>& active_kitty_image_ids()
{
  static std::vector<std::size_t> image_ids;
  return image_ids;
}

bool terminal_graphics_equal(std::vector<TerminalGraphicOverlay> const& left, std::vector<TerminalGraphicOverlay> const& right)
{
  if (left.size() != right.size())
    return false;
  for (std::size_t index = 0; index < left.size(); ++index)
  {
    auto const& lhs = left[index];
    auto const& rhs = right[index];
    if (lhs.protocol != rhs.protocol || lhs.row != rhs.row || lhs.column != rhs.column || lhs.rows != rhs.rows || lhs.columns != rhs.columns ||
        lhs.image_id != rhs.image_id || lhs.sequence != rhs.sequence)
    {
      return false;
    }
  }
  return true;
}

enum class BackgroundRole
{
  Screen,
  Composer,
  Tool,
  Question,
};

struct CursesStyle
{
  attr_t attributes = {};
  NcursesColorRole color = NcursesColorRole::Text;
  BackgroundRole background = BackgroundRole::Screen;
};

struct CursorPlacement
{
  std::size_t row = 0;
  std::size_t column = 0;
};

std::string strip_sgr_sequences(std::string_view text)
{
  std::string stripped;
  stripped.reserve(text.size());
  for (std::size_t index = 0; index < text.size();)
  {
    auto const before = index;
    if (detail::skip_sgr_sequence(text, index))
    {
      continue;
    }
    stripped.push_back(text[before]);
    ++index;
  }
  return stripped;
}

std::string strip_terminal_styling(std::string_view text)
{
  std::string stripped;
  stripped.reserve(text.size());
  for (std::size_t index = 0; index < text.size();)
  {
    auto const before = index;
    if (detail::skip_sgr_sequence(text, index) || detail::skip_osc_sequence(text, index))
      continue;
    stripped.push_back(text[before]);
    ++index;
  }
  return sanitize_terminal_text(stripped);
}

std::vector<std::string> quiet_modal_backdrop(std::vector<std::string> lines)
{
  for (auto& line : lines)
  {
    line = std::string(detail::kSgrThinking) + strip_terminal_styling(line) + std::string(kSgrReset);
  }
  return lines;
}

std::vector<std::string> strip_sgr_frame(std::vector<std::string> lines)
{
  for (auto& line : lines)
  {
    line = strip_sgr_sequences(line);
  }
  return lines;
}

bool line_contains_osc_sequence(std::string_view line)
{
  for (std::size_t index = 0; index < line.size();)
  {
    if (detail::skip_osc_sequence(line, index))
      return true;
    ++index;
  }
  return false;
}

void initialize_color_pairs()
{
  auto& terminal_context = core::Application::instance().terminal_context();
  static std::optional<std::string> initialized_theme;
  auto const theme = active_tui_theme();
  auto const theme_key = theme.name + "|" + theme.badge + "|" + theme.revision;
  if (theme.kind == TuiThemeKind::Plain || !terminal_context.has_colors())
  {
    initialized_theme = theme_key;
    return;
  }
  if (initialized_theme && *initialized_theme == theme_key)
    return;
  initialized_theme = theme_key;
  auto screen_bg = COLOR_BLACK;
  auto composer_bg = COLOR_BLACK;
  auto tool_bg = COLOR_BLACK;
  auto question_bg = COLOR_BLACK;
  auto text_fg = COLOR_WHITE;
  auto muted_fg = COLOR_WHITE;
  auto success_fg = COLOR_GREEN;
  auto warning_fg = COLOR_YELLOW;
  auto error_fg = COLOR_RED;
  auto accent_fg = COLOR_BLUE;
  auto color_or_default = [](int value, short fallback) -> short {
    if (value < 0)
      return -1;
    if (value <= SHRT_MAX && value < COLORS)
      return static_cast<short>(value);
    return fallback;
  };
  if (theme.kind == TuiThemeKind::Custom && theme.palette)
  {
    screen_bg = color_or_default(theme.palette->screen_bg, COLOR_BLACK);
    composer_bg = color_or_default(theme.palette->composer_bg, screen_bg);
    tool_bg = color_or_default(theme.palette->tool_bg, composer_bg);
    question_bg = color_or_default(theme.palette->question_bg, composer_bg);
    text_fg = color_or_default(theme.palette->text, COLOR_WHITE);
    muted_fg = color_or_default(theme.palette->muted, COLOR_CYAN);
    success_fg = color_or_default(theme.palette->success, COLOR_GREEN);
    warning_fg = color_or_default(theme.palette->warning, COLOR_YELLOW);
    error_fg = color_or_default(theme.palette->error, COLOR_RED);
    accent_fg = color_or_default(theme.palette->accent, COLOR_CYAN);
  }
  else if (theme.kind == TuiThemeKind::Light)
  {
    // Built-in screen canvas inherits the terminal default background (-1).
    screen_bg = -1;
    composer_bg = COLOR_WHITE;
    text_fg = COLOR_BLACK;
    muted_fg = COLOR_BLACK;
    accent_fg = COLOR_BLUE;
    if (can_change_color() && COLORS > kColorAccent)
    {
      static_cast<void>(init_color(kColorComposerBg, 914, 933, 961));
      static_cast<void>(init_color(kColorMuted, 430, 430, 430));
      static_cast<void>(init_color(kColorAccent, 120, 360, 780));
      composer_bg = kColorComposerBg;
      muted_fg = kColorMuted;
      accent_fg = kColorAccent;
    }
    tool_bg = composer_bg;
    question_bg = composer_bg;
    if (can_change_color() && COLORS > kColorQuestionBg)
    {
      static_cast<void>(init_color(kColorQuestionBg, 875, 902, 953));
      question_bg = kColorQuestionBg;
    }
    if (can_change_color() && COLORS > kColorToolBg)
    {
      static_cast<void>(init_color(kColorToolBg, 902, 918, 949));
      tool_bg = kColorToolBg;
    }
  }
  else
  {
    // Built-in screen canvas inherits the terminal default background (-1).
    screen_bg = -1;
    if (can_change_color() && COLORS > kColorComposerBg)
    {
      static_cast<void>(init_color(kColorComposerBg, 102, 122, 180));
      composer_bg = kColorComposerBg;
    }
    tool_bg = composer_bg;
    question_bg = composer_bg;
    if (can_change_color() && COLORS > kColorQuestionBg)
    {
      static_cast<void>(init_color(kColorQuestionBg, 125, 153, 224));
      question_bg = kColorQuestionBg;
    }
    if (can_change_color() && COLORS > kColorToolBg)
    {
      static_cast<void>(init_color(kColorToolBg, 71, 90, 133));
      tool_bg = kColorToolBg;
    }
    if (can_change_color() && COLORS > kColorAccent)
    {
      static_cast<void>(init_color(kColorMuted, 510, 510, 510));
      static_cast<void>(init_color(kColorAccent, 280, 480, 720));
      muted_fg = kColorMuted;
      accent_fg = kColorAccent;
    }
  }
  static_cast<void>(init_pair(kPairText, text_fg, screen_bg));
  static_cast<void>(init_pair(kPairMuted, muted_fg, screen_bg));
  static_cast<void>(init_pair(kPairSuccess, success_fg, screen_bg));
  static_cast<void>(init_pair(kPairWarning, warning_fg, screen_bg));
  static_cast<void>(init_pair(kPairError, error_fg, screen_bg));
  static_cast<void>(init_pair(kPairAccent, accent_fg, screen_bg));
  static_cast<void>(init_pair(kPairScreen, text_fg, screen_bg));
  static_cast<void>(init_pair(kPairComposer, text_fg, composer_bg));
  static_cast<void>(init_pair(kPairComposerText, text_fg, composer_bg));
  static_cast<void>(init_pair(kPairComposerMuted, muted_fg, composer_bg));
  static_cast<void>(init_pair(kPairComposerSuccess, success_fg, composer_bg));
  static_cast<void>(init_pair(kPairComposerWarning, warning_fg, composer_bg));
  static_cast<void>(init_pair(kPairComposerError, error_fg, composer_bg));
  static_cast<void>(init_pair(kPairComposerAccent, accent_fg, composer_bg));
  if (COLOR_PAIRS > kPairToolAccent)
  {
    static_cast<void>(init_pair(kPairToolText, text_fg, tool_bg));
    static_cast<void>(init_pair(kPairToolMuted, muted_fg, tool_bg));
    static_cast<void>(init_pair(kPairToolSuccess, success_fg, tool_bg));
    static_cast<void>(init_pair(kPairToolWarning, warning_fg, tool_bg));
    static_cast<void>(init_pair(kPairToolError, error_fg, tool_bg));
    static_cast<void>(init_pair(kPairToolAccent, accent_fg, tool_bg));
  }
  if (COLOR_PAIRS > kPairQuestionAccent)
  {
    static_cast<void>(init_pair(kPairQuestionText, text_fg, question_bg));
    static_cast<void>(init_pair(kPairQuestionMuted, muted_fg, question_bg));
    static_cast<void>(init_pair(kPairQuestionSuccess, success_fg, question_bg));
    static_cast<void>(init_pair(kPairQuestionWarning, warning_fg, question_bg));
    static_cast<void>(init_pair(kPairQuestionError, error_fg, question_bg));
    static_cast<void>(init_pair(kPairQuestionAccent, accent_fg, question_bg));
  }
}

short color_pair_for(CursesStyle const& style)
{
  if (style.background == BackgroundRole::Composer)
  {
    switch (style.color)
    {
      case NcursesColorRole::Muted:
        return kPairComposerMuted;
      case NcursesColorRole::Success:
        return kPairComposerSuccess;
      case NcursesColorRole::Warning:
        return kPairComposerWarning;
      case NcursesColorRole::Error:
        return kPairComposerError;
      case NcursesColorRole::Accent:
        return kPairComposerAccent;
      case NcursesColorRole::Text:
        return kPairComposerText;
    }
  }
  if (style.background == BackgroundRole::Tool && COLOR_PAIRS > kPairToolAccent)
  {
    switch (style.color)
    {
      case NcursesColorRole::Muted:
        return kPairToolMuted;
      case NcursesColorRole::Success:
        return kPairToolSuccess;
      case NcursesColorRole::Warning:
        return kPairToolWarning;
      case NcursesColorRole::Error:
        return kPairToolError;
      case NcursesColorRole::Accent:
        return kPairToolAccent;
      case NcursesColorRole::Text:
        return kPairToolText;
    }
  }
  if (style.background == BackgroundRole::Question && COLOR_PAIRS > kPairQuestionAccent)
  {
    switch (style.color)
    {
      case NcursesColorRole::Muted:
        return kPairQuestionMuted;
      case NcursesColorRole::Success:
        return kPairQuestionSuccess;
      case NcursesColorRole::Warning:
        return kPairQuestionWarning;
      case NcursesColorRole::Error:
        return kPairQuestionError;
      case NcursesColorRole::Accent:
        return kPairQuestionAccent;
      case NcursesColorRole::Text:
        return kPairQuestionText;
    }
  }

  switch (style.color)
  {
    case NcursesColorRole::Muted:
      return kPairMuted;
    case NcursesColorRole::Success:
      return kPairSuccess;
    case NcursesColorRole::Warning:
      return kPairWarning;
    case NcursesColorRole::Error:
      return kPairError;
    case NcursesColorRole::Accent:
      return kPairAccent;
    case NcursesColorRole::Text:
      return kPairText;
  }
  return kPairText;
}

attr_t curses_attributes(CursesStyle const& style)
{
  if (tui_plain_output())
    return style.attributes;
  if (!core::Application::instance().terminal_context().has_colors())
    return style.attributes;
  return style.attributes | COLOR_PAIR(color_pair_for(style));
}

bool parse_sgr_codes(std::string_view sequence, std::vector<int>& codes)
{
  codes.clear();
  if (sequence.empty())
    return false;
  std::size_t start = 0;
  while (start <= sequence.size())
  {
    auto const end = sequence.find(';', start);
    auto const token = sequence.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (token.empty())
    {
      codes.push_back(0);
    }
    else
    {
      auto const token_text = std::string(token);
      char* parsed_end = nullptr;
      auto const value = std::strtol(token_text.c_str(), &parsed_end, 10);
      if (parsed_end == nullptr || *parsed_end != '\0')
        return false;
      codes.push_back(static_cast<int>(value));
    }
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return true;
}

void apply_sgr_codes(std::vector<int> const& codes, CursesStyle& style)
{
  if (codes.empty())
  {
    style = CursesStyle{};
    return;
  }
  for (std::size_t index = 0; index < codes.size(); ++index)
  {
    switch (codes[index])
    {
      case 0:
        style = CursesStyle{};
        break;
      case 1:
        style.attributes |= A_BOLD;
        break;
      case 3:
#ifdef A_ITALIC
        style.attributes |= A_ITALIC;
#endif
        break;
      case 4:
        style.attributes |= A_UNDERLINE;
        break;
      case 7:
        style.attributes |= A_REVERSE;
        break;
      case 27:
        style.attributes &= ~A_REVERSE;
        break;
      case 9:
        // ncurses has no portable strike-through attribute. The snapshot renderer
        // still emits SGR 9 for ANSI-capable output; curses degrades to plain text.
        break;
      case 38:
        if (index + 4 < codes.size() && codes[index + 1] == 2)
        {
          auto const red = codes[index + 2];
          auto const green = codes[index + 3];
          auto const blue = codes[index + 4];
          auto const sgr = "\x1b[38;2;" + std::to_string(red) + ";" + std::to_string(green) + ";" + std::to_string(blue) + "m";
          auto const exact_role = detail::ncurses_color_role_for_sgr(sgr);
          if (exact_role != NcursesColorRole::Text || (red == 232 && green == 236 && blue == 241))
          {
            style.color = exact_role;
          }
          else if (red > 220 && green > 180 && blue < 80)
          {
            style.color = NcursesColorRole::Warning;
          }
          else if (red > 220 && green < 150 && blue < 150)
          {
            style.color = NcursesColorRole::Error;
          }
          else if (red < 100 && green > 180 && blue > 120)
          {
            style.color = NcursesColorRole::Success;
          }
          else if (red < 180 && green < 180 && blue < 190)
          {
            style.color = NcursesColorRole::Muted;
          }
          else if (blue > red && blue > green)
          {
            style.color = NcursesColorRole::Accent;
          }
          else
          {
            style.color = NcursesColorRole::Text;
          }
          index += 4;
        }
        break;
      case 48:
        if (index + 4 < codes.size() && codes[index + 1] == 2)
        {
          auto const red = codes[index + 2];
          auto const green = codes[index + 3];
          auto const blue = codes[index + 4];
          if (red == 18 && green == 23 && blue == 34)
            style.background = BackgroundRole::Tool;
          else if (red == 32 && green == 38 && blue == 56)
            style.background = BackgroundRole::Question;
          else
            style.background = (red > 15 || green > 20 || blue > 30) ? BackgroundRole::Composer : BackgroundRole::Screen;
          index += 4;
        }
        break;
      case 49:
        // SGR 49 restores the terminal default background (screen canvas).
        style.background = BackgroundRole::Screen;
        break;
      default:
        break;
    }
  }
}

void add_text_chunk(std::string_view text, CursesStyle style)
{
  if (text.empty())
    return;
  attrset(curses_attributes(style));
  static_cast<void>(addnstr(text.data(), static_cast<int>(std::min<std::size_t>(text.size(), INT_MAX))));
}

void draw_styled_line(std::string_view line, bool clear_to_end = true)
{
  CursesStyle style{.attributes = {}, .color = NcursesColorRole::Text, .background = BackgroundRole::Screen};
  std::vector<int> codes;
  std::size_t chunk_start = 0;
  for (std::size_t index = 0; index < line.size();)
  {
    if (line[index] != '\x1b' || index + 1 >= line.size())
    {
      ++index;
      continue;
    }
    if (line[index + 1] == ']')
    {
      auto const osc_start = index;
      if (!detail::skip_osc_sequence(line, index))
      {
        index = osc_start + 1;
        continue;
      }
      add_text_chunk(line.substr(chunk_start, osc_start - chunk_start), style);
      chunk_start = index;
      continue;
    }
    if (line[index + 1] != '[')
    {
      ++index;
      continue;
    }
    auto const end = line.find('m', index + 2);
    if (end == std::string_view::npos)
    {
      ++index;
      continue;
    }
    add_text_chunk(line.substr(chunk_start, index - chunk_start), style);
    if (parse_sgr_codes(line.substr(index + 2, end - index - 2), codes))
    {
      apply_sgr_codes(codes, style);
    }
    index = end + 1;
    chunk_start = index;
  }
  add_text_chunk(line.substr(chunk_start), style);
  attrset(curses_attributes(CursesStyle{.attributes = {}, .color = NcursesColorRole::Text, .background = BackgroundRole::Screen}));
  if (clear_to_end)
    clrtoeol();
}

CursorPlacement input_cursor_placement(ComposerSnapshot const& snapshot, std::size_t rendered_line_count, std::size_t width)
{
  auto const input_lines = detail::input_render_line_spans(snapshot.input, width);
  auto const composer_lines = detail::composer_block_line_count(snapshot, rendered_line_count, width);
  auto const policy = detail::composer_layout_policy(snapshot, rendered_line_count);
  auto const layout = detail::composer_input_layout(input_lines.size(), composer_lines, snapshot.draft_scroll_offset, policy.composer_top_padding_lines);
  auto const cursor_line = detail::input_cursor_line(snapshot, width);
  auto const visible_cursor_line = cursor_line < layout.first_visible ? std::size_t{0} : cursor_line - layout.first_visible;
  auto const composer_start_row = rendered_line_count >= composer_lines ? rendered_line_count - composer_lines : std::size_t{0};
  auto const visible_line = std::min(visible_cursor_line, layout.visible_input_lines == 0 ? std::size_t{0} : layout.visible_input_lines - 1);
  auto const column = detail::input_cursor_column(snapshot, width);
  return {.row = composer_start_row + layout.top_padding + visible_line, .column = column == 0 ? std::size_t{0} : column - 1};
}

constexpr auto kCanvasMaxWidth = std::size_t{120};
// Plugin attribution must be able to show a maximum 128-byte canonical id on
// one host-owned line at sufficiently wide terminal dimensions.
constexpr auto kPluginUiCanvasMaxWidth = std::size_t{160};
constexpr auto kSidebarWidth = std::size_t{38};
constexpr auto kSidebarActionableMinTerminalWidth = std::size_t{144};
constexpr auto kSidebarIdleMinTerminalWidth = std::size_t{176};
constexpr auto kSidebarMinTerminalHeight = std::size_t{16};

bool sidebar_drawer_active(ComposerSnapshot const& snapshot)
{
  return snapshot.sidebar_drawer_visible && snapshot.sidebar.has_value() && !snapshot.permission_prompt && !snapshot.question_prompt &&
         !snapshot.command_output && !snapshot.plugin_ui_modal && !snapshot.select_list && !snapshot.subagent_workspace;
}

bool todo_is_active(TodoItem const& item) noexcept
{
  return item.status == TodoStatus::Pending || item.status == TodoStatus::InProgress;
}

std::size_t count_todos_with_status(std::vector<TodoItem> const& todos, TodoStatus status) noexcept
{
  return static_cast<std::size_t>(std::ranges::count_if(todos, [status](TodoItem const& item) { return item.status == status; }));
}

bool sidebar_has_active_todos(SidebarSnapshot const& sidebar) noexcept
{
  return std::ranges::any_of(sidebar.todos, todo_is_active);
}

bool sidebar_has_actionable_data(SidebarSnapshot const& sidebar)
{
  auto const has_running_activity =
      std::ranges::any_of(sidebar.activity, [](SidebarActivityItem const& activity) { return activity.status == ToolTimelineStatus::Running; });
  return has_running_activity || !sidebar.modified_files.empty() || sidebar_has_active_todos(sidebar);
}

std::string todo_status_marker(TodoStatus status)
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

std::string_view todo_status_sgr(TodoStatus status)
{
  switch (status)
  {
    case TodoStatus::Pending:
      return kSgrDim;
    case TodoStatus::InProgress:
      return kSgrWarning;
    case TodoStatus::Completed:
      return kSgrSuccess;
  }
  return kSgrDim;
}

std::string todo_status_label(TodoStatus status)
{
  switch (status)
  {
    case TodoStatus::Pending:
      return "pending";
    case TodoStatus::InProgress:
      return "in progress";
    case TodoStatus::Completed:
      return "completed";
  }
  return "pending";
}

std::string format_todo_counts_header(std::vector<TodoItem> const& todos, bool active_only_title)
{
  auto const total = todos.size();
  auto const completed = count_todos_with_status(todos, TodoStatus::Completed);
  auto const in_progress = count_todos_with_status(todos, TodoStatus::InProgress);
  auto const pending = count_todos_with_status(todos, TodoStatus::Pending);
  auto const active = in_progress + pending;
  if (active_only_title)
    return "Todos — " + std::to_string(active) + " active";
  return "Todos — " + std::to_string(completed) + "/" + std::to_string(total) + " completed · " + std::to_string(in_progress) + " in progress · " +
         std::to_string(pending) + " pending";
}

std::string format_todo_rail_line(TodoItem const& item, std::size_t width)
{
  auto line = std::string(todo_status_sgr(item.status)) + todo_status_marker(item.status) + std::string(kSgrReset) + " #" + sanitize_terminal_text(item.id) +
              " " + sanitize_terminal_text(item.content);
  return detail::fit_line_preserving_sgr(std::move(line), width);
}

bool sidebar_visible(ComposerSnapshot const& snapshot, std::size_t width, std::size_t height)
{
  if (!snapshot.sidebar || snapshot.sidebar_drawer_visible || snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output ||
      snapshot.plugin_ui_modal || snapshot.select_list || snapshot.subagent_workspace || height < kSidebarMinTerminalHeight)
  {
    return false;
  }
  auto const minimum_width = sidebar_has_actionable_data(*snapshot.sidebar) ? kSidebarActionableMinTerminalWidth : kSidebarIdleMinTerminalWidth;
  return width >= minimum_width;
}

std::size_t main_width_for(ComposerSnapshot const& snapshot, std::size_t width)
{
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  if (!sidebar_visible(snapshot, width, height))
    return width;
  auto const sidebar_width = std::min<std::size_t>(kSidebarWidth, width / 3);
  return width > sidebar_width + 1 ? width - sidebar_width - 1 : width;
}

std::string status_marker(ToolTimelineStatus status)
{
  switch (status)
  {
    case ToolTimelineStatus::Running:
      return std::string(kSgrWarning) + "[~]" + std::string(kSgrReset);
    case ToolTimelineStatus::Success:
      return std::string(kSgrSuccess) + "[+]" + std::string(kSgrReset);
    case ToolTimelineStatus::Canceled:
      return std::string(kSgrDim) + "[-]" + std::string(kSgrReset);
    case ToolTimelineStatus::Error:
      return std::string(kSgrError) + "[x]" + std::string(kSgrReset);
  }
  return std::string(kSgrDim) + "[?]" + std::string(kSgrReset);
}

std::string_view drawer_status_marker(ToolTimelineStatus status)
{
  switch (status)
  {
    case ToolTimelineStatus::Running:
      return "[~]";
    case ToolTimelineStatus::Success:
      return "[+]";
    case ToolTimelineStatus::Canceled:
      return "[-]";
    case ToolTimelineStatus::Error:
      return "[x]";
  }
  return "[?]";
}

void push_sidebar_line(std::vector<std::string>& lines, std::string line, std::size_t width)
{
  lines.push_back(detail::fit_line_preserving_sgr("  " + std::move(line), width));
}

std::optional<std::string> percent_text_from_token_status(std::optional<std::string> const& token_status)
{
  if (!token_status || token_status->empty())
    return std::nullopt;
  auto const open = token_status->rfind('(');
  if (open == std::string::npos)
    return std::nullopt;
  auto const percent = token_status->find('%', open + 1);
  if (percent == std::string::npos || percent <= open + 1)
    return std::nullopt;
  return token_status->substr(open + 1, percent - open - 1);
}

double context_pressure_value(std::string const& percent_text)
{
  double value = 0.0;
  double scale = 1.0;
  bool fractional = false;
  bool parsed_digit = false;
  for (auto const ch : percent_text)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (std::isdigit(byte) != 0)
    {
      parsed_digit = true;
      auto const digit = static_cast<double>(byte - static_cast<unsigned char>('0'));
      if (fractional)
      {
        scale *= 10.0;
        value += digit / scale;
      }
      else
      {
        value = (value * 10.0) + digit;
      }
    }
    else if (ch == '.' && !fractional)
    {
      fractional = true;
    }
    else
    {
      break;
    }
  }
  return parsed_digit ? value : 0.0;
}

std::string context_pressure_level(std::string const& percent_text)
{
  auto const value = context_pressure_value(percent_text);
  return value >= 90.0 ? "critical" : value >= 70.0 ? "high" : value >= 40.0 ? "moderate" : "low";
}

std::string context_pressure_line(std::string const& percent_text)
{
  auto const value = context_pressure_value(percent_text);
  auto const color = value >= 70.0 ? kSgrWarning : kSgrDim;
  return std::string("context pressure ") + std::string(color) + context_pressure_level(percent_text) + " " + sanitize_terminal_text(percent_text) + "%" +
         std::string(kSgrReset);
}

void push_sidebar_drawer_value(std::vector<std::string>& lines, std::string_view label, std::string_view value, std::size_t width)
{
  auto const first_prefix = std::string("  ") + std::string(label) + " ";
  constexpr std::string_view continuation_prefix = "    ";
  auto const content_width = width > first_prefix.size() ? width - first_prefix.size() : std::size_t{1};
  auto wrapped = detail::wrap_transcript_text(value.empty() ? std::string_view("unknown") : value, content_width);
  if (wrapped.empty())
    wrapped.emplace_back("unknown");
  lines.push_back(detail::screen_surface_line(first_prefix + wrapped.front(), width));
  for (std::size_t index = 1; index < wrapped.size(); ++index)
  {
    lines.push_back(detail::screen_surface_line(std::string(continuation_prefix) + wrapped[index], width));
  }
}

void push_sidebar_drawer_section(std::vector<std::string>& lines, std::string_view title, std::size_t width)
{
  if (!lines.empty())
    lines.push_back(detail::screen_surface_line("", width));
  lines.push_back(detail::screen_surface_line(std::string("  ") + std::string(kSgrBold) + sanitize_terminal_text(title) + std::string(kSgrReset), width));
}

std::vector<std::string> render_sidebar_drawer_body(SidebarSnapshot const& sidebar, std::size_t width)
{
  std::vector<std::string> lines;
  push_sidebar_drawer_section(lines, "Activity", width);
  if (sidebar.activity.empty())
  {
    push_sidebar_drawer_value(lines, "activity", "idle", width);
  }
  else
  {
    for (auto const& activity : sidebar.activity)
    {
      auto value = std::string(drawer_status_marker(activity.status)) + " " + sanitize_terminal_text(activity.label);
      if (!activity.detail.empty())
        value += " " + sanitize_terminal_text(activity.detail);
      push_sidebar_drawer_value(lines, "activity", value, width);
    }
  }

  push_sidebar_drawer_section(lines, "Modified Files", width);
  if (sidebar.modified_files.empty())
  {
    push_sidebar_drawer_value(lines, "files", "no file changes yet", width);
  }
  else
  {
    for (auto const& file : sidebar.modified_files)
    {
      auto value = sanitize_terminal_text(file.path);
      if (file.added || file.removed)
      {
        if (file.added)
          value += " +" + std::to_string(*file.added);
        if (file.removed)
          value += " -" + std::to_string(*file.removed);
      }
      else
      {
        value += " changed";
      }
      push_sidebar_drawer_value(lines, "file", value, width);
    }
  }

  push_sidebar_drawer_section(lines, "Todos", width);
  if (sidebar.todos.empty())
  {
    push_sidebar_drawer_value(lines, "todos", "no todos", width);
  }
  else
  {
    push_sidebar_drawer_value(lines, "summary", format_todo_counts_header(sidebar.todos, false), width);
    for (auto const& item : sidebar.todos)
    {
      auto value = todo_status_marker(item.status) + " #" + sanitize_terminal_text(item.id) + " [" + todo_status_label(item.status) + "] " +
                   sanitize_terminal_text(item.content);
      push_sidebar_drawer_value(lines, "todo", value, width);
    }
  }

  push_sidebar_drawer_section(lines, "Session", width);
  push_sidebar_drawer_value(lines, "mode", sanitize_terminal_text(sidebar.mode), width);
  push_sidebar_drawer_value(lines, "provider", sanitize_terminal_text(sidebar.provider), width);
  push_sidebar_drawer_value(lines, "model", sanitize_terminal_text(sidebar.model), width);
  push_sidebar_drawer_value(lines, "session", sanitize_terminal_text(sidebar.session_id), width);
  push_sidebar_drawer_value(lines, "path", sanitize_terminal_text(sidebar.session_path), width);
  push_sidebar_drawer_value(lines, "entries", sidebar.session_entry_count ? std::to_string(*sidebar.session_entry_count) : std::string("unknown"), width);
  push_sidebar_drawer_value(lines, "workspace", sanitize_terminal_text(sidebar.workspace), width);
  push_sidebar_drawer_value(lines, "branch", sanitize_terminal_text(sidebar.git_branch), width);
  push_sidebar_drawer_value(lines, "reasoning", sidebar.reasoning_status ? sanitize_terminal_text(*sidebar.reasoning_status) : std::string("unknown"), width);
  push_sidebar_drawer_value(lines, "usage", sidebar.token_status ? sanitize_terminal_text(*sidebar.token_status) : std::string("tokens unknown"), width);
  if (auto const percent = percent_text_from_token_status(sidebar.token_status))
  {
    push_sidebar_drawer_value(lines, "context pressure", context_pressure_level(*percent) + " " + sanitize_terminal_text(*percent) + "%", width);
  }
  else
  {
    push_sidebar_drawer_value(lines, "context", "pressure unknown", width);
  }
  push_sidebar_drawer_value(lines, "context sources",
                            sidebar.context_source_count.has_value() ? std::to_string(*sidebar.context_source_count) : std::string("unknown"), width);
  push_sidebar_drawer_value(lines, "version", sidebar.version.empty() ? std::string("unknown") : std::string("AVA ") + sanitize_terminal_text(sidebar.version),
                            width);
  return lines;
}

std::size_t sidebar_drawer_body_height(ComposerSnapshot const& snapshot, std::size_t width, std::size_t height)
{
  auto const composer_lines = detail::composer_block_line_count(snapshot, height, width);
  auto const drawer_height = height > composer_lines ? height - composer_lines : std::size_t{0};
  if (drawer_height == 0)
    return 0;
  auto const footer_lines = drawer_height >= 3 ? std::size_t{1} : std::size_t{0};
  return drawer_height > 1 + footer_lines ? drawer_height - 1 - footer_lines : std::size_t{0};
}

ComposerFrame render_sidebar_drawer(ComposerSnapshot const& snapshot, std::size_t width, std::size_t height)
{
  ComposerFrame frame;
  auto const composer_line_count = detail::composer_block_line_count(snapshot, height, width);
  auto const drawer_height = height > composer_line_count ? height - composer_line_count : std::size_t{0};
  auto const body_height = sidebar_drawer_body_height(snapshot, width, height);
  auto const body = render_sidebar_drawer_body(*snapshot.sidebar, width);
  auto const max_offset = body.size() > body_height ? body.size() - body_height : std::size_t{0};
  auto const offset = std::min(snapshot.sidebar_drawer_scroll_offset, max_offset);

  frame.lines.reserve(height);
  if (drawer_height > 0)
  {
    frame.lines.push_back(
        detail::screen_surface_line(std::string("  ") + std::string(kSgrBold) + std::string(kSgrAccent) + "Session overview" + std::string(kSgrReset), width));
  }
  for (std::size_t index = 0; index < body_height; ++index)
  {
    auto const body_index = offset + index;
    frame.lines.push_back(body_index < body.size() ? body[body_index] : detail::screen_surface_line("", width));
  }
  while (frame.lines.size() + (drawer_height >= 3 ? 1 : 0) < drawer_height)
    frame.lines.push_back(detail::screen_surface_line("", width));
  if (drawer_height >= 3)
  {
    frame.lines.push_back(
        detail::screen_surface_line(std::string("  ") + std::string(kSgrDim) + "PgUp/PgDn scroll · Home/End jump · Esc close" + std::string(kSgrReset), width));
  }
  auto const composer_lines = detail::render_composer_block(snapshot, width, composer_line_count);
  frame.lines.insert(frame.lines.end(), composer_lines.begin(), composer_lines.end());
  while (frame.lines.size() < height)
    frame.lines.push_back(detail::screen_surface_line("", width));
  return frame;
}

std::string normalized_sidebar_value(std::string_view value)
{
  auto const first = std::ranges::find_if_not(value, [](unsigned char character) { return std::isspace(character); });
  auto const last = std::ranges::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) { return std::isspace(character); }).base();
  if (first >= last)
    return {};
  std::string normalized(first, last);
  std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return normalized;
}

std::vector<std::string> render_sidebar(SidebarSnapshot const& sidebar, std::size_t width, std::size_t height)
{
  std::vector<std::string> lines;
  lines.reserve(height);

  auto const known_value = [](std::string_view value) {
    auto const normalized = normalized_sidebar_value(value);
    return !normalized.empty() && normalized != "unknown";
  };
  auto const known_token_value = [&](std::string_view value) {
    auto const normalized = normalized_sidebar_value(value);
    return known_value(value) && normalized != "tokens unknown";
  };
  auto begin_group = [&](std::string_view title) {
    if (!lines.empty())
      push_sidebar_line(lines, "", width);
    push_sidebar_line(lines, std::string(kSgrBold) + sanitize_terminal_text(title) + std::string(kSgrReset), width);
  };

  if (sidebar_has_active_todos(sidebar) && lines.size() < height)
  {
    begin_group(format_todo_counts_header(sidebar.todos, true));
    for (auto const& item : sidebar.todos)
    {
      if (!todo_is_active(item))
        continue;
      push_sidebar_line(lines, format_todo_rail_line(item, width), width);
      if (lines.size() >= height)
        return lines;
    }
  }

  begin_group("Session");
  std::string metadata;
  if (known_value(sidebar.mode))
    metadata = sanitize_terminal_text(sidebar.mode);
  std::string provider_model;
  if (known_value(sidebar.provider))
    provider_model = sanitize_terminal_text(sidebar.provider);
  if (known_value(sidebar.model))
  {
    if (!provider_model.empty())
      provider_model += "/";
    provider_model += sanitize_terminal_text(sidebar.model);
  }
  if (!provider_model.empty())
  {
    if (!metadata.empty())
      metadata += " · ";
    metadata += provider_model;
  }
  if (!metadata.empty())
    push_sidebar_line(lines, std::string(kSgrDim) + metadata + std::string(kSgrReset), width);

  auto const has_running_activity =
      std::ranges::any_of(sidebar.activity, [](SidebarActivityItem const& activity) { return activity.status == ToolTimelineStatus::Running; });
  if (has_running_activity && lines.size() < height)
  {
    begin_group("Activity");
    for (auto const& activity : sidebar.activity)
    {
      if (activity.status != ToolTimelineStatus::Running)
        continue;
      auto line = status_marker(activity.status) + " " + sanitize_terminal_text(activity.label);
      if (!activity.detail.empty())
        line += " " + std::string(kSgrDim) + sanitize_terminal_text(activity.detail) + std::string(kSgrReset);
      push_sidebar_line(lines, std::move(line), width);
      if (lines.size() >= height)
        return lines;
    }
  }

  if (!sidebar.modified_files.empty() && lines.size() < height)
  {
    begin_group("Modified Files");
    for (auto const& file : sidebar.modified_files)
    {
      auto line = sanitize_terminal_text(file.path);
      if (file.added || file.removed)
      {
        line += " ";
        if (file.added)
          line += std::string(kSgrSuccess) + "+" + std::to_string(*file.added) + std::string(kSgrReset);
        if (file.removed)
          line += " " + std::string(kSgrError) + "-" + std::to_string(*file.removed) + std::string(kSgrReset);
      }
      else
      {
        line += " " + std::string(kSgrDim) + "changed" + std::string(kSgrReset);
      }
      push_sidebar_line(lines, std::move(line), width);
      if (lines.size() >= height)
        return lines;
    }
  }

  auto const known_branch = known_value(sidebar.git_branch);
  auto const known_reasoning = sidebar.reasoning_status && known_value(*sidebar.reasoning_status);
  auto const known_token_status = sidebar.token_status && known_token_value(*sidebar.token_status);
  auto const has_context = known_branch || known_reasoning || known_token_status || sidebar.context_source_count.has_value();
  if (has_context && lines.size() < height)
  {
    begin_group("Context");
    if (known_branch)
      push_sidebar_line(lines, "branch " + sanitize_terminal_text(sidebar.git_branch), width);
    if (known_reasoning)
      push_sidebar_line(lines, "reasoning " + sanitize_terminal_text(*sidebar.reasoning_status), width);
    if (known_token_status)
    {
      push_sidebar_line(lines, "usage " + sanitize_terminal_text(*sidebar.token_status), width);
      if (auto const percent = percent_text_from_token_status(sidebar.token_status))
        push_sidebar_line(lines, context_pressure_line(*percent), width);
    }
    if (sidebar.context_source_count.has_value())
      push_sidebar_line(lines, "context sources " + std::to_string(*sidebar.context_source_count), width);
  }

  while (lines.size() < height)
    lines.emplace_back();
  return lines;
}

ComposerFrame render_composer_main_frame(ComposerSnapshot snapshot, std::size_t width, std::size_t height, detail::CompletionMatchCache& completion_cache,
                                         std::size_t source_revision, detail::TranscriptLayoutCache* transcript_cache, std::size_t transcript_generation,
                                         bool freeze_transcript_layout, bool allow_frozen_width_mismatch)
{
  snapshot.width = width;
  snapshot.height = height;
  if (!snapshot.context_source_count && snapshot.sidebar)
    snapshot.context_source_count = snapshot.sidebar->context_source_count;
  // The automatic rail owns the status area, so its main frame must not retain
  // one-action feedback after removing the sidebar for recursive rendering.
  snapshot.reasoning_feedback.reset();
  snapshot.local_command_feedback.reset();
  snapshot.sidebar = std::nullopt;
  return detail::render_composer_frame_cached(snapshot, completion_cache, source_revision, transcript_cache, transcript_generation, false, true,
                                              freeze_transcript_layout, allow_frozen_width_mismatch);
}

std::string pad_line_to_width(std::string line, std::size_t width)
{
  auto fitted = detail::fit_line_preserving_sgr(std::move(line), width);
  auto const columns = detail::terminal_text_columns(fitted);
  if (columns < width)
    fitted.append(width - columns, ' ');
  return fitted;
}

std::size_t modal_width_for(std::size_t width)
{
  if (width < 48)
    return width;
  return std::min<std::size_t>(72, std::max<std::size_t>(44, (width * 4) / 5));
}

std::size_t plugin_ui_modal_width_for(std::size_t width)
{
  // The host owns this cap. It is wide enough for the maximum canonical
  // plugin id while preventing a plugin modal from claiming an entire wide
  // terminal.
  return std::min<std::size_t>(160, width);
}

std::size_t modal_height_for(std::size_t height)
{
  if (height < 10)
    return height;
  auto const inset_height = height > 4 ? height - 4 : height;
  return std::min<std::size_t>(22, std::max(detail::kMinHeight, inset_height));
}

std::size_t select_modal_height_for(std::size_t height)
{
  if (height < 10)
    return height;
  return height > 4 ? height - 4 : height;
}

std::size_t plugin_ui_modal_height_for(std::size_t height)
{
  // Short terminals need every row for nontruncating attribution, one visible
  // choice, and host controls. Roomier terminals retain the normal inset.
  if (height <= 12)
    return height;
  return std::min<std::size_t>(22, height > 4 ? height - 4 : height);
}

std::vector<std::string> overlay_question_modal(std::vector<std::string> lines, QuestionPromptView const& prompt, std::size_t width, std::size_t height)
{
  while (lines.size() < height)
    lines.emplace_back();
  auto const modal_width = std::min(modal_width_for(width), width);
  auto const modal_height = std::min(modal_height_for(height), height);
  auto const modal_lines = detail::render_question_modal(prompt, modal_width, modal_height);
  auto const top = height > modal_lines.size() ? (height - modal_lines.size()) / 2 : std::size_t{0};
  auto const left = width > modal_width ? (width - modal_width) / 2 : std::size_t{0};
  auto const right = width > left + modal_width ? width - left - modal_width : std::size_t{0};
  for (std::size_t index = 0; index < modal_lines.size() && top + index < lines.size(); ++index)
  {
    lines[top + index] = std::string(left, ' ') + modal_lines[index] + std::string(right, ' ');
  }
  return lines;
}

std::vector<std::string> overlay_select_list_modal(std::vector<std::string> lines, SelectListView const& view, std::size_t width, std::size_t height)
{
  while (lines.size() < height)
    lines.emplace_back();
  auto const modal_width = std::min(modal_width_for(width), width);
  auto const modal_height = std::min(select_modal_height_for(height), height);
  auto const modal_lines = detail::render_select_list_modal(view, modal_width, modal_height);
  auto const top = height > modal_lines.size() ? (height - modal_lines.size()) / 2 : std::size_t{0};
  auto const left = width > modal_width ? (width - modal_width) / 2 : std::size_t{0};
  auto const right = width > left + modal_width ? width - left - modal_width : std::size_t{0};
  for (std::size_t index = 0; index < modal_lines.size() && top + index < lines.size(); ++index)
  {
    lines[top + index] = std::string(left, ' ') + modal_lines[index] + std::string(right, ' ');
  }
  return lines;
}

std::vector<std::string> overlay_command_output_modal(std::vector<std::string> lines, CommandOutputView const& view, std::size_t width, std::size_t height,
                                                      ToolPresentation presentation)
{
  while (lines.size() < height)
    lines.emplace_back();
  auto const geometry = command_output_geometry(width, height);
  auto const modal_width = geometry.width;
  auto const modal_height = geometry.height;
  auto const modal_lines = detail::render_command_output_modal(view, modal_width, modal_height, presentation);
  auto const top = height > modal_lines.size() ? (height - modal_lines.size()) / 2 : std::size_t{0};
  auto const left = width > modal_width ? (width - modal_width) / 2 : std::size_t{0};
  auto const right = width > left + modal_width ? width - left - modal_width : std::size_t{0};
  for (std::size_t index = 0; index < modal_lines.size() && top + index < lines.size(); ++index)
  {
    lines[top + index] = std::string(left, ' ') + modal_lines[index] + std::string(right, ' ');
  }
  return lines;
}

std::vector<std::string> overlay_plugin_ui_modal(std::vector<std::string> lines, TuiPluginUiModalView const& view, std::size_t width, std::size_t height)
{
  while (lines.size() < height)
    lines.emplace_back();
  auto const modal_width = std::min(plugin_ui_modal_width_for(width), width);
  auto const modal_height = std::min(plugin_ui_modal_height_for(height), height);
  auto const modal_lines = detail::render_plugin_ui_modal(view, modal_width, modal_height);
  auto const top = height > modal_lines.size() ? (height - modal_lines.size()) / 2 : std::size_t{0};
  auto const left = width > modal_width ? (width - modal_width) / 2 : std::size_t{0};
  auto const right = width > left + modal_width ? width - left - modal_width : std::size_t{0};
  for (std::size_t index = 0; index < modal_lines.size() && top + index < lines.size(); ++index)
  {
    lines[top + index] = std::string(left, ' ') + modal_lines[index] + std::string(right, ' ');
  }
  return lines;
}

std::string strip_provider_body_suffix(std::string_view detail)
{
  auto const separator = detail.find(" - ");
  if (separator == std::string_view::npos)
    return std::string(detail);
  return std::string(detail.substr(0, separator));
}

std::optional<std::string> extract_detail_token(std::string_view detail, std::string_view key)
{
  auto const pos = detail.find(key);
  if (pos == std::string_view::npos)
    return std::nullopt;
  auto value_begin = pos + key.size();
  auto value_end = value_begin;
  while (value_end < detail.size())
  {
    auto const ch = static_cast<unsigned char>(detail[value_end]);
    if (std::isspace(ch) != 0)
      break;
    ++value_end;
  }
  if (value_end == value_begin)
    return std::nullopt;
  return std::string(detail.substr(value_begin, value_end - value_begin));
}

// Allowlisted RUNNING retry/compaction lifecycle only. Scans once and keeps the newest match.
std::optional<std::string> compact_retry_compaction_status(ComposerSnapshot const& snapshot)
{
  if (!snapshot.sidebar)
    return std::nullopt;

  SidebarActivityItem const* chosen = nullptr;
  for (auto const& activity : snapshot.sidebar->activity)
  {
    if (activity.status != ToolTimelineStatus::Running)
      continue;
    if (activity.label != "retry" && activity.label != "compaction")
      continue;
    chosen = &activity;
  }
  if (chosen == nullptr)
    return std::nullopt;

  if (chosen->label == "compaction")
    return std::string("compaction");

  auto const safe_detail = sanitize_terminal_text(strip_provider_body_suffix(chosen->detail));
  if (safe_detail.starts_with("retry countdown"))
  {
    if (auto remaining = extract_detail_token(safe_detail, "remaining="))
      return std::string("retry ") + sanitize_terminal_text(*remaining);
    return std::string("retry countdown");
  }

  std::string text = "retry";
  if (auto attempt = extract_detail_token(safe_detail, "attempt "))
    text += " attempt " + sanitize_terminal_text(*attempt);
  return text;
}

void append_hint_segment(std::string& text, std::string segment)
{
  if (segment.empty())
    return;
  if (!text.empty())
    text += " · ";
  text += std::move(segment);
}

std::string configured_interrupt_hint(ActiveRunHint const& hint)
{
  if (hint.interrupt.empty())
    return "stop unbound";
  return sanitize_terminal_text(hint.interrupt) + " stop";
}

std::string detached_new_output_hint(ComposerSnapshot const& snapshot)
{
  if (snapshot.transcript_scroll_offset == 0 || snapshot.transcript_new_output_count == 0)
    return {};
  auto text = std::to_string(snapshot.transcript_new_output_count) + " new";
  if (!snapshot.active_run_hint.jump_to_bottom.empty())
    text += " · " + sanitize_terminal_text(snapshot.active_run_hint.jump_to_bottom);
  else
    text += " · jump unbound";
  return text;
}

std::vector<std::string> render_active_run_hint_lines(ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completion_cache, std::size_t width,
                                                      std::size_t max_lines)
{
  auto const completion_visible = completion_cache.model && completion_cache.model->palette_visible;
  auto const detached_hint = detached_new_output_hint(snapshot);
  auto const suppress_chrome = snapshot.permission_prompt || snapshot.question_prompt || snapshot.plugin_ui_modal || snapshot.select_list ||
                               snapshot.subagent_workspace || snapshot.sidebar_drawer_visible ||
                               slash_palette_visible(snapshot.input, snapshot.input_cursor, snapshot.slash_commands) || completion_visible;
  if (max_lines == 0 || suppress_chrome || (!snapshot.processing && detached_hint.empty()))
    return {};

  std::string text;
  if (snapshot.processing)
  {
    append_hint_segment(text, configured_interrupt_hint(snapshot.active_run_hint));
    // Compute lifecycle once per render; prefer it over generic queue/follow-up copy.
    auto status = compact_retry_compaction_status(snapshot);
    if (status)
    {
      append_hint_segment(text, std::move(*status));
    }
    else if (snapshot.input.empty())
    {
      append_hint_segment(text, "type a follow-up");
    }
    else
    {
      if (!snapshot.active_run_hint.submit_or_queue.empty())
        append_hint_segment(text, sanitize_terminal_text(snapshot.active_run_hint.submit_or_queue) + " queue");
      if (!snapshot.active_run_hint.follow_up.empty())
        append_hint_segment(text, sanitize_terminal_text(snapshot.active_run_hint.follow_up) + " follow-up");
      if (!snapshot.active_run_hint.dequeue.empty())
        append_hint_segment(text, sanitize_terminal_text(snapshot.active_run_hint.dequeue) + " /restore");
    }
  }
  append_hint_segment(text, detached_hint);
  if (text.empty())
    return {};
  return {detail::screen_surface_line(detail::composer_gutter() + std::string(kSgrDim) + text + std::string(kSgrReset), width)};
}

std::vector<std::string> render_empty_transcript_discovery_lines(ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completion_cache,
                                                                 std::size_t width, std::size_t transcript_height)
{
  auto const completion_visible = completion_cache.model && completion_cache.model->palette_visible;
  if (transcript_height < 2 || snapshot.width < 80 || snapshot.height < 24 || snapshot.processing || !snapshot.transcript.empty() || !snapshot.input.empty() ||
      snapshot.permission_prompt || snapshot.question_prompt || snapshot.plugin_ui_modal || snapshot.select_list || snapshot.subagent_workspace ||
      snapshot.sidebar_drawer_visible || !snapshot.pending_attachments.empty() ||
      slash_palette_visible(snapshot.input, snapshot.input_cursor, snapshot.slash_commands) || completion_visible)
  {
    return {};
  }

  std::vector<std::string> lines;
  lines.push_back(detail::screen_surface_line(std::string(kSgrDim) + "/ commands · @ files" + std::string(kSgrReset), width));
  lines.push_back(detail::screen_surface_line(std::string(kSgrDim) + "/help · /hotkeys" + std::string(kSgrReset), width));
  return lines;
}

std::vector<std::string> render_queued_message_lines(ComposerSnapshot const& snapshot, std::size_t width, std::size_t max_lines)
{
  std::vector<std::string> lines;
  if (max_lines == 0 || snapshot.queued_messages.empty())
    return lines;

  auto const visible_count = std::min(snapshot.queued_messages.size(), max_lines);
  lines.reserve(visible_count);
  auto const start = snapshot.queued_messages.size() - visible_count;
  for (std::size_t index = start; index < snapshot.queued_messages.size(); ++index)
  {
    auto const& item = snapshot.queued_messages[index];
    auto line = std::string(kSgrDim) + "queued " + sanitize_terminal_text(item.kind) + std::string(kSgrReset) + " " + sanitize_terminal_text(item.text);
    if (index == snapshot.queued_messages.size() - 1)
    {
      auto const dequeue = snapshot.active_run_hint.dequeue.empty() ? std::string{} : sanitize_terminal_text(snapshot.active_run_hint.dequeue) + " ";
      line += " " + std::string(kSgrDim) + "(/restore or " + dequeue + "dequeue latest)" + std::string(kSgrReset);
    }
    lines.push_back(detail::screen_surface_line(std::move(line), width));
  }
  if (start > 0 && !lines.empty())
  {
    lines.front() = detail::screen_surface_line(std::string(kSgrDim) + "queued +" + std::to_string(start) + " more" + std::string(kSgrReset), width);
  }
  return lines;
}

struct PendingAttachmentRender
{
  std::vector<std::string> lines;
  std::vector<TerminalGraphicOverlay> graphics;
};

std::optional<ImageCellSize> pending_attachment_graphic_size(PendingAttachmentItem const& item, std::size_t width, std::size_t max_rows,
                                                             std::size_t image_width_cells)
{
  if (!item.preview || item.preview->protocol == TerminalImageProtocol::None || !item.preview->base64_data || item.preview->base64_data->empty() ||
      max_rows == 0)
  {
    return std::nullopt;
  }
  // image_width_cells comes from the application-owned display document and is clamped again to
  // the current content viewport. Intrinsic aspect ratio still uses calculate_image_cell_size.
  auto const configured_width = std::max<std::size_t>(1, image_width_cells);
  auto const max_width = width > 4 ? std::min(configured_width, width - 4) : std::size_t{1};
  // TODO: Pass measured terminal cell pixel dimensions here once the TUI runtime has a safe
  // terminal-query seam. Until then image previews use calculate_image_cell_size's deterministic
  // fallback cell dimensions, so sizing remains bounded but not terminal-specific.
  return calculate_image_cell_size(item.preview->dimensions, max_width, max_rows);
}

TerminalGraphicOverlay pending_attachment_graphic(PendingAttachmentItem const& item, std::size_t width, ImageCellSize cells)
{
  std::string sequence;
  switch (item.preview->protocol)
  {
    case TerminalImageProtocol::Kitty: {
      KittyImageOptions options;
      options.columns = cells.columns;
      options.rows = cells.rows;
      options.image_id = item.preview->image_id;
      options.move_cursor = false;
      sequence = encode_kitty_image(*item.preview->base64_data, options);
      break;
    }
    case TerminalImageProtocol::Iterm2: {
      Iterm2ImageOptions options;
      options.width = std::to_string(cells.columns);
      options.height = std::to_string(cells.rows);
      options.name = item.label;
      sequence = encode_iterm2_image(*item.preview->base64_data, options);
      break;
    }
    case TerminalImageProtocol::None:
      break;
  }
  return TerminalGraphicOverlay{.protocol = item.preview->protocol,
                                .row = 0,
                                .column = width > 4 ? std::size_t{2} : std::size_t{0},
                                .rows = cells.rows,
                                .columns = cells.columns,
                                .image_id = item.preview->protocol == TerminalImageProtocol::Kitty ? item.preview->image_id : std::nullopt,
                                .sequence = std::move(sequence)};
}

PendingAttachmentRender render_pending_attachment_lines(ComposerSnapshot const& snapshot, std::size_t width, std::size_t max_lines, bool emit_graphics)
{
  PendingAttachmentRender render;
  if (max_lines == 0 || snapshot.pending_attachments.empty())
    return render;

  // Reserve preview rows whenever images are enabled so layout/line-count stays stable even when
  // the caller only wants text measurement. Emit protocol bytes only when requested.
  auto const reserve_preview_rows = snapshot.show_images;
  auto const visible_count = std::min(snapshot.pending_attachments.size(), max_lines);
  render.lines.reserve(max_lines);
  auto const start = snapshot.pending_attachments.size() - visible_count;
  for (std::size_t index = start; index < snapshot.pending_attachments.size(); ++index)
  {
    if (render.lines.size() >= max_lines)
      break;
    auto const& item = snapshot.pending_attachments[index];
    auto line = std::string(kSgrDim) + "attached image" + std::string(kSgrReset) + " " + sanitize_terminal_text(item.label);
    if (!item.detail.empty())
    {
      line += " " + std::string(kSgrDim) + item.detail + std::string(kSgrReset);
    }
    if (index == snapshot.pending_attachments.size() - 1)
    {
      line += " " + std::string(kSgrDim) + "(next prompt)" + std::string(kSgrReset);
    }
    render.lines.push_back(detail::screen_surface_line(std::move(line), width));
    if (reserve_preview_rows && index == snapshot.pending_attachments.size() - 1 && render.lines.size() < max_lines)
    {
      auto const graphic_size = pending_attachment_graphic_size(item, width, max_lines - render.lines.size(), snapshot.image_width_cells);
      if (graphic_size)
      {
        auto const graphic_row = render.lines.size();
        for (std::size_t row = 0; row < graphic_size->rows; ++row)
        {
          render.lines.push_back(detail::screen_surface_line("", width));
        }
        if (emit_graphics)
        {
          auto graphic = pending_attachment_graphic(item, width, *graphic_size);
          graphic.row = graphic_row;
          render.graphics.push_back(std::move(graphic));
        }
      }
    }
  }
  if (start > 0 && !render.lines.empty())
  {
    render.lines.front() =
        detail::screen_surface_line(std::string(kSgrDim) + "attached +" + std::to_string(start) + " more images" + std::string(kSgrReset), width);
  }
  return render;
}

bool status_is_alert(std::string_view status)
{
  constexpr std::array kErrorPrefixes = {"invalid_argument:",
                                         "io:",
                                         "not_found:",
                                         "permission_denied:",
                                         "provider:",
                                         "session:",
                                         "tool:",
                                         "unknown:",
                                         "command disabled:",
                                         "reference disabled:",
                                         "path disabled:",
                                         "selection too large to copy",
                                         "clipboard copy failed",
                                         "thinking mode unavailable for current model",
                                         "thinking mode can be changed between turns"};
  return std::ranges::any_of(kErrorPrefixes, [status](std::string_view prefix) { return status.starts_with(prefix); });
}

std::vector<std::string> render_status_alert_lines(std::string_view status, std::size_t width, std::size_t max_lines)
{
  std::vector<std::string> lines;
  auto const alert = status_is_alert(status);
  auto const selection_copy_request_sent = status == "selection copy request sent";
  if (max_lines == 0 || status.empty() || (!alert && !selection_copy_request_sent))
    return lines;

  auto const parts = split_lines(status);
  auto const visible_count = std::min(parts.size(), max_lines);
  lines.reserve(visible_count);
  for (std::size_t index = 0; index < visible_count; ++index)
  {
    auto line = std::string(index == 0 ? (alert ? "! " : "✓ ") : "  ") + sanitize_terminal_text(parts[index]);
    if (index + 1 == visible_count && parts.size() > visible_count)
      line += " ...";
    line = std::string(index == 0 ? (alert ? kSgrError : kSgrSuccess) : kSgrDim) + line + std::string(kSgrReset);
    lines.push_back(detail::fit_line_preserving_sgr(std::move(line), width));
  }
  return lines;
}

struct ComposerVerticalLayout
{
  std::size_t transcript_height = 0;
  std::size_t transcript_composer_gap_lines = 0;
};

// Align main-column geometry with render behavior. Rail visibility must be decided
// from the original full-width snapshot before reducing to content width.
// - When the automatic rail is visible, clear sidebar/todo dock exactly like
//   render_composer_main_frame (and drop one-action reasoning feedback).
// - When the rail is not visible, keep only todos so the sticky narrow dock still
//   reserves space without re-enabling automatic-rail side effects.
void prepare_main_column_geometry_snapshot(ComposerSnapshot& snapshot, bool rail_visible)
{
  if (rail_visible)
  {
    snapshot.reasoning_feedback.reset();
    snapshot.local_command_feedback.reset();
    snapshot.sidebar = std::nullopt;
    return;
  }
  auto todos = snapshot.sidebar ? snapshot.sidebar->todos : std::vector<TodoItem>{};
  snapshot.sidebar = SidebarSnapshot{.todos = std::move(todos)};
}

bool todo_dock_suppressed(ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completion_cache, std::size_t width, std::size_t height)
{
  auto const completion_visible = completion_cache.model && completion_cache.model->palette_visible;
  if (snapshot.permission_prompt || snapshot.question_prompt || snapshot.select_list || snapshot.subagent_workspace || snapshot.sidebar_drawer_visible ||
      completion_visible || slash_palette_visible(snapshot.input, snapshot.input_cursor, snapshot.slash_commands))
  {
    return true;
  }
  if (!snapshot.sidebar || !sidebar_has_active_todos(*snapshot.sidebar))
    return true;
  // Only when the automatic rail is not visible. Explicit /sidebar drawer already suppresses above.
  if (sidebar_visible(snapshot, width, height))
    return true;
  return false;
}

std::size_t todo_dock_line_budget(ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completion_cache, std::size_t width, std::size_t height,
                                  std::size_t reserved_non_dock_lines)
{
  if (todo_dock_suppressed(snapshot, completion_cache, width, height))
    return 0;
  if (height <= reserved_non_dock_lines)
    return 0;
  auto const available = height - reserved_non_dock_lines;
  // Keep room for transcript (>=1) and the composer block itself.
  if (available < 3)
    return 0;
  auto const max_dock = std::min<std::size_t>(available - 2, 6);
  if (max_dock == 0)
    return 0;
  if (height < 12)
    return std::min<std::size_t>(max_dock, 2);
  return max_dock;
}

std::vector<std::string> render_todo_dock_lines(SidebarSnapshot const& sidebar, std::size_t width, std::size_t max_lines)
{
  std::vector<std::string> lines;
  if (max_lines == 0 || !sidebar_has_active_todos(sidebar))
    return lines;

  auto const header = format_todo_counts_header(sidebar.todos, false);
  lines.push_back(detail::screen_surface_line(std::string(kSgrBold) + sanitize_terminal_text(header) + std::string(kSgrReset), width));
  if (max_lines == 1)
    return lines;

  std::vector<TodoItem const*> active;
  active.reserve(sidebar.todos.size());
  for (auto const& item : sidebar.todos)
  {
    if (todo_is_active(item))
      active.push_back(&item);
  }
  if (active.empty())
    return lines;

  auto const body_budget = max_lines - 1;
  auto const collapse_to_current = body_budget <= 1 || (body_budget == 2 && active.size() > 2);
  if (collapse_to_current)
  {
    // Prefer the in_progress item when collapsing, else the first active item.
    TodoItem const* current = active.front();
    for (auto const* item : active)
    {
      if (item->status == TodoStatus::InProgress)
      {
        current = item;
        break;
      }
    }
    lines.push_back(detail::screen_surface_line(format_todo_rail_line(*current, width), width));
    return lines;
  }

  auto const visible = std::min(active.size(), body_budget);
  auto const overflow = active.size() > visible;
  auto const item_slots = overflow && visible > 0 ? visible - 1 : visible;
  for (std::size_t index = 0; index < item_slots; ++index)
    lines.push_back(detail::screen_surface_line(format_todo_rail_line(*active[index], width), width));
  if (overflow)
  {
    auto const remaining = active.size() - item_slots;
    lines.push_back(detail::screen_surface_line(std::string(kSgrDim) + "+" + std::to_string(remaining) + " more" + std::string(kSgrReset), width));
  }
  return lines;
}

ComposerVerticalLayout calculate_composer_vertical_layout(ComposerSnapshot const& snapshot, detail::CompletionMatchCache& completion_cache, std::size_t width,
                                                          std::size_t height, std::size_t& normal_composer_lines, std::vector<std::string>& permission_lines,
                                                          std::vector<std::string>& question_lines, std::vector<std::string>& status_alert_lines,
                                                          std::vector<std::string>& palette_lines, std::vector<std::string>& queued_lines,
                                                          PendingAttachmentRender& pending_attachment_lines, std::vector<std::string>& todo_dock_lines,
                                                          bool emit_attachment_graphics, bool allow_transcript_gap = true,
                                                          std::size_t* plugin_line_budget = nullptr)
{
  // Startup overview is on-demand only (exact /overview); nothing reserves leading rows.
  auto const content_height = height;
  auto const prompt_active = snapshot.permission_prompt.has_value() || snapshot.question_prompt.has_value();
  auto const desired_composer_lines = detail::composer_block_line_count(snapshot, content_height, width);
  constexpr auto kMinimumPromptLines = std::size_t{4};
  auto const prompt_reserve = prompt_active ? std::min(kMinimumPromptLines, content_height) : std::size_t{0};
  normal_composer_lines = prompt_active ? std::min(desired_composer_lines, content_height - prompt_reserve) : desired_composer_lines;
  auto const max_prompt_lines = content_height > normal_composer_lines ? content_height - normal_composer_lines : 0;
  auto const prompt_line_limit = snapshot.permission_prompt && !snapshot.permission_prompt->diff_preview.empty() ? std::size_t{12} : std::size_t{7};
  auto const prompt_line_budget = prompt_active ? std::min(prompt_line_limit, max_prompt_lines) : 0;
  permission_lines =
      snapshot.permission_prompt ? detail::render_permission_prompt(*snapshot.permission_prompt, width, prompt_line_budget) : std::vector<std::string>{};
  question_lines = snapshot.question_prompt ? detail::render_question_prompt(*snapshot.question_prompt, width, prompt_line_budget) : std::vector<std::string>{};
  auto const fixed_and_prompt_lines = normal_composer_lines + permission_lines.size() + question_lines.size();
  auto const status_alert_line_budget =
      (content_height > fixed_and_prompt_lines && !prompt_active) ? std::min<std::size_t>(4, content_height - fixed_and_prompt_lines) : 0;
  auto const alerts = render_status_alert_lines(snapshot.status, width, std::min<std::size_t>(3, status_alert_line_budget));
  auto const hints = render_active_run_hint_lines(snapshot, completion_cache, width,
                                                  alerts.empty() ? status_alert_line_budget : (status_alert_line_budget > alerts.size() ? 1 : 0));
  status_alert_lines = hints;
  status_alert_lines.insert(status_alert_lines.end(), alerts.begin(), alerts.end());
  auto const fixed_prompt_alert_lines = fixed_and_prompt_lines + status_alert_lines.size();
  auto const palette_line_budget = (content_height > fixed_prompt_alert_lines && !prompt_active && !snapshot.slash_palette_suppressed)
                                       ? std::min(detail::kMaxPaletteLines, content_height - fixed_prompt_alert_lines)
                                       : 0;
  palette_lines = detail::render_slash_palette(snapshot, width, palette_line_budget);
  if (palette_lines.empty())
    palette_lines = detail::render_file_reference_palette(snapshot, completion_cache, width, palette_line_budget);
  if (palette_lines.empty())
    palette_lines = detail::render_path_completion_palette(snapshot, completion_cache, width, palette_line_budget);
  auto const fixed_prompt_palette_lines = fixed_prompt_alert_lines + palette_lines.size();
  auto const action_feedback = snapshot.local_command_feedback ? snapshot.local_command_feedback : snapshot.reasoning_feedback;
  if (!prompt_active && palette_lines.empty() && action_feedback && !action_feedback->empty() && content_height > fixed_prompt_palette_lines)
  {
    status_alert_lines.push_back(
        detail::fit_line_preserving_sgr(std::string(kSgrDim) + "  " + sanitize_terminal_text(*action_feedback) + std::string(kSgrReset), width));
  }
  auto const fixed_prompt_feedback_palette_lines =
      normal_composer_lines + permission_lines.size() + question_lines.size() + status_alert_lines.size() + palette_lines.size();
  auto const queued_line_budget = (content_height > fixed_prompt_feedback_palette_lines && !prompt_active)
                                      ? std::min<std::size_t>(3, content_height - fixed_prompt_feedback_palette_lines)
                                      : 0;
  queued_lines = render_queued_message_lines(snapshot, width, queued_line_budget);
  auto const fixed_prompt_feedback_palette_queued_lines = fixed_prompt_feedback_palette_lines + queued_lines.size();
  auto const pending_attachment_line_budget = (content_height > fixed_prompt_feedback_palette_queued_lines && !prompt_active)
                                                  ? std::min<std::size_t>(8, content_height - fixed_prompt_feedback_palette_queued_lines)
                                                  : 0;
  pending_attachment_lines = render_pending_attachment_lines(snapshot, width, pending_attachment_line_budget, emit_attachment_graphics);

  auto const reserved_before_todo_dock = normal_composer_lines + permission_lines.size() + question_lines.size() + status_alert_lines.size() +
                                         palette_lines.size() + queued_lines.size() + pending_attachment_lines.lines.size();
  auto const plugin_available = content_height > reserved_before_todo_dock ? content_height - reserved_before_todo_dock : std::size_t{0};
  auto const plugin_budget = snapshot.plugin_ui_dock ? std::min<std::size_t>(12, plugin_available > 1 ? plugin_available - 1 : plugin_available) : 0;
  if (plugin_line_budget)
    *plugin_line_budget = plugin_budget;
  todo_dock_lines = snapshot.plugin_ui_dock ? detail::render_plugin_ui_dock(*snapshot.plugin_ui_dock, width, plugin_budget) : std::vector<std::string>{};
  auto const reserved_after_plugin_dock = reserved_before_todo_dock + todo_dock_lines.size();
  auto const todo_budget = todo_dock_line_budget(snapshot, completion_cache, width, content_height, reserved_after_plugin_dock);
  if (todo_budget > 0 && snapshot.sidebar)
  {
    auto todo_lines = render_todo_dock_lines(*snapshot.sidebar, width, todo_budget);
    todo_dock_lines.insert(todo_dock_lines.end(), std::make_move_iterator(todo_lines.begin()), std::make_move_iterator(todo_lines.end()));
  }

  auto const non_transcript_lines = reserved_before_todo_dock + todo_dock_lines.size();
  auto const available_lines = content_height > non_transcript_lines ? content_height - non_transcript_lines : std::size_t{0};
  auto const desired_gap = allow_transcript_gap ? detail::composer_layout_policy(snapshot, content_height).transcript_composer_gap_lines : std::size_t{0};
  auto const gap_lines = available_lines >= 2 ? std::min(desired_gap, available_lines - 1) : std::size_t{0};
  return {.transcript_height = available_lines - gap_lines, .transcript_composer_gap_lines = gap_lines};
}

std::size_t input_line_cursor_offset_for_columns(std::string_view line, std::size_t target_columns)
{
  if (target_columns == 0)
    return 0;
  std::size_t columns = 0;
  for (std::size_t index = 0; index < line.size();)
  {
    auto const byte = static_cast<unsigned char>(line[index]);
    if (byte == '\t')
    {
      columns += 2;
      ++index;
      if (columns >= target_columns)
        return index;
      continue;
    }
    if (byte < 0x20 || byte == 0x7F)
    {
      ++columns;
      ++index;
      if (columns >= target_columns)
        return index;
      continue;
    }

    auto const cell = detail::terminal_text_cell(line, index);
    if (cell.valid)
    {
      columns += cell.columns;
      index += cell.bytes;
    }
    else
    {
      ++columns;
      ++index;
    }
    if (columns >= target_columns)
      return index;
  }
  return line.size();
}

}  // namespace

detail::ComposerLayoutPolicy detail::composer_layout_policy(ComposerSnapshot const& snapshot, std::size_t height)
{
  auto const authoritative_layout = snapshot.permission_prompt.has_value() || snapshot.question_prompt.has_value() || snapshot.plugin_ui_modal.has_value() ||
                                    snapshot.select_list.has_value() || snapshot.subagent_workspace.has_value() || snapshot.sidebar_drawer_visible;
  auto const roomy_normal_layout = height > 12 && !authoritative_layout;
  return {.compact_transcript_spacing = height <= 12,
          .transcript_composer_gap_lines = roomy_normal_layout ? std::size_t{1} : std::size_t{0},
          .composer_top_padding_lines = roomy_normal_layout ? std::size_t{1} : std::size_t{0}};
}

void detail::mark_screen_row_dirty(ScreenRowCache& screen_cache, std::size_t row)
{
  if (!screen_cache.valid || row >= screen_cache.surfaces.size())
    return;
  screen_cache.dirty_rows.resize(screen_cache.surfaces.size(), false);
  screen_cache.dirty_rows[row] = true;
}

std::vector<std::size_t> detail::changed_screen_rows(std::vector<std::string> const& previous, std::vector<std::string> const& current,
                                                     std::vector<bool> const& dirty_rows, bool invalidate)
{
  std::vector<std::size_t> changed;
  auto const row_count = std::max(previous.size(), current.size());
  changed.reserve(row_count);
  for (std::size_t row = 0; row < row_count; ++row)
  {
    if (invalidate || row >= previous.size() || row >= current.size() || previous[row] != current[row] || (row < dirty_rows.size() && dirty_rows[row]))
      changed.push_back(row);
  }
  return changed;
}

detail::NcursesColorRole detail::ncurses_color_role_for_sgr(std::string_view sgr)
{
  if (sgr == detail::kSgrMuted || sgr == detail::kSgrThinking || sgr == detail::kSgrTextDimmed)
    return detail::NcursesColorRole::Muted;
  if (sgr == detail::kSgrAccent)
    return detail::NcursesColorRole::Accent;
  if (sgr == detail::kSgrSuccess)
    return detail::NcursesColorRole::Success;
  if (sgr == detail::kSgrWarning)
    return detail::NcursesColorRole::Warning;
  if (sgr == detail::kSgrError)
    return detail::NcursesColorRole::Error;
  return detail::NcursesColorRole::Text;
}

ComposerFrame finish_frame(ComposerFrame frame)
{
  if (tui_plain_output())
  {
    frame.lines = strip_sgr_frame(std::move(frame.lines));
    frame.graphics.clear();
  }
  return frame;
}

ComposerCanvasLayout composer_canvas_layout(ComposerSnapshot const& snapshot)
{
  auto const width = std::max<std::size_t>(detail::kMinWidth, snapshot.width);
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  if (sidebar_drawer_active(snapshot) || (snapshot.subagent_workspace && !snapshot.permission_prompt && !snapshot.question_prompt))
    return {.content_width = width, .left = 0, .rail_visible = false};
  if ((snapshot.plugin_ui_dock || snapshot.plugin_ui_modal) && !snapshot.permission_prompt && !snapshot.question_prompt && !snapshot.select_list &&
      !snapshot.subagent_workspace)
  {
    auto const content_width = std::min(width, kPluginUiCanvasMaxWidth);
    return {.content_width = content_width, .left = (width - content_width) / 2, .rail_visible = false};
  }
  if (sidebar_visible(snapshot, width, height))
    return {.content_width = main_width_for(snapshot, width), .left = 0, .rail_visible = true};
  auto const content_width = std::min(width, kCanvasMaxWidth);
  return {.content_width = content_width, .left = (width - content_width) / 2, .rail_visible = false};
}

ComposerFrame detail::render_composer_frame_cached(ComposerSnapshot const& snapshot, CompletionMatchCache& completion_cache, std::size_t source_revision,
                                                   TranscriptLayoutCache* transcript_cache, std::size_t transcript_generation, bool center_canvas,
                                                   bool allow_transcript_gap, bool freeze_transcript_layout, bool allow_frozen_width_mismatch)
{
  refresh_completion_match_cache(completion_cache, snapshot, source_revision);
  auto const width = std::max<std::size_t>(detail::kMinWidth, snapshot.width);
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  auto const canvas = composer_canvas_layout(snapshot);
  if (center_canvas && (canvas.left > 0 || (!canvas.rail_visible && canvas.content_width < width)))
  {
    auto inner = snapshot;
    inner.width = canvas.content_width;
    auto frame = detail::render_composer_frame_cached(inner, completion_cache, source_revision, transcript_cache, transcript_generation, false,
                                                      allow_transcript_gap, freeze_transcript_layout, allow_frozen_width_mismatch);
    auto const right = width - canvas.left - canvas.content_width;
    for (auto& line : frame.lines)
    {
      line = std::string(canvas.left, ' ') + pad_line_to_width(std::move(line), canvas.content_width) + std::string(right, ' ');
    }
    for (auto& graphic : frame.graphics)
      graphic.column += canvas.left;
    return finish_frame(std::move(frame));
  }
  if (snapshot.question_prompt && snapshot.question_prompt->modal)
  {
    auto base = snapshot;
    auto const prompt = *base.question_prompt;
    base.question_prompt = std::nullopt;
    base.command_output.reset();
    base.plugin_ui_modal.reset();
    base.select_list.reset();
    base.subagent_workspace.reset();
    base.reasoning_feedback.reset();
    base.local_command_feedback.reset();
    base.sidebar = std::nullopt;
    // Preserve the modal's authoritative vertical policy while rendering its quiet backdrop.
    base.sidebar_drawer_visible = true;
    auto frame = detail::render_composer_frame_cached(base, completion_cache, source_revision, transcript_cache, transcript_generation, false, false,
                                                      freeze_transcript_layout, allow_frozen_width_mismatch);
    frame.lines = overlay_question_modal(quiet_modal_backdrop(std::move(frame.lines)), prompt, width, height);
    frame.graphics.clear();
    return finish_frame(std::move(frame));
  }
  if (snapshot.command_output && !snapshot.permission_prompt && !snapshot.question_prompt)
  {
    auto base = snapshot;
    auto const view = *base.command_output;
    base.command_output.reset();
    base.plugin_ui_modal.reset();
    base.select_list.reset();
    base.subagent_workspace.reset();
    base.reasoning_feedback.reset();
    base.local_command_feedback.reset();
    base.sidebar = std::nullopt;
    base.sidebar_drawer_visible = true;
    auto frame = detail::render_composer_frame_cached(base, completion_cache, source_revision, transcript_cache, transcript_generation, false, false,
                                                      freeze_transcript_layout, allow_frozen_width_mismatch);
    frame.lines = overlay_command_output_modal(quiet_modal_backdrop(std::move(frame.lines)), view, width, height, snapshot.tool_presentation);
    frame.graphics.clear();
    return finish_frame(std::move(frame));
  }
  if (snapshot.subagent_workspace && !snapshot.permission_prompt && !snapshot.question_prompt)
  {
    ComposerFrame frame;
    frame.lines = render_subagent_workspace(*snapshot.subagent_workspace, width, height);
    return finish_frame(std::move(frame));
  }
  if (snapshot.select_list && !snapshot.permission_prompt && !snapshot.question_prompt)
  {
    auto base = snapshot;
    auto const view = *base.select_list;
    base.select_list = std::nullopt;
    base.plugin_ui_modal.reset();
    base.reasoning_feedback.reset();
    base.local_command_feedback.reset();
    base.sidebar = std::nullopt;
    // Preserve the modal's authoritative vertical policy while rendering its quiet backdrop.
    base.sidebar_drawer_visible = true;
    auto frame = detail::render_composer_frame_cached(base, completion_cache, source_revision, transcript_cache, transcript_generation, false, false,
                                                      freeze_transcript_layout, allow_frozen_width_mismatch);
    frame.lines = overlay_select_list_modal(quiet_modal_backdrop(std::move(frame.lines)), view, width, height);
    frame.graphics.clear();
    return finish_frame(std::move(frame));
  }
  if (snapshot.plugin_ui_modal && !snapshot.permission_prompt && !snapshot.question_prompt && !snapshot.select_list && !snapshot.subagent_workspace)
  {
    auto base = snapshot;
    auto const view = *base.plugin_ui_modal;
    base.plugin_ui_modal.reset();
    base.reasoning_feedback.reset();
    base.local_command_feedback.reset();
    base.sidebar = std::nullopt;
    base.sidebar_drawer_visible = true;
    auto frame = detail::render_composer_frame_cached(base, completion_cache, source_revision, transcript_cache, transcript_generation, false, false,
                                                      freeze_transcript_layout, allow_frozen_width_mismatch);
    frame.lines = overlay_plugin_ui_modal(quiet_modal_backdrop(std::move(frame.lines)), view, width, height);
    frame.graphics.clear();
    return finish_frame(std::move(frame));
  }
  if (canvas.rail_visible)
  {
    auto const sidebar_width = std::min<std::size_t>(kSidebarWidth, width / 3);
    auto const main_width = canvas.content_width;
    auto main_frame = render_composer_main_frame(snapshot, main_width, height, completion_cache, source_revision, transcript_cache, transcript_generation,
                                                 freeze_transcript_layout, allow_frozen_width_mismatch);
    auto sidebar_lines = render_sidebar(*snapshot.sidebar, sidebar_width, height);
    ComposerFrame combined;
    combined.lines.reserve(height);
    for (std::size_t row = 0; row < height; ++row)
    {
      auto const main_line = row < main_frame.lines.size() ? main_frame.lines[row] : std::string{};
      auto const sidebar_line = row < sidebar_lines.size() ? sidebar_lines[row] : std::string{};
      combined.lines.push_back(pad_line_to_width(main_line, main_width) + std::string(kSgrDim) + "│" + std::string(kSgrReset) +
                               pad_line_to_width(sidebar_line, sidebar_width));
    }
    combined.graphics = std::move(main_frame.graphics);
    return finish_frame(std::move(combined));
  }
  if (sidebar_drawer_active(snapshot))
  {
    auto frame = render_sidebar_drawer(snapshot, width, height);
    frame.graphics.clear();
    return finish_frame(std::move(frame));
  }
  ComposerFrame frame;
  frame.lines.reserve(height);

  std::size_t normal_composer_lines = 0;
  std::vector<std::string> permission_lines;
  std::vector<std::string> question_lines;
  std::vector<std::string> status_alert_lines;
  std::vector<std::string> palette_lines;
  std::vector<std::string> queued_lines;
  PendingAttachmentRender pending_attachment_lines;
  std::vector<std::string> todo_dock_lines;
  auto const vertical_layout =
      calculate_composer_vertical_layout(snapshot, completion_cache, width, height, normal_composer_lines, permission_lines, question_lines, status_alert_lines,
                                         palette_lines, queued_lines, pending_attachment_lines, todo_dock_lines, true, allow_transcript_gap);
  auto const transcript_height = vertical_layout.transcript_height;
  std::vector<std::string> visible_transcript;
  detail::TranscriptLayoutCache local_transcript_cache;
  auto const selection_active = snapshot.transcript_selection_anchor_item != std::string::npos && snapshot.transcript_selection_focus_item != std::string::npos;
  // Selection paint/hit-test share the full TranscriptLayout authority. While a
  // selection is active, force the full-layout visible path so the overlay maps
  // exactly onto the rows currently drawn from that authority.
  if (snapshot.transcript_scroll_offset > 0 || selection_active || snapshot.transcript_position_indicator_visible)
  {
    auto& active_cache = transcript_cache ? *transcript_cache : local_transcript_cache;
    auto const compact_spacing = detail::composer_layout_policy(snapshot, height).compact_transcript_spacing;
    auto const frozen_presentation_compatible = active_cache.valid && active_cache.tool_presentation == snapshot.tool_presentation &&
                                                active_cache.thinking_visible == snapshot.thinking_visible && active_cache.compact_spacing == compact_spacing;
    // Ordinary detached freeze still requires width parity. Modal underlying-layout freeze may keep a
    // pre-modal width while non-geometry presentation settings continue to match.
    auto const frozen_cache_compatible =
        frozen_presentation_compatible && (active_cache.width == width || (freeze_transcript_layout && allow_frozen_width_mismatch));
    if (!selection_active && (!freeze_transcript_layout || !frozen_cache_compatible))
    {
      detail::refresh_transcript_layout_cache(active_cache, snapshot.transcript, transcript_generation, width, snapshot.tool_presentation,
                                              snapshot.thinking_visible, compact_spacing, detail::active_mermaid_projection(snapshot));
    }
    visible_transcript = detail::cached_visible_transcript_lines(active_cache, transcript_height, snapshot.transcript_scroll_offset);
    if (selection_active && active_cache.valid)
    {
      auto const max_scroll = detail::cached_transcript_max_scroll_offset(active_cache, transcript_height);
      auto const scroll = std::min(snapshot.transcript_scroll_offset, max_scroll);
      auto const visible_start =
          active_cache.layout.lines.size() > transcript_height ? (active_cache.layout.lines.size() - transcript_height - scroll) : std::size_t{0};
      TranscriptSelectionRange const range{
          .anchor =
              TranscriptSelectionEndpoint{
                  .item_index = snapshot.transcript_selection_anchor_item,
                  .line_offset = snapshot.transcript_selection_anchor_line,
                  .display_column = snapshot.transcript_selection_anchor_column,
              },
          .focus =
              TranscriptSelectionEndpoint{
                  .item_index = snapshot.transcript_selection_focus_item,
                  .line_offset = snapshot.transcript_selection_focus_line,
                  .display_column = snapshot.transcript_selection_focus_column,
              },
      };
      // Non-mutating overlay: only the visible frame copy is highlighted.
      apply_transcript_selection_overlay(visible_transcript, active_cache.layout, range, visible_start, tui_plain_output());
    }
    if (snapshot.transcript_position_indicator_visible && active_cache.valid)
    {
      auto const geometry =
          detail::transcript_position_indicator_geometry(active_cache.layout.lines.size(), transcript_height, snapshot.transcript_scroll_offset);
      detail::apply_transcript_position_indicator_overlay(visible_transcript, width, geometry, tui_plain_output());
    }
  }
  else
  {
    auto& active_cache = transcript_cache ? *transcript_cache : local_transcript_cache;
    auto const rendered_transcript = detail::render_transcript_tail_lines_cached(
        active_cache.tail, snapshot.transcript, transcript_generation, width, transcript_height, snapshot.tool_presentation, snapshot.thinking_visible,
        detail::composer_layout_policy(snapshot, height).compact_transcript_spacing, detail::active_mermaid_projection(snapshot));
    visible_transcript = detail::visible_transcript_lines(rendered_transcript, width, transcript_height, 0);
  }
  frame.lines.insert(frame.lines.end(), visible_transcript.begin(), visible_transcript.end());
  if (frame.lines.size() < transcript_height && snapshot.transcript.empty())
  {
    auto const discovery = render_empty_transcript_discovery_lines(snapshot, completion_cache, width, transcript_height);
    for (auto const& line : discovery)
    {
      if (frame.lines.size() >= transcript_height)
        break;
      frame.lines.push_back(line);
    }
  }
  while (frame.lines.size() < transcript_height)
  {
    frame.lines.push_back("");
  }
  for (std::size_t row = 0; row < vertical_layout.transcript_composer_gap_lines; ++row)
    frame.lines.emplace_back();

  for (auto const& line : palette_lines)
  {
    frame.lines.push_back(line);
  }
  for (auto const& line : queued_lines)
  {
    frame.lines.push_back(line);
  }
  auto const pending_attachment_start_row = frame.lines.size();
  for (auto const& line : pending_attachment_lines.lines)
  {
    frame.lines.push_back(line);
  }
  for (auto graphic : pending_attachment_lines.graphics)
  {
    graphic.row += pending_attachment_start_row;
    frame.graphics.push_back(std::move(graphic));
  }
  for (auto const& line : todo_dock_lines)
  {
    frame.lines.push_back(line);
  }
  for (auto const& line : status_alert_lines)
  {
    frame.lines.push_back(line);
  }
  for (auto const& line : permission_lines)
  {
    frame.lines.push_back(line);
  }
  for (auto const& line : question_lines)
  {
    frame.lines.push_back(line);
  }
  auto const composer_lines = detail::render_composer_block(snapshot, width, normal_composer_lines);
  frame.lines.insert(frame.lines.end(), composer_lines.begin(), composer_lines.end());
  return finish_frame(std::move(frame));
}

ComposerFrame render_composer_frame(ComposerSnapshot const& snapshot)
{
  detail::CompletionMatchCache completion_cache;
  return detail::render_composer_frame_cached(snapshot, completion_cache, snapshot.file_references_generation, nullptr, snapshot.transcript_generation);
}

std::vector<std::string> render_composer(ComposerSnapshot const& snapshot)
{
  return render_composer_frame(snapshot).lines;
}

std::size_t composer_main_width(ComposerSnapshot const& snapshot)
{
  return composer_canvas_layout(snapshot).content_width;
}

PluginUiSurfaceGeometry plugin_ui_surface_geometry(ComposerSnapshot const& snapshot, TuiPluginUiKind kind)
{
  auto candidate = snapshot;
  auto const modal = kind == TuiPluginUiKind::Select || kind == TuiPluginUiKind::Confirm;
  if (modal)
  {
    candidate.plugin_ui_modal =
        TuiPluginUiModalView{.binding = {}, .request_id = {}, .kind = kind, .title = {}, .description = {}, .options = {}, .selected_option = 0};
    auto const canvas = composer_canvas_layout(candidate);
    auto const height = std::max<std::size_t>(detail::kMinHeight, candidate.height);
    return {.width = std::min(plugin_ui_modal_width_for(canvas.content_width), canvas.content_width),
            .max_lines = std::min(plugin_ui_modal_height_for(height), height)};
  }

  if (!candidate.plugin_ui_dock)
  {
    candidate.plugin_ui_dock = TuiPluginUiDockView{.binding = {.plugin_id = "a", .command = "a", .invocation_id = "a"}, .status = std::string{}, .widgets = {}};
  }
  auto const canvas = composer_canvas_layout(candidate);
  auto const width = canvas.content_width;
  auto const height = std::max<std::size_t>(detail::kMinHeight, candidate.height);
  candidate.width = width;

  detail::CompletionMatchCache completion_cache;
  std::size_t composer_lines = 0;
  std::vector<std::string> permission_lines;
  std::vector<std::string> question_lines;
  std::vector<std::string> status_lines;
  std::vector<std::string> palette_lines;
  std::vector<std::string> queued_lines;
  PendingAttachmentRender attachments;
  std::vector<std::string> dock_lines;
  std::size_t plugin_line_budget = 0;
  static_cast<void>(calculate_composer_vertical_layout(candidate, completion_cache, width, height, composer_lines, permission_lines, question_lines,
                                                       status_lines, palette_lines, queued_lines, attachments, dock_lines, false, true, &plugin_line_budget));
  return {.width = width, .max_lines = plugin_line_budget};
}

namespace {

struct TranscriptHeaderHitGeometry
{
  std::size_t width = 0;
  std::size_t item_index = 0;
  bool valid = false;
};

TranscriptHeaderHitGeometry transcript_header_hit_geometry(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column)
{
  if (row == 0 || column == 0 || snapshot.sidebar_drawer_visible || snapshot.command_output || snapshot.select_list || snapshot.subagent_workspace ||
      (snapshot.question_prompt && snapshot.question_prompt->modal))
    return {};

  auto const canvas = composer_canvas_layout(snapshot);
  auto const width = canvas.content_width;
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  if (column <= canvas.left || column > canvas.left + width)
    return {};
  column -= canvas.left;

  auto main = snapshot;
  main.width = width;
  main.height = height;
  prepare_main_column_geometry_snapshot(main, canvas.rail_visible);
  detail::CompletionMatchCache completion_cache;
  detail::refresh_completion_match_cache(completion_cache, main, main.file_references_generation);
  std::size_t composer_lines = 0;
  std::vector<std::string> permission_lines;
  std::vector<std::string> question_lines;
  std::vector<std::string> status_lines;
  std::vector<std::string> palette_lines;
  std::vector<std::string> queued_lines;
  PendingAttachmentRender attachments;
  std::vector<std::string> todo_dock_lines;
  auto const vertical_layout = calculate_composer_vertical_layout(main, completion_cache, width, height, composer_lines, permission_lines, question_lines,
                                                                  status_lines, palette_lines, queued_lines, attachments, todo_dock_lines, false);
  auto const transcript_height = vertical_layout.transcript_height;
  // Transcript hit rows start at the first frame row; no leading chrome is reserved.
  auto const transcript_row = row - 1;
  if (transcript_row >= transcript_height)
    return {};

  auto const layout = detail::render_transcript_layout(snapshot.transcript, width, snapshot.tool_presentation, snapshot.thinking_visible,
                                                       detail::composer_layout_policy(snapshot, height).compact_transcript_spacing,
                                                       detail::active_mermaid_projection(snapshot));
  if (layout.lines.empty())
    return {};
  auto const max_scroll = layout.lines.size() > transcript_height ? layout.lines.size() - transcript_height : std::size_t{0};
  auto const visible_start = max_scroll - std::min(snapshot.transcript_scroll_offset, max_scroll);
  auto const line_index = visible_start + transcript_row;
  if (line_index >= layout.lines.size())
    return {};

  auto const content = std::ranges::find(layout.content_starts, line_index);
  if (content == layout.content_starts.end())
    return {};
  auto const position = static_cast<std::size_t>(content - layout.content_starts.begin());
  if (position >= layout.message_item_indices.size())
    return {};
  auto const item_index = layout.message_item_indices[position];
  if (item_index >= snapshot.transcript.size() || column <= 2 || column > width - std::min<std::size_t>(2, width))
    return {};
  return TranscriptHeaderHitGeometry{.width = width, .item_index = item_index, .valid = true};
}

}  // namespace

detail::TranscriptBodyScreenGeometry detail::transcript_body_screen_geometry(ComposerSnapshot const& snapshot)
{
  if (snapshot.sidebar_drawer_visible || snapshot.command_output || snapshot.plugin_ui_modal || snapshot.select_list || snapshot.subagent_workspace ||
      (snapshot.question_prompt && snapshot.question_prompt->modal) || snapshot.permission_prompt)
    return {};

  auto const canvas = composer_canvas_layout(snapshot);
  auto const width = canvas.content_width;
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  auto main = snapshot;
  main.width = width;
  main.height = height;
  prepare_main_column_geometry_snapshot(main, canvas.rail_visible);
  detail::CompletionMatchCache completion_cache;
  detail::refresh_completion_match_cache(completion_cache, main, main.file_references_generation);
  std::size_t composer_lines = 0;
  std::vector<std::string> permission_lines;
  std::vector<std::string> question_lines;
  std::vector<std::string> status_lines;
  std::vector<std::string> palette_lines;
  std::vector<std::string> queued_lines;
  PendingAttachmentRender attachments;
  std::vector<std::string> todo_dock_lines;
  auto const vertical_layout = calculate_composer_vertical_layout(main, completion_cache, width, height, composer_lines, permission_lines, question_lines,
                                                                  status_lines, palette_lines, queued_lines, attachments, todo_dock_lines, false);
  if (vertical_layout.transcript_height == 0)
    return {};
  return TranscriptBodyScreenGeometry{
      .transcript_height = vertical_layout.transcript_height, .content_width = width, .canvas_left = canvas.left, .valid = true};
}

std::optional<std::size_t> detail::transcript_tool_card_header_for_screen_position(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column)
{
  auto const hit = transcript_header_hit_geometry(snapshot, row, column);
  if (!hit.valid || !snapshot.transcript[hit.item_index].tool)
    return std::nullopt;
  return hit.item_index;
}

std::optional<std::size_t> detail::transcript_thinking_header_for_screen_position(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column)
{
  auto const hit = transcript_header_hit_geometry(snapshot, row, column);
  if (!hit.valid)
    return std::nullopt;
  auto const& item = snapshot.transcript[hit.item_index];
  if (!transcript_item_has_boundable_thinking(item, hit.width, snapshot.thinking_visible))
    return std::nullopt;
  return hit.item_index;
}

std::optional<ComposerPaletteScreenLayout> detail::composer_palette_screen_layout_cached(ComposerSnapshot const& snapshot,
                                                                                         CompletionMatchCache& completion_cache, std::size_t source_revision)
{
  refresh_completion_match_cache(completion_cache, snapshot, source_revision);
  if (snapshot.sidebar_drawer_visible || snapshot.command_output || snapshot.select_list || snapshot.subagent_workspace ||
      (snapshot.question_prompt && snapshot.question_prompt->modal) || snapshot.slash_palette_suppressed)
    return std::nullopt;
  auto const canvas = composer_canvas_layout(snapshot);
  auto const width = canvas.content_width;
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  auto main = snapshot;
  main.width = width;
  main.height = height;
  prepare_main_column_geometry_snapshot(main, canvas.rail_visible);
  std::size_t composer_lines = 0;
  std::vector<std::string> permission_lines;
  std::vector<std::string> question_lines;
  std::vector<std::string> status_lines;
  std::vector<std::string> palette_lines;
  std::vector<std::string> queued_lines;
  PendingAttachmentRender attachments;
  std::vector<std::string> todo_dock_lines;
  auto const vertical_layout = calculate_composer_vertical_layout(main, completion_cache, width, height, composer_lines, permission_lines, question_lines,
                                                                  status_lines, palette_lines, queued_lines, attachments, todo_dock_lines, false);
  if (palette_lines.empty())
    return std::nullopt;
  return ComposerPaletteScreenLayout{.first_item_row = vertical_layout.transcript_height + vertical_layout.transcript_composer_gap_lines + 1,
                                     .item_count = palette_lines.size(),
                                     .first_item_index = 0};
}

std::optional<ComposerPaletteScreenLayout> composer_palette_screen_layout(ComposerSnapshot const& snapshot)
{
  detail::CompletionMatchCache completion_cache;
  return detail::composer_palette_screen_layout_cached(snapshot, completion_cache, snapshot.file_references_generation);
}

std::size_t detail::composer_max_transcript_scroll_offset_cached(ComposerSnapshot const& snapshot, std::size_t width, std::size_t height,
                                                                 CompletionMatchCache& completion_cache, std::size_t source_revision,
                                                                 TranscriptLayoutCache& transcript_cache, std::size_t transcript_generation,
                                                                 bool allow_transcript_gap)
{
  refresh_completion_match_cache(completion_cache, snapshot, source_revision);
  width = std::max<std::size_t>(detail::kMinWidth, width);
  height = std::max<std::size_t>(detail::kMinHeight, height);
  auto layout_snapshot = snapshot;
  layout_snapshot.width = width;
  layout_snapshot.height = height;
  auto const canvas = composer_canvas_layout(layout_snapshot);
  width = canvas.content_width;
  layout_snapshot.width = width;
  prepare_main_column_geometry_snapshot(layout_snapshot, canvas.rail_visible);
  if (sidebar_drawer_active(layout_snapshot))
    return 0;
  if ((snapshot.question_prompt && snapshot.question_prompt->modal) || snapshot.select_list)
  {
    auto base = snapshot;
    if (base.question_prompt && base.question_prompt->modal)
      base.question_prompt = std::nullopt;
    if (base.select_list)
      base.select_list = std::nullopt;
    base.reasoning_feedback.reset();
    base.local_command_feedback.reset();
    base.sidebar = std::nullopt;
    // Preserve the modal's authoritative vertical policy for its backdrop viewport.
    base.sidebar_drawer_visible = true;
    return detail::composer_max_transcript_scroll_offset_cached(base, width, height, completion_cache, source_revision, transcript_cache, transcript_generation,
                                                                false);
  }

  std::size_t normal_composer_lines = 0;
  std::vector<std::string> permission_lines;
  std::vector<std::string> question_lines;
  std::vector<std::string> status_alert_lines;
  std::vector<std::string> palette_lines;
  std::vector<std::string> queued_lines;
  PendingAttachmentRender pending_attachment_lines;
  std::vector<std::string> todo_dock_lines;
  auto const vertical_layout = calculate_composer_vertical_layout(layout_snapshot, completion_cache, width, height, normal_composer_lines, permission_lines,
                                                                  question_lines, status_alert_lines, palette_lines, queued_lines, pending_attachment_lines,
                                                                  todo_dock_lines, false, allow_transcript_gap);
  if (vertical_layout.transcript_height == 0)
    return 0;
  refresh_transcript_layout_cache(transcript_cache, snapshot.transcript, transcript_generation, width, snapshot.tool_presentation, snapshot.thinking_visible,
                                  detail::composer_layout_policy(snapshot, height).compact_transcript_spacing, detail::active_mermaid_projection(snapshot));
  return cached_transcript_max_scroll_offset(transcript_cache, vertical_layout.transcript_height);
}

std::size_t composer_max_transcript_scroll_offset(ComposerSnapshot const& snapshot, std::size_t width, std::size_t height)
{
  detail::CompletionMatchCache completion_cache;
  detail::TranscriptLayoutCache transcript_cache;
  return detail::composer_max_transcript_scroll_offset_cached(snapshot, width, height, completion_cache, snapshot.file_references_generation, transcript_cache,
                                                              snapshot.transcript_generation);
}

std::size_t sidebar_drawer_max_scroll_offset(ComposerSnapshot const& snapshot)
{
  if (!snapshot.sidebar_drawer_visible || !snapshot.sidebar)
    return 0;
  auto const width = std::max<std::size_t>(detail::kMinWidth, snapshot.width);
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  auto const body_height = sidebar_drawer_body_height(snapshot, width, height);
  auto const body = render_sidebar_drawer_body(*snapshot.sidebar, width);
  return body.size() > body_height ? body.size() - body_height : std::size_t{0};
}

std::optional<std::size_t> composer_input_cursor_for_screen_position(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column)
{
  if (row == 0 || column == 0 || snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output || snapshot.plugin_ui_modal ||
      snapshot.select_list || snapshot.subagent_workspace || sidebar_drawer_active(snapshot))
    return std::nullopt;

  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  auto const canvas = composer_canvas_layout(snapshot);
  auto const width = canvas.content_width;
  if (column <= canvas.left || column > canvas.left + width)
    return std::nullopt;
  column -= canvas.left;

  auto const input_lines = detail::input_render_line_spans(snapshot.input, width);
  auto const composer_lines = detail::composer_block_line_count(snapshot, height, width);
  auto const policy = detail::composer_layout_policy(snapshot, height);
  auto const layout = detail::composer_input_layout(input_lines.size(), composer_lines, snapshot.draft_scroll_offset, policy.composer_top_padding_lines);
  auto const composer_start_row = height >= composer_lines ? height - composer_lines : std::size_t{0};
  auto const row_index = row - 1;
  auto const first_input_row = composer_start_row + layout.top_padding;
  if (row_index < first_input_row)
    return std::nullopt;
  auto const visible_line = row_index - first_input_row;
  if (visible_line >= layout.visible_input_lines)
    return std::nullopt;
  auto const logical_line = layout.first_visible + visible_line;
  if (logical_line >= input_lines.size())
    return std::nullopt;

  auto const& input_line = input_lines[logical_line];
  auto const prefix_columns = detail::composer_input_prefix_columns(input_line.first_line);
  auto const target_columns = column <= prefix_columns ? std::size_t{0} : column - prefix_columns - 1;
  auto const line_offset = input_line_cursor_offset_for_columns(input_line.text, target_columns);
  return std::min(input_line.start + line_offset, input_line.end);
}

std::optional<std::size_t> question_option_for_screen_position(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column)
{
  if (row == 0 || column == 0 || !snapshot.question_prompt || snapshot.permission_prompt || snapshot.select_list || sidebar_drawer_active(snapshot))
    return std::nullopt;

  auto const canvas = composer_canvas_layout(snapshot);
  auto const width = canvas.content_width;
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  if (column <= canvas.left || column > canvas.left + width)
    return std::nullopt;
  column -= canvas.left;

  if (snapshot.question_prompt->modal)
  {
    auto const modal_width = std::min(modal_width_for(width), width);
    auto const modal_height = std::min(modal_height_for(height), height);
    auto const top = height > modal_height ? (height - modal_height) / 2 : std::size_t{0};
    auto const left = width > modal_width ? (width - modal_width) / 2 : std::size_t{0};
    if (row <= top || row > top + modal_height || column <= left || column > left + modal_width)
      return std::nullopt;
    return detail::question_option_for_modal_row(*snapshot.question_prompt, row - top - 1, modal_width, modal_height);
  }

  auto main = snapshot;
  main.width = width;
  main.height = height;
  prepare_main_column_geometry_snapshot(main, canvas.rail_visible);
  detail::CompletionMatchCache completion_cache;
  detail::refresh_completion_match_cache(completion_cache, main, main.file_references_generation);
  std::size_t composer_lines = 0;
  std::vector<std::string> permission_lines;
  std::vector<std::string> question_lines;
  std::vector<std::string> status_lines;
  std::vector<std::string> palette_lines;
  std::vector<std::string> queued_lines;
  PendingAttachmentRender attachments;
  std::vector<std::string> todo_dock_lines;
  static_cast<void>(calculate_composer_vertical_layout(main, completion_cache, width, height, composer_lines, permission_lines, question_lines, status_lines,
                                                       palette_lines, queued_lines, attachments, todo_dock_lines, false));
  if (question_lines.empty() || height < composer_lines + question_lines.size())
    return std::nullopt;
  auto const first_question_row = height - composer_lines - question_lines.size();
  auto const row_index = row - 1;
  if (row_index < first_question_row || row_index >= first_question_row + question_lines.size())
    return std::nullopt;
  return detail::question_option_for_dock_row(*snapshot.question_prompt, row_index - first_question_row, width, question_lines.size());
}

std::optional<std::size_t> select_list_selection_for_screen_position(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column)
{
  if (row == 0 || column == 0 || !snapshot.select_list || snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output)
    return std::nullopt;

  auto const canvas = composer_canvas_layout(snapshot);
  auto const width = canvas.content_width;
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  if (column <= canvas.left || column > canvas.left + width)
    return std::nullopt;
  column -= canvas.left;
  auto const modal_width = std::min(modal_width_for(width), width);
  auto const modal_height = std::min(select_modal_height_for(height), height);
  auto const modal_lines = detail::render_select_list_modal(*snapshot.select_list, modal_width, modal_height);
  auto const top = height > modal_lines.size() ? (height - modal_lines.size()) / 2 : std::size_t{0};
  auto const left = width > modal_width ? (width - modal_width) / 2 : std::size_t{0};
  if (row <= top || row > top + modal_lines.size())
    return std::nullopt;
  if (column <= left || column > left + modal_width)
    return std::nullopt;

  return detail::select_list_item_for_modal_row(*snapshot.select_list, row - top - 1, modal_width, modal_height);
}

std::optional<std::size_t> plugin_ui_modal_option_for_screen_position(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column)
{
  if (row == 0 || column == 0 || !snapshot.plugin_ui_modal || snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output ||
      snapshot.select_list || snapshot.subagent_workspace)
  {
    return std::nullopt;
  }
  auto const canvas = composer_canvas_layout(snapshot);
  auto const width = canvas.content_width;
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  if (column <= canvas.left || column > canvas.left + width)
    return std::nullopt;
  column -= canvas.left;
  auto const modal_width = std::min(plugin_ui_modal_width_for(width), width);
  auto const modal_height = std::min(plugin_ui_modal_height_for(height), height);
  auto const modal_lines = detail::render_plugin_ui_modal(*snapshot.plugin_ui_modal, modal_width, modal_height);
  auto const top = height > modal_lines.size() ? (height - modal_lines.size()) / 2 : std::size_t{0};
  auto const left = width > modal_width ? (width - modal_width) / 2 : std::size_t{0};
  if (row <= top || row > top + modal_lines.size() || column <= left || column > left + modal_width)
    return std::nullopt;
  return detail::plugin_ui_option_for_modal_row(*snapshot.plugin_ui_modal, row - top - 1, modal_width, modal_height);
}

void detail::clear_composer_terminal_graphics() noexcept
{
  auto image_ids = std::exchange(active_kitty_image_ids(), {});
  for (auto const image_id : image_ids)
  {
    try
    {
      auto const sequence = delete_kitty_image(image_id);
      static_cast<void>(std::fwrite(sequence.data(), 1, sequence.size(), stdout));
    }
    catch (...)
    {
      // Terminal cleanup is best-effort and must not escape teardown.
    }
  }
  static_cast<void>(std::fflush(stdout));
}

bool detail::draw_screen_cached(ComposerSnapshot const& snapshot, CompletionMatchCache& completion_cache, std::size_t source_revision,
                                TranscriptLayoutCache& transcript_cache, std::size_t transcript_generation, ScreenRowCache& screen_cache,
                                bool freeze_transcript_layout, bool allow_frozen_width_mismatch)
{
  auto& terminal_context = core::Application::instance().terminal_context();
  auto& window = terminal_context.stdscr();
  auto const terminal_rows = terminal_context.rows();
  auto const terminal_cols = terminal_context.cols();
  auto move_window = [&window](terminal::Position position) {
    if (window.is_initialized())
      window.move(position);
    else
      static_cast<void>(move(static_cast<int>(position.row()), static_cast<int>(position.col())));
  };
  auto& active_image_ids = active_kitty_image_ids();
  initialize_color_pairs();
  auto const width = std::max<std::size_t>(detail::kMinWidth, snapshot.width);
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  auto const theme = active_tui_theme();
  auto const style_key = theme.name + "|" + theme.badge + "|" + theme.revision + "|" + (tui_plain_output() ? "plain" : "styled") + "|" +
                         (terminal_context.has_colors() ? "colors" : "mono");
  auto const invalidate = !screen_cache.valid || screen_cache.width != width || screen_cache.height != height || screen_cache.style_key != style_key;
  if (invalidate)
  {
    if (!tui_plain_output() && terminal_context.has_colors())
    {
      static_cast<void>(bkgd(curses_attributes(CursesStyle{.attributes = {}, .color = NcursesColorRole::Text, .background = BackgroundRole::Screen}) | ' '));
    }
    else
    {
      static_cast<void>(bkgd(attr_t{} | ' '));
    }
  }
  auto const canvas = composer_canvas_layout(snapshot);
  auto frame = detail::render_composer_frame_cached(snapshot, completion_cache, source_revision, &transcript_cache, transcript_generation, true, true,
                                                    freeze_transcript_layout, allow_frozen_width_mismatch);
  auto const& lines = frame.lines;
  std::vector<std::string> surfaces;
  surfaces.reserve(lines.size());
  for (auto const& line : lines)
    surfaces.push_back(detail::screen_surface_line(line, width));
  auto const changed_rows = detail::changed_screen_rows(screen_cache.surfaces, surfaces, screen_cache.dirty_rows, invalidate);

  auto const cursor_visible = !snapshot.permission_prompt && !snapshot.question_prompt && !snapshot.command_output && !snapshot.plugin_ui_modal &&
                              !snapshot.select_list && !snapshot.subagent_workspace && !sidebar_drawer_active(snapshot);
  if (!cursor_visible)
    terminal_context.set_cursor_visible(false);
  if (window.is_initialized())
    window.leaveok(!cursor_visible);
  else
    static_cast<void>(leaveok(stdscr, cursor_visible ? FALSE : TRUE));

  std::vector<std::pair<std::size_t, std::string>> osc_overlay_lines;
  for (auto const index : changed_rows)
  {
    if (index > static_cast<std::size_t>(terminal_rows > 0 ? terminal_rows - 1 : 0))
      break;
    move_window({static_cast<uint32_t>(index), 0});
    auto const surface_line = index < surfaces.size() ? surfaces[index] : detail::screen_surface_line("", width);
    if (line_contains_osc_sequence(surface_line))
      osc_overlay_lines.emplace_back(index, surface_line);
    draw_styled_line(surface_line, false);
  }

  auto cursor = input_cursor_placement(snapshot, lines.size(), canvas.content_width);
  cursor.column += canvas.left;
  if (cursor_visible)
  {
    move_window({static_cast<uint32_t>(std::min<std::size_t>(cursor.row, terminal_rows > 0 ? terminal_rows - 1 : 0)),
                 static_cast<uint32_t>(std::min<std::size_t>(cursor.column, terminal_cols > 0 ? terminal_cols - 1 : 0))});
    terminal_context.set_cursor_visible(true);
  }

  auto fail_screen_draw = [&]() -> bool {
    screen_cache.valid = false;
    return false;
  };
  if (window.is_initialized())
  {
    window.wnoutrefresh();
    terminal_context.doupdate();
  }
  else if (wnoutrefresh(stdscr) == ERR || doupdate() == ERR)
  {
    return fail_screen_draw();
  }
  bool wrote_direct_sequences = false;
  for (auto const& [row, line] : osc_overlay_lines)
  {
    if (row >= static_cast<std::size_t>(terminal_rows))
      continue;
    auto const move = "\x1b[" + std::to_string(row + 1) + ";1H";
    if (std::fwrite(move.data(), 1, move.size(), stdout) != move.size())
      return fail_screen_draw();
    if (std::fwrite(line.data(), 1, line.size(), stdout) != line.size())
      return fail_screen_draw();
    wrote_direct_sequences = true;
  }
  std::vector<std::size_t> current_kitty_image_ids;
  for (auto const& graphic : frame.graphics)
  {
    if (graphic.protocol == TerminalImageProtocol::Kitty && graphic.image_id)
      current_kitty_image_ids.push_back(*graphic.image_id);
  }
  auto const graphics_changed = invalidate || !terminal_graphics_equal(screen_cache.graphics, frame.graphics);
  if (graphics_changed)
  {
    for (auto const image_id : active_image_ids)
    {
      if (std::ranges::find(current_kitty_image_ids, image_id) != current_kitty_image_ids.end())
        continue;
      auto const sequence = delete_kitty_image(image_id);
      if (std::fwrite(sequence.data(), 1, sequence.size(), stdout) != sequence.size())
        return fail_screen_draw();
      wrote_direct_sequences = true;
    }
    for (auto const& graphic : frame.graphics)
    {
      if (graphic.sequence.empty() || graphic.row >= static_cast<std::size_t>(terminal_rows))
        continue;
      auto const column = std::min<std::size_t>(graphic.column, terminal_cols > 0 ? terminal_cols - 1 : 0);
      auto const move = "\x1b[" + std::to_string(graphic.row + 1) + ";" + std::to_string(column + 1) + "H";
      if (std::fwrite(move.data(), 1, move.size(), stdout) != move.size())
        return fail_screen_draw();
      if (std::fwrite(graphic.sequence.data(), 1, graphic.sequence.size(), stdout) != graphic.sequence.size())
        return fail_screen_draw();
      if (graphic.protocol == TerminalImageProtocol::Kitty && graphic.image_id &&
          std::ranges::find(active_image_ids, *graphic.image_id) == active_image_ids.end())
      {
        active_image_ids.push_back(*graphic.image_id);
      }
      wrote_direct_sequences = true;
    }
  }
  if (wrote_direct_sequences && cursor_visible)
  {
    auto const row = std::min<std::size_t>(cursor.row, terminal_rows > 0 ? terminal_rows - 1 : 0);
    auto const column = std::min<std::size_t>(cursor.column, terminal_cols > 0 ? terminal_cols - 1 : 0);
    auto const move = "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(column + 1) + "H";
    if (std::fwrite(move.data(), 1, move.size(), stdout) != move.size())
      return fail_screen_draw();
  }
  if (std::fflush(stdout) != 0)
    return fail_screen_draw();
  active_image_ids = std::move(current_kitty_image_ids);
  screen_cache.surfaces = std::move(surfaces);
  screen_cache.dirty_rows.assign(screen_cache.surfaces.size(), false);
  screen_cache.graphics = std::move(frame.graphics);
  screen_cache.style_key = style_key;
  screen_cache.width = width;
  screen_cache.height = height;
  screen_cache.valid = true;
  return true;
}

bool detail::draw_processing_footer_cached(ComposerSnapshot const& snapshot, CompletionMatchCache& /*completion_cache*/, std::size_t /*source_revision*/,
                                           TranscriptLayoutCache& /*transcript_cache*/, std::size_t /*transcript_generation*/, ScreenRowCache& screen_cache)
{
  if (!snapshot.processing || snapshot.permission_prompt || snapshot.question_prompt || snapshot.command_output || snapshot.select_list ||
      snapshot.subagent_workspace || sidebar_drawer_active(snapshot))
    return false;

  auto& terminal_context = core::Application::instance().terminal_context();
  auto& window = terminal_context.stdscr();
  auto const terminal_rows = terminal_context.rows();
  auto const terminal_cols = terminal_context.cols();
  auto move_window = [&window](terminal::Position position) {
    if (window.is_initialized())
      window.move(position);
    else
      static_cast<void>(move(static_cast<int>(position.row()), static_cast<int>(position.col())));
  };
  auto const height = std::max<std::size_t>(detail::kMinHeight, snapshot.height);
  auto const canvas = composer_canvas_layout(snapshot);
  auto footer = detail::render_composer_footer_line(snapshot, canvas.content_width);
  if (tui_plain_output())
    footer = strip_sgr_sequences(footer);
  auto const row = std::min<std::size_t>(height - 1, terminal_rows > 0 ? terminal_rows - 1 : 0);
  move_window({static_cast<uint32_t>(row), static_cast<uint32_t>(canvas.left)});
  // Do not clear past the main canvas: that would erase a visible sidebar on every tick.
  draw_styled_line(footer, false);

  auto cursor = input_cursor_placement(snapshot, height, canvas.content_width);
  cursor.column += canvas.left;
  move_window({static_cast<uint32_t>(std::min<std::size_t>(cursor.row, terminal_rows > 0 ? terminal_rows - 1 : 0)),
               static_cast<uint32_t>(std::min<std::size_t>(cursor.column, terminal_cols > 0 ? terminal_cols - 1 : 0))});
  if (window.is_initialized())
  {
    window.leaveok(false);
    window.wnoutrefresh();
    terminal_context.doupdate();
  }
  else if (leaveok(stdscr, FALSE) == ERR || wnoutrefresh(stdscr) == ERR || doupdate() == ERR)
  {
    screen_cache.valid = false;
    return false;
  }
  mark_screen_row_dirty(screen_cache, row);
  return true;
}

bool draw_screen(ComposerSnapshot const& snapshot)
{
  detail::CompletionMatchCache completion_cache;
  detail::TranscriptLayoutCache transcript_cache;
  detail::ScreenRowCache screen_cache;
  return detail::draw_screen_cached(snapshot, completion_cache, snapshot.file_references_generation, transcript_cache, snapshot.transcript_generation,
                                    screen_cache);
}

}  // namespace ava::tui
