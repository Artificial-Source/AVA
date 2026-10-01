#include "sys.h"
#include "ava/tui/command_output.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/event_state.h"
#include "ava/tui/tool_cards.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {
namespace {

constexpr std::size_t kMaxCommandOutputBlocks = 32;
constexpr std::size_t kMaxCommandOutputBytes = 256 * 1024;
constexpr std::size_t kMaxCommandOutputTools = 32;

std::string tool_index_identity(ToolTimelineItem const& tool)
{
  if (!tool.call_id.empty())
    return "call\n" + tool.call_id;
  if (!tool.request_id.empty())
    return "request\n" + tool.request_id;
  if (!tool.correlation_id.empty())
    return "correlation\n" + tool.correlation_id;
  return {};
}

bool tool_index_identity_was_evicted(ComposerSnapshot const& snapshot, std::string const& identity)
{
  return !identity.empty() && std::ranges::find(snapshot.evicted_tool_index_identities, identity) != snapshot.evicted_tool_index_identities.end();
}

void remember_evicted_tool_identity(ComposerSnapshot& snapshot, ToolTimelineItem const& tool)
{
  auto identity = tool_index_identity(tool);
  if (identity.empty())
    return;
  auto existing = std::ranges::find(snapshot.evicted_tool_index_identities, identity);
  if (existing != snapshot.evicted_tool_index_identities.end())
    snapshot.evicted_tool_index_identities.erase(existing);
  snapshot.evicted_tool_index_identities.push_back(std::move(identity));
  if (snapshot.evicted_tool_index_identities.size() > kMaxTuiToolIndexItems)
  {
    snapshot.evicted_tool_index_identities.erase(
        snapshot.evicted_tool_index_identities.begin(),
        snapshot.evicted_tool_index_identities.begin() + static_cast<std::ptrdiff_t>(snapshot.evicted_tool_index_identities.size() - kMaxTuiToolIndexItems));
  }
}

std::size_t utf8_prefix_size(std::string_view text, std::size_t max_bytes) noexcept
{
  auto size = std::min(text.size(), max_bytes);
  while (size > 0 && size < text.size() && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U)
    --size;
  return size;
}

std::string trim_ascii(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    text.remove_suffix(1);
  return std::string(text);
}

std::string sanitize_output_block(std::string_view text)
{
  std::string result;
  auto const lines = split_lines(text);
  for (std::size_t index = 0; index < lines.size(); ++index)
  {
    if (index > 0)
      result.push_back('\n');
    result += sanitize_terminal_text(lines[index]);
  }
  return result;
}

std::size_t command_output_body_height(std::size_t height)
{
  return height > 2 ? height - 2 : std::size_t{0};
}

std::string modal_line(std::string content, std::size_t width)
{
  auto const inset = detail::modal_horizontal_inset(width);
  content = detail::fit_line_preserving_sgr(std::move(content), detail::modal_content_width(width));
  return detail::composer_surface_line(std::string(inset, ' ') + std::move(content), width);
}

std::vector<std::string> command_output_content_lines(CommandOutputView const& view, std::size_t width, ToolPresentation presentation)
{
  auto const content_width = std::max<std::size_t>(detail::modal_content_width(width), 1);
  std::vector<std::string> lines;
  auto add_gap = [&]() {
    if (!lines.empty() && !lines.back().empty())
      lines.emplace_back();
  };
  for (auto const& block : view.blocks)
  {
    add_gap();
    for (auto const& raw_line : split_lines(block))
    {
      auto wrapped = detail::wrap_transcript_text(sanitize_terminal_text(raw_line), content_width);
      lines.insert(lines.end(), std::make_move_iterator(wrapped.begin()), std::make_move_iterator(wrapped.end()));
    }
  }
  for (auto const& tool : view.tools)
  {
    add_gap();
    auto rendered = detail::render_tool_card(tool, content_width, presentation);
    lines.insert(lines.end(), std::make_move_iterator(rendered.begin()), std::make_move_iterator(rendered.end()));
  }
  if (lines.empty())
    lines.push_back(std::string(detail::kSgrMuted) + "Command completed without output" + std::string(detail::kSgrReset));
  return lines;
}

std::string command_output_footer(CommandOutputView const& view, std::size_t width, std::size_t height, std::size_t line_count)
{
  auto const body_height = command_output_body_height(height);
  auto const max_scroll = line_count > body_height ? line_count - body_height : std::size_t{0};
  auto const scroll = std::min(view.scroll_offset, max_scroll);
  std::string hint;
  if (max_scroll > 0)
  {
    auto const first = scroll + 1;
    auto const last = std::min(line_count, scroll + body_height);
    hint = std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(line_count) + " · ↑↓/PgUp/PgDn scroll · Enter close";
  }
  else
  {
    hint = "Enter/Esc close";
  }
  return modal_line(std::string(detail::kSgrMuted) + std::move(hint) + std::string(detail::kSgrReset), width);
}

}  // namespace

TuiSubmissionProjectionPolicy tui_submission_projection_policy(bool is_command_submission, bool ordinary_turn_committed) noexcept
{
  if (!is_command_submission || ordinary_turn_committed)
  {
    return {.project_conversation = true, .preserve_transcript = false, .present_command_output = false};
  }
  return {.project_conversation = false, .preserve_transcript = true, .present_command_output = true};
}

TuiSubmissionSettlement settle_tui_submission(std::vector<TranscriptItem> const& submitted_transcript, TuiEventState const& buffered_event_state,
                                              std::string_view submitted, bool is_command_submission, bool ordinary_turn_committed,
                                              std::vector<std::string> output, std::vector<ToolTimelineItem> tools, bool canceled, bool present_command_output)
{
  auto settlement = TuiSubmissionSettlement{
      .policy = tui_submission_projection_policy(is_command_submission, ordinary_turn_committed), .transcript = {}, .command_output = {}, .command_tools = {}};
  settlement.policy.present_command_output = settlement.policy.present_command_output || present_command_output;
  if (settlement.policy.preserve_transcript)
  {
    settlement.transcript = submitted_transcript;
  }
  else
  {
    settlement.transcript = event_state_transcript_snapshot(buffered_event_state, PendingTextProjection::Unparsed);
    if (is_command_submission)
      remove_literal_command_invocation(settlement.transcript, submitted);
  }

  if (settlement.policy.present_command_output)
  {
    if (output.empty() && buffered_event_state.run_status == TuiEventRunStatus::Error)
    {
      output.push_back(buffered_event_state.error_text.empty() ? std::string("local command failed") : buffered_event_state.error_text);
      if (!buffered_event_state.error_details.empty())
        output.push_back(buffered_event_state.error_details);
    }
    if (output.empty() && canceled)
      output.push_back("stopped by user");
    settlement.command_output = std::move(output);
    settlement.command_tools = std::move(tools);
  }
  return settlement;
}

void remove_literal_command_invocation(std::vector<TranscriptItem>& items, std::string_view submitted)
{
  std::erase_if(items, [&](TranscriptItem const& item) { return item.label == "you" && item.text == submitted; });
}

std::string command_output_title_token(std::string_view submitted)
{
  auto const trimmed = trim_ascii(submitted);
  auto const text = std::string_view(trimmed);
  if (text.starts_with("!!"))
    return "!!";
  if (text.starts_with('!'))
    return "!";
  auto const end = text.find_first_of(" \t\r\n");
  auto token = ava::core::json::replace_invalid_utf8(sanitize_terminal_text(text.substr(0, end == std::string_view::npos ? text.size() : end)));
  if (!token.starts_with('/'))
    token = "command";
  constexpr std::size_t kMaxTitleBytes = 64;
  token.resize(utf8_prefix_size(token, kMaxTitleBytes));
  return token.empty() ? std::string("command") : token;
}

CommandOutputView make_command_output_view(std::string_view submitted, std::vector<std::string> const& output, std::vector<ToolTimelineItem> const& tools)
{
  CommandOutputView view;
  view.title_token = command_output_title_token(submitted);
  std::size_t retained_bytes = 0;
  for (auto const& block : output)
  {
    if (view.blocks.size() >= kMaxCommandOutputBlocks || retained_bytes >= kMaxCommandOutputBytes)
    {
      view.truncated = true;
      break;
    }
    auto sanitized = sanitize_output_block(ava::core::json::replace_invalid_utf8(block));
    auto const remaining = kMaxCommandOutputBytes - retained_bytes;
    auto const bounded_size = utf8_prefix_size(sanitized, remaining);
    if (bounded_size < sanitized.size())
      view.truncated = true;
    sanitized.resize(bounded_size);
    retained_bytes += sanitized.size();
    view.blocks.push_back(std::move(sanitized));
  }
  auto const tool_count = std::min(tools.size(), kMaxCommandOutputTools);
  view.tools.insert(view.tools.end(), tools.begin(), tools.begin() + static_cast<std::ptrdiff_t>(tool_count));
  if (tool_count < tools.size())
    view.truncated = true;
  if (view.truncated && view.blocks.size() < kMaxCommandOutputBlocks)
    view.blocks.push_back("[command output truncated by TUI presentation limits]");
  return view;
}

void open_command_output(ComposerSnapshot& snapshot, std::string_view submitted, std::vector<std::string> output, std::vector<ToolTimelineItem> tools)
{
  snapshot.command_output = make_command_output_view(submitted, output, tools);
  snapshot.local_command_feedback.reset();
  snapshot.status = "command output";
}

void open_command_error(ComposerSnapshot& snapshot, std::string_view submitted, std::string error)
{
  open_command_output(snapshot, submitted, {std::move(error)});
  snapshot.status = "command failed";
}

void settle_local_command_status(ComposerSnapshot& snapshot, std::string status)
{
  snapshot.status = status;
  snapshot.local_command_feedback = std::move(status);
}

void settle_local_command_completion(ComposerSnapshot& snapshot, std::string_view submitted, std::vector<std::string> output,
                                     std::vector<ToolTimelineItem> tools, bool record_tools)
{
  if (record_tools)
  {
    for (auto const& tool : tools)
      record_tui_tool(snapshot, tool, TuiToolIndexOrigin::LocalCommand);
  }
  if (output.empty() && tools.empty())
  {
    snapshot.command_output.reset();
    settle_local_command_status(snapshot, "command complete");
    return;
  }
  open_command_output(snapshot, submitted, std::move(output), std::move(tools));
}

void seed_tui_tool_index(ComposerSnapshot& snapshot)
{
  snapshot.tool_index.clear();
  snapshot.evicted_tool_index_identities.clear();
  snapshot.next_tool_index_sequence = 0;
  for (auto const& item : snapshot.transcript)
  {
    if (item.tool)
      record_tui_tool(snapshot, *item.tool, TuiToolIndexOrigin::Provider);
  }
}

void record_tui_tool(ComposerSnapshot& snapshot, ToolTimelineItem tool, TuiToolIndexOrigin origin)
{
  auto const identity = tool_index_identity(tool);
  if (!identity.empty())
  {
    auto existing = std::ranges::find_if(snapshot.tool_index, [&](TuiToolIndexEntry const& entry) { return tool_index_identity(entry.tool) == identity; });
    if (existing != snapshot.tool_index.end())
    {
      existing->tool = std::move(tool);
      return;
    }
    if (tool_index_identity_was_evicted(snapshot, identity))
      return;
  }

  snapshot.tool_index.push_back(TuiToolIndexEntry{.tool = std::move(tool), .origin = origin, .sequence = snapshot.next_tool_index_sequence++});
  if (snapshot.tool_index.size() <= kMaxTuiToolIndexItems)
    return;
  auto const remove_count = snapshot.tool_index.size() - kMaxTuiToolIndexItems;
  for (std::size_t index = 0; index < remove_count; ++index)
    remember_evicted_tool_identity(snapshot, snapshot.tool_index[index].tool);
  snapshot.tool_index.erase(snapshot.tool_index.begin(), snapshot.tool_index.begin() + static_cast<std::ptrdiff_t>(remove_count));
}

void update_indexed_tui_tool(ComposerSnapshot& snapshot, ToolTimelineItem const& tool)
{
  auto const identity = tool_index_identity(tool);
  if (identity.empty())
    return;
  auto existing = std::ranges::find_if(snapshot.tool_index, [&](TuiToolIndexEntry const& entry) { return tool_index_identity(entry.tool) == identity; });
  if (existing != snapshot.tool_index.end())
    existing->tool = tool;
}

std::optional<TuiToolIndexEntry> latest_matching_indexed_tool(ComposerSnapshot const& snapshot, std::string_view query)
{
  for (auto entry = snapshot.tool_index.rbegin(); entry != snapshot.tool_index.rend(); ++entry)
  {
    if (detail::tool_card_matches_copy_query(entry->tool, query))
      return *entry;
  }
  return std::nullopt;
}

std::optional<std::size_t> indexed_provider_tool_transcript_index(ComposerSnapshot const& snapshot, TuiToolIndexEntry const& entry)
{
  if (entry.origin != TuiToolIndexOrigin::Provider)
    return std::nullopt;
  auto const identity = tool_index_identity(entry.tool);
  if (identity.empty())
    return std::nullopt;
  for (std::size_t index = snapshot.transcript.size(); index > 0; --index)
  {
    auto const& item = snapshot.transcript[index - 1];
    if (item.tool && tool_index_identity(*item.tool) == identity)
      return index - 1;
  }
  return std::nullopt;
}

std::optional<std::string> latest_indexed_tool_copy_text(ComposerSnapshot const& snapshot, std::string_view query)
{
  for (auto entry = snapshot.tool_index.rbegin(); entry != snapshot.tool_index.rend(); ++entry)
  {
    if (!detail::tool_card_matches_copy_query(entry->tool, query))
      continue;
    auto text = detail::tool_card_copy_text(entry->tool);
    if (!text.empty())
      return text;
  }
  return std::nullopt;
}

std::optional<std::string> latest_indexed_tool_diff_copy_text(ComposerSnapshot const& snapshot, std::string_view query)
{
  for (auto entry = snapshot.tool_index.rbegin(); entry != snapshot.tool_index.rend(); ++entry)
  {
    if (!detail::tool_card_matches_copy_query(entry->tool, query))
      continue;
    auto text = detail::tool_card_diff_copy_text(entry->tool);
    if (!text.empty())
      return text;
  }
  return std::nullopt;
}

std::optional<std::string> latest_indexed_permission_copy_text(ComposerSnapshot const& snapshot, std::string_view query)
{
  for (auto entry = snapshot.tool_index.rbegin(); entry != snapshot.tool_index.rend(); ++entry)
  {
    auto text = detail::tool_card_permission_copy_text(entry->tool, query);
    if (!text.empty())
      return text;
  }
  return std::nullopt;
}

CommandOutputGeometry command_output_geometry(std::size_t terminal_width, std::size_t terminal_height) noexcept
{
  auto width = terminal_width;
  if (width >= 48)
    width = std::min<std::size_t>(120, std::max<std::size_t>(48, (width * 19) / 20));
  auto height = terminal_height;
  if (height >= 10)
    height = std::min<std::size_t>(30, height - 4);
  return {.width = width, .height = height};
}

std::size_t command_output_max_scroll_offset(CommandOutputView const& view, std::size_t width, std::size_t height, ToolPresentation presentation)
{
  auto const lines = command_output_content_lines(view, width, presentation);
  auto const body_height = command_output_body_height(height);
  return lines.size() > body_height ? lines.size() - body_height : std::size_t{0};
}

CommandOutputInputResult handle_command_output_input(CommandOutputView const& view, InputEvent event, std::size_t width, std::size_t height,
                                                     ToolPresentation presentation)
{
  auto const max_scroll = command_output_max_scroll_offset(view, width, height, presentation);
  auto result = CommandOutputInputResult{.scroll_offset = std::min(view.scroll_offset, max_scroll)};
  auto const page = std::max<std::size_t>(command_output_body_height(height), 1);
  using Key = terminal::Key;
  switch (event.key)
  {
    case Key::Escape:
    case Key::CtrlC:
    case Key::Enter:
      result.action = CommandOutputInputAction::Dismiss;
      return result;
    case Key::ArrowUp:
    case Key::MouseWheelUp:
      result.scroll_offset = result.scroll_offset == 0 ? 0 : result.scroll_offset - 1;
      result.action = CommandOutputInputAction::Redraw;
      return result;
    case Key::ArrowDown:
    case Key::MouseWheelDown:
      result.scroll_offset = std::min(max_scroll, result.scroll_offset + 1);
      result.action = CommandOutputInputAction::Redraw;
      return result;
    case Key::PageUp:
      result.scroll_offset = result.scroll_offset > page ? result.scroll_offset - page : 0;
      result.action = CommandOutputInputAction::Redraw;
      return result;
    case Key::PageDown:
      result.scroll_offset = std::min(max_scroll, result.scroll_offset + page);
      result.action = CommandOutputInputAction::Redraw;
      return result;
    case Key::Home:
    case Key::CtrlHome:
      result.scroll_offset = 0;
      result.action = CommandOutputInputAction::Redraw;
      return result;
    case Key::End:
    case Key::CtrlEnd:
      result.scroll_offset = max_scroll;
      result.action = CommandOutputInputAction::Redraw;
      return result;
    default:
      return result;
  }
}

std::vector<std::string> detail::render_command_output_modal(CommandOutputView const& view, std::size_t width, std::size_t height,
                                                             ToolPresentation presentation)
{
  width = std::max<std::size_t>(width, 1);
  height = std::max<std::size_t>(height, 1);
  auto const content = command_output_content_lines(view, width, presentation);
  auto const body_height = command_output_body_height(height);
  auto const max_scroll = content.size() > body_height ? content.size() - body_height : std::size_t{0};
  auto const scroll = std::min(view.scroll_offset, max_scroll);

  std::vector<std::string> lines;
  lines.reserve(height);
  auto title = std::string(detail::kSgrBold) + "Command " + sanitize_terminal_text(view.title_token) + std::string(detail::kSgrReset) +
               std::string(detail::kSgrComposerBg);
  lines.push_back(modal_line(std::move(title), width));
  for (std::size_t row = 0; row < body_height; ++row)
  {
    auto const index = scroll + row;
    lines.push_back(modal_line(index < content.size() ? content[index] : std::string{}, width));
  }
  if (height > 1)
    lines.push_back(command_output_footer(view, width, height, content.size()));
  if (lines.size() > height)
    lines.resize(height);
  while (lines.size() < height)
    lines.push_back(modal_line({}, width));
  return lines;
}

std::vector<std::string> render_command_output(CommandOutputView const& view, std::size_t width, std::size_t height, ToolPresentation presentation)
{
  return detail::render_command_output_modal(view, width, height, presentation);
}

}  // namespace ava::tui
