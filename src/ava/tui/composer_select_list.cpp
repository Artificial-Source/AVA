#include "sys.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <utility>

namespace ava::tui {
namespace {

std::string lower_ascii(std::string_view text)
{
  std::string lowered;
  lowered.reserve(text.size());
  for (char const ch : text) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return lowered;
}

std::optional<int> fuzzy_match_score(std::string_view query, std::string_view candidate)
{
  if (query.empty())
    return 0;
  if (candidate.empty())
    return std::nullopt;

  auto const lowered_query = lower_ascii(query);
  auto const lowered_candidate = lower_ascii(candidate);
  auto const contains = lowered_candidate.find(lowered_query);
  if (contains != std::string::npos)
  {
    auto const length_delta = lowered_candidate.size() > lowered_query.size() ? lowered_candidate.size() - lowered_query.size() : std::size_t{0};
    auto const max_score = static_cast<std::size_t>(std::numeric_limits<int>::max());
    auto const capped_delta = std::min<std::size_t>(length_delta, 64);
    if (contains > (max_score - capped_delta) / 4)
      return std::numeric_limits<int>::max();
    return static_cast<int>((contains * 4) + capped_delta);
  }

  std::size_t query_index = 0;
  std::size_t previous_match = std::string::npos;
  int gap_penalty = 0;
  for (std::size_t index = 0; index < lowered_candidate.size() && query_index < lowered_query.size(); ++index)
  {
    if (lowered_candidate[index] != lowered_query[query_index])
      continue;
    if (previous_match != std::string::npos)
    {
      gap_penalty += static_cast<int>(std::min<std::size_t>(index - previous_match - 1, 32));
    }
    previous_match = index;
    ++query_index;
  }

  if (query_index != lowered_query.size())
    return std::nullopt;
  return 1000 + gap_penalty + static_cast<int>(std::min<std::size_t>(lowered_candidate.size(), 128));
}

std::optional<int> item_match_score(SelectListView const& view, SelectListItemView const& item)
{
  if (view.query.empty())
    return 0;
  std::optional<int> best;
  for (auto const field : {std::string_view(item.label), std::string_view(item.description), std::string_view(item.value), std::string_view(item.group),
                           std::string_view(item.detail), std::string_view(item.badge), std::string_view(item.priority_suffix)})
  {
    auto score = fuzzy_match_score(view.query, field);
    if (!score)
      continue;
    if (!best || *score < *best)
      best = *score;
  }
  return best;
}

std::string select_modal_line(std::string content, std::size_t width)
{
  auto const inset = detail::modal_horizontal_inset(width);
  content = detail::fit_line_preserving_sgr(std::move(content), detail::modal_content_width(width));
  return detail::composer_surface_line(std::string(inset, ' ') + std::move(content), width);
}

std::string select_title_line(SelectListView const& view, std::size_t width)
{
  auto title = view.title.empty() ? std::string("Select") : sanitize_terminal_text(view.title);
  std::string line = std::string(detail::kSgrBold) + title + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  if (!view.subtitle.empty())
  {
    line +=
        "  " + std::string(detail::kSgrMuted) + sanitize_terminal_text(view.subtitle) + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  }
  return select_modal_line(std::move(line), width);
}

std::string select_search_line(SelectListView const& view, std::size_t width)
{
  std::string query = sanitize_terminal_text(view.query);
  if (query.empty())
  {
    query = std::string(detail::kSgrMuted) + sanitize_terminal_text(view.placeholder) + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  }
  query += std::string(detail::kSgrAccent) + "█" + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  auto const prefix = view.compact_settings_chrome ? std::string("Search  ") : std::string("filter  ");
  return select_modal_line(std::string(detail::kSgrMuted) + prefix + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg) + std::move(query),
                           width);
}

std::string select_group_line(std::string_view group, std::size_t width)
{
  return select_modal_line(
      std::string(detail::kSgrMuted) + sanitize_terminal_text(group) + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg), width);
}

std::string select_item_line(SelectListItemView const& item, bool selected, std::size_t width)
{
  std::string line = selected ? std::string(detail::kSgrAccent) + "› " + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg) : "  ";
  line += item.current ? "● " : "  ";
  auto const label = sanitize_terminal_text(item.label.empty() ? item.value : item.label);
  if (selected)
    line += std::string(detail::kSgrBold) + label + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  else
    line += label;
  if (!item.priority_suffix.empty())
  {
    auto const priority = "  " + std::string(detail::kSgrMuted) + sanitize_terminal_text(item.priority_suffix) + std::string(detail::kSgrReset) +
                          std::string(detail::kSgrComposerBg);
    auto const content_width = detail::modal_content_width(width);
    auto const priority_width = detail::terminal_text_columns(priority);
    auto const trailing_reserve = !item.description.empty() || !item.detail.empty() || !item.enabled ? std::size_t{3} : std::size_t{0};
    auto const reserved_width = priority_width + trailing_reserve;
    line = detail::fit_line_preserving_sgr(std::move(line), content_width > reserved_width ? content_width - reserved_width : std::size_t{0});
    line += priority;
  }
  else if (!item.badge.empty())
  {
    line += "  " + std::string(detail::kSgrMuted) + sanitize_terminal_text(item.badge) + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  }
  if (!item.description.empty())
  {
    line +=
        "  " + std::string(detail::kSgrMuted) + sanitize_terminal_text(item.description) + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  }
  if (!item.detail.empty())
  {
    line += "  " + std::string(detail::kSgrMuted) + sanitize_terminal_text(item.detail) + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  }
  if (!item.enabled)
  {
    line += "  " + std::string(detail::kSgrWarning) + "disabled";
    if (!item.disabled_reason.empty())
      line += ": " + sanitize_terminal_text(item.disabled_reason);
    line += std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  }
  line = detail::fit_line_preserving_sgr(std::move(line), detail::modal_content_width(width));
  if (!item.enabled)
    line = std::string(detail::kSgrDim) + line + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  return select_modal_line(std::move(line), width);
}

std::string select_item_launch_line(SelectListItemView const& item, std::size_t width)
{
  auto line = std::string("    ") + std::string(detail::kSgrMuted) + "Launch: " + sanitize_terminal_text(item.non_searchable_suffix) +
              std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  line = detail::fit_line_preserving_sgr(std::move(line), detail::modal_content_width(width));
  if (!item.enabled)
    line = std::string(detail::kSgrDim) + line + std::string(detail::kSgrReset) + std::string(detail::kSgrComposerBg);
  return select_modal_line(std::move(line), width);
}

std::string select_footer_line(SelectListView const& view, std::size_t width)
{
  auto const content_width = detail::modal_content_width(width);
  auto const session_selector = view.title.find("session") != std::string::npos || view.title.find("Session") != std::string::npos;
  auto const scoped_models = view.title.find("Scoped model") != std::string::npos;
  std::string hint;
  if (view.compact_settings_chrome && !view.footer_hint.empty())
    hint = sanitize_terminal_text(view.footer_hint);
  else if ((view.title == "Select thinking mode" || view.placeholder.empty()) && !view.footer_hint.empty())
    hint = sanitize_terminal_text(view.footer_hint);
  else if (session_selector && content_width >= 67)
    hint = "↑↓ navigate · Enter open · type filter · Esc close · Ctrl+D archive";
  else if (session_selector && content_width >= 53)
    hint = "↑↓ navigate · Enter open · Esc close · Ctrl+D archive";
  else if (!session_selector && content_width >= 52)
  {
    hint = "↑↓ navigate · Enter select · type filter · Esc close";
    if (scoped_models && content_width >= 67)
      hint += " · Enter toggle";
  }
  else if (content_width >= 44)
    hint = "↑↓ navigate · Enter " + std::string(session_selector ? "open" : "select") + " · Esc close";
  else
    hint = "↑↓ · Enter · Esc";
  return select_modal_line(std::string(detail::kSgrMuted) + std::move(hint) + std::string(detail::kSgrReset), width);
}

struct SelectListContentRow
{
  std::optional<std::size_t> item_index;
  std::string group;
  bool launch_detail = false;
};

std::vector<SelectListContentRow> select_list_content_rows(SelectListView const& view, std::size_t row_budget)
{
  if (row_budget == 0)
    return {};

  auto const matches = filter_select_list_items(view);
  if (matches.empty())
    return {};
  auto const selected = clamp_select_list_selection(view, view.selected_item_index);
  auto const selected_match = std::ranges::find(matches, selected);
  auto const selected_visible = selected_match == matches.end() ? std::size_t{0} : static_cast<std::size_t>(selected_match - matches.begin());

  auto build_rows = [&](std::size_t start) {
    std::vector<SelectListContentRow> rows;
    rows.reserve(row_budget);
    std::string last_group;
    for (auto visible = start; visible < matches.size() && rows.size() < row_budget; ++visible)
    {
      auto const item_index = matches[visible];
      auto const& item = view.items[item_index];
      auto const needs_heading = !item.group.empty() && (visible == start || item.group != last_group);
      if (needs_heading)
      {
        auto const rows_remaining = row_budget - rows.size();
        if (rows_remaining >= 2)
          rows.push_back(SelectListContentRow{.item_index = std::nullopt, .group = item.group});
        else if (!rows.empty())
          break;
      }
      if (rows.size() >= row_budget)
        break;
      rows.push_back(SelectListContentRow{.item_index = item_index, .group = {}});
      if (!item.non_searchable_suffix.empty() && rows.size() < row_budget)
        rows.push_back(SelectListContentRow{.item_index = item_index, .group = {}, .launch_detail = true});
      last_group = item.group;
    }
    return rows;
  };

  auto const baseline_rows = build_rows(selected_visible);
  auto const baseline_shows_selected_launch =
      std::ranges::any_of(baseline_rows, [selected](SelectListContentRow const& row) { return row.item_index == selected && row.launch_detail; });
  auto start = selected_visible;
  // At most row_budget preceding logical items can fit because every item owns
  // a primary row. Rebuild only this bounded candidate window so the first
  // visible grouped item can synthesize its heading without scanning all rows.
  for (auto attempts = std::size_t{0}; start > 0 && attempts < row_budget; ++attempts)
  {
    auto candidate = build_rows(start - 1);
    auto const candidate_shows_selected_primary =
        std::ranges::any_of(candidate, [selected](SelectListContentRow const& row) { return row.item_index == selected && !row.launch_detail; });
    auto const candidate_shows_selected_launch =
        std::ranges::any_of(candidate, [selected](SelectListContentRow const& row) { return row.item_index == selected && row.launch_detail; });
    if (!candidate_shows_selected_primary || (baseline_shows_selected_launch && !candidate_shows_selected_launch))
      break;
    --start;
  }
  return build_rows(start);
}

std::string character_text(InputEvent const& event)
{
  if (!event.text.empty())
    return event.text;
  if (event.character == '\0')
    return {};
  return std::string(1, event.character);
}

bool event_matches_action(InputEvent const& event, TuiKeyBindings const& bindings, TuiAction action)
{
  if (key_matches_action(bindings, action, event.key))
    return true;
  // Some terminals report Shift+L/T as an uppercase character rather than Key::ShiftL/T.
  // Keep lowercase l/t as ordinary filter text. Prefer lowercase filter queries in smokes
  // when these session-tree actions remain bound.
  if (event.key == Key::Character && character_text(event) == "L")
    return key_matches_action(bindings, action, Key::ShiftL);
  if (event.key == Key::Character && character_text(event) == "T")
    return key_matches_action(bindings, action, Key::ShiftT);
  return false;
}

InputEvent select_list_bound_event(InputEvent event, TuiKeyBindings const& bindings)
{
  auto bound = [&](TuiAction action) { return event_matches_action(event, bindings, action); };
  if (bound(TuiAction::SelectConfirm))
    return InputEvent{.key = Key::Enter};
  if (bound(TuiAction::SelectCancel))
    return InputEvent{.key = Key::Escape};
  if (bound(TuiAction::SelectPrev))
    return InputEvent{.key = Key::ArrowUp};
  if (bound(TuiAction::SelectNext))
    return InputEvent{.key = Key::ArrowDown};
  if (bound(TuiAction::SelectPageUp))
    return InputEvent{.key = Key::PageUp};
  if (bound(TuiAction::SelectPageDown))
    return InputEvent{.key = Key::PageDown};
  if (bound(TuiAction::SessionTogglePath))
    return InputEvent{.key = Key::CtrlP};
  if (bound(TuiAction::SessionToggleSort))
    return InputEvent{.key = Key::CtrlS};
  if (bound(TuiAction::SessionToggleNamedFilter))
    return InputEvent{.key = Key::CtrlN};
  if (bound(TuiAction::SessionRename))
    return InputEvent{.key = Key::CtrlR};
  if (bound(TuiAction::SessionArchive))
    return InputEvent{.key = Key::CtrlD};
  if (bound(TuiAction::SessionArchiveNoninvasive))
    return InputEvent{.key = Key::CtrlBackspace};
  if (bound(TuiAction::TreeFoldOrUp))
    return InputEvent{.key = Key::CtrlArrowLeft};
  if (bound(TuiAction::TreeUnfoldOrDown))
    return InputEvent{.key = Key::CtrlArrowRight};
  if (bound(TuiAction::TreeEditLabel))
    return InputEvent{.key = Key::CtrlL};
  if (bound(TuiAction::TreeToggleLabelTimestamp))
    return InputEvent{.key = Key::ShiftT};
  if (bound(TuiAction::TreeFilterLabeledOnly))
    return InputEvent{.key = Key::CtrlN};
  if (bound(TuiAction::TreeFilterAll))
    return InputEvent{.key = Key::CtrlA};
  return event;
}

}  // namespace

std::vector<std::size_t> filter_select_list_items(SelectListView const& view)
{
  if (view.query.empty())
  {
    std::vector<std::size_t> indices;
    indices.reserve(view.items.size());
    for (std::size_t index = 0; index < view.items.size(); ++index) indices.push_back(index);
    return indices;
  }

  std::vector<std::pair<int, std::size_t>> scored;
  scored.reserve(view.items.size());
  for (std::size_t index = 0; index < view.items.size(); ++index)
  {
    auto score = item_match_score(view, view.items[index]);
    if (score)
      scored.push_back({*score, index});
  }
  std::ranges::sort(scored, [](auto const& lhs, auto const& rhs) {
    if (lhs.first != rhs.first)
      return lhs.first < rhs.first;
    return lhs.second < rhs.second;
  });

  std::vector<std::size_t> indices;
  indices.reserve(scored.size());
  for (auto const& [_, index] : scored) indices.push_back(index);
  return indices;
}

std::size_t clamp_select_list_selection(SelectListView const& view, std::size_t selected_index)
{
  auto const matches = filter_select_list_items(view);
  if (matches.empty())
    return 0;
  if (std::ranges::find(matches, selected_index) != matches.end())
    return selected_index;
  return matches.front();
}

std::size_t previous_select_list_selection(SelectListView const& view, std::size_t selected_index)
{
  auto const matches = filter_select_list_items(view);
  if (matches.empty())
    return 0;
  auto const selected = clamp_select_list_selection(view, selected_index);
  auto const current = std::ranges::find(matches, selected);
  auto visible = current == matches.end() ? std::size_t{0} : static_cast<std::size_t>(current - matches.begin());
  visible = visible == 0 ? matches.size() - 1 : visible - 1;
  return matches[visible];
}

std::size_t next_select_list_selection(SelectListView const& view, std::size_t selected_index)
{
  auto const matches = filter_select_list_items(view);
  if (matches.empty())
    return 0;
  auto const selected = clamp_select_list_selection(view, selected_index);
  auto const current = std::ranges::find(matches, selected);
  auto visible = current == matches.end() ? std::size_t{0} : static_cast<std::size_t>(current - matches.begin());
  visible = (visible + 1) % matches.size();
  return matches[visible];
}

std::size_t page_select_list_selection(SelectListView const& view, std::size_t selected_index, bool previous, std::size_t rows)
{
  auto const matches = filter_select_list_items(view);
  if (matches.empty())
    return 0;
  auto const selected = clamp_select_list_selection(view, selected_index);
  auto const current = std::ranges::find(matches, selected);
  auto visible = current == matches.end() ? std::size_t{0} : static_cast<std::size_t>(current - matches.begin());
  if (previous)
  {
    visible = rows > visible ? std::size_t{0} : visible - rows;
  }
  else
  {
    visible = std::min(matches.size() - 1, visible + rows);
  }
  return matches[visible];
}

SelectListInputResult handle_select_list_input(SelectListView const& view, InputEvent event)
{
  SelectListInputResult result{
      .selected_item_index = clamp_select_list_selection(view, view.selected_item_index), .query = view.query, .action = SelectListInputAction::None};
  auto view_with_result = [&]() {
    auto current = view;
    current.query = result.query;
    current.selected_item_index = result.selected_item_index;
    return current;
  };

  switch (event.key)
  {
    case Key::Character: {
      if (auto text = character_text(event); !text.empty())
      {
        result.query += text;
        auto current = view_with_result();
        result.selected_item_index = clamp_select_list_selection(current, result.selected_item_index);
        result.action = SelectListInputAction::Redraw;
      }
      return result;
    }
    case Key::Space:
      result.query += ' ';
      {
        auto current = view_with_result();
        result.selected_item_index = clamp_select_list_selection(current, result.selected_item_index);
        result.action = SelectListInputAction::Redraw;
      }
      return result;
    case Key::Backspace:
    case Key::ShiftBackspace:
      if (!result.query.empty())
      {
        erase_last_utf8_codepoint(result.query);
        auto current = view_with_result();
        result.selected_item_index = clamp_select_list_selection(current, result.selected_item_index);
        result.action = SelectListInputAction::Redraw;
      }
      return result;
    case Key::ArrowUp:
    case Key::MouseWheelUp: {
      auto current = view_with_result();
      result.selected_item_index = previous_select_list_selection(current, result.selected_item_index);
      result.action = SelectListInputAction::Redraw;
      return result;
    }
    case Key::ArrowDown:
    case Key::Tab:
    case Key::MouseWheelDown: {
      auto current = view_with_result();
      result.selected_item_index = next_select_list_selection(current, result.selected_item_index);
      result.action = SelectListInputAction::Redraw;
      return result;
    }
    case Key::Enter: {
      auto current = view_with_result();
      auto const matches = filter_select_list_items(current);
      if (matches.empty())
      {
        result.action = SelectListInputAction::Redraw;
        return result;
      }
      result.selected_item_index = clamp_select_list_selection(current, result.selected_item_index);
      if (result.selected_item_index >= view.items.size() || !view.items[result.selected_item_index].enabled)
      {
        result.action = SelectListInputAction::Redraw;
        return result;
      }
      result.action = SelectListInputAction::Resolve;
      return result;
    }
    case Key::Escape:
    case Key::CtrlC:
      result.action = SelectListInputAction::Cancel;
      return result;
    case Key::CtrlD:
      result.action = SelectListInputAction::Archive;
      return result;
    case Key::CtrlBackspace:
      if (result.query.empty())
        result.action = SelectListInputAction::ArchiveNoninvasive;
      return result;
    case Key::CtrlA:
      result.action = SelectListInputAction::ToggleArchivedFilter;
      return result;
    case Key::ShiftT:
      result.action = SelectListInputAction::ToggleLabelTimestamp;
      return result;
    case Key::PageUp: {
      auto current = view_with_result();
      result.selected_item_index = page_select_list_selection(current, result.selected_item_index, true, 5);
      result.action = SelectListInputAction::Redraw;
      return result;
    }
    case Key::PageDown: {
      auto current = view_with_result();
      result.selected_item_index = page_select_list_selection(current, result.selected_item_index, false, 5);
      result.action = SelectListInputAction::Redraw;
      return result;
    }
    case Key::Home:
    case Key::CtrlHome: {
      auto const matches = filter_select_list_items(view_with_result());
      if (!matches.empty())
      {
        result.selected_item_index = matches.front();
        result.action = SelectListInputAction::Redraw;
      }
      return result;
    }
    case Key::End:
    case Key::CtrlEnd: {
      auto const matches = filter_select_list_items(view_with_result());
      if (!matches.empty())
      {
        result.selected_item_index = matches.back();
        result.action = SelectListInputAction::Redraw;
      }
      return result;
    }
    case Key::CtrlArrowLeft:
    case Key::AltArrowLeft:
      result.action = SelectListInputAction::BranchParent;
      return result;
    case Key::CtrlArrowRight:
    case Key::AltArrowRight:
      result.action = SelectListInputAction::BranchChild;
      return result;
    case Key::ArrowLeft:
    case Key::ArrowRight:
    case Key::ShiftArrowUp:
    case Key::ShiftArrowDown:
    case Key::ShiftArrowLeft:
    case Key::ShiftArrowRight:
    case Key::ShiftCtrlArrowLeft:
    case Key::ShiftCtrlArrowRight:
    case Key::ShiftAltArrowLeft:
    case Key::ShiftAltArrowRight:
    case Key::ShiftHome:
    case Key::ShiftEnd:
    case Key::ShiftCtrlHome:
    case Key::ShiftCtrlEnd:
    case Key::ShiftEnter:
    case Key::CtrlB:
    case Key::CtrlE:
    case Key::CtrlF:
    case Key::CtrlG:
    case Key::CtrlH:
    case Key::CtrlK:
    case Key::CtrlMinus:
    case Key::CtrlN:
      result.action = SelectListInputAction::ToggleNamedFilter;
      return result;
    case Key::CtrlP:
      result.action = SelectListInputAction::TogglePathDisplay;
      return result;
    case Key::CtrlShiftP:
      break;
    case Key::CtrlX:
      result.action = SelectListInputAction::ModelsClearAll;
      return result;
    case Key::CtrlR:
      result.action = SelectListInputAction::Rename;
      return result;
    case Key::CtrlL:
      result.action = SelectListInputAction::Label;
      return result;
    case Key::CtrlS:
    case Key::CtrlT:
      result.action = SelectListInputAction::CycleSort;
      return result;
    case Key::Delete:
    case Key::ShiftDelete:
    case Key::Insert:
    case Key::Clear:
    case Key::ShiftTab:
    case Key::ShiftL:
    case Key::CtrlEnter:
    case Key::AltEnter:
      break;
    case Key::AltArrowUp:
      result.action = SelectListInputAction::ModelsReorderUp;
      return result;
    case Key::AltArrowDown:
      result.action = SelectListInputAction::ModelsReorderDown;
      return result;
    default: // Ignore all other values.
      break;
  }
  return result;
}

SelectListInputResult handle_select_list_input(SelectListView const& view, InputEvent event, TuiKeyBindings const& bindings)
{
  if (event_matches_action(event, bindings, TuiAction::SessionSummarizeParent))
  {
    return SelectListInputResult{.selected_item_index = clamp_select_list_selection(view, view.selected_item_index),
                                 .query = view.query,
                                 .action = SelectListInputAction::SummarizeParent};
  }
  return handle_select_list_input(view, select_list_bound_event(event, bindings));
}

namespace detail {
namespace {

std::vector<std::string> select_list_modal_prefix(SelectListView const& view, std::size_t width, std::size_t max_lines)
{
  std::vector<std::string> lines;
  if (max_lines == 0)
    return lines;
  if (modal_vertical_inset(max_lines) != 0)
    lines.push_back(composer_surface_line({}, width));
  if (lines.size() >= max_lines)
    return lines;
  lines.push_back(select_title_line(view, width));
  if (lines.size() < max_lines && (!view.compact_settings_chrome || !view.query.empty()))
    lines.push_back(select_search_line(view, width));
  return lines;
}

}  // namespace

std::optional<std::size_t> select_list_item_for_modal_row(SelectListView const& view, std::size_t modal_row, std::size_t width, std::size_t max_lines)
{
  auto const prefix = select_list_modal_prefix(view, width, max_lines);
  auto const content_start = prefix.size();
  if (modal_row < content_start)
    return std::nullopt;

  auto const reserved_footer = std::size_t{1} + modal_vertical_inset(max_lines);
  auto const budget = max_lines > content_start + reserved_footer ? max_lines - content_start - reserved_footer : 0;
  auto const content_rows = select_list_content_rows(view, budget);
  auto const content_row = modal_row - content_start;
  if (content_row >= content_rows.size())
    return std::nullopt;
  return content_rows[content_row].item_index;
}

std::vector<std::string> render_select_list_modal(SelectListView const& view, std::size_t width, std::size_t max_lines)
{
  auto lines = select_list_modal_prefix(view, width, max_lines);
  if (lines.size() >= max_lines)
    return lines;

  auto const matches = filter_select_list_items(view);
  auto const selected = clamp_select_list_selection(view, view.selected_item_index);
  auto const reserved_footer = std::size_t{1} + modal_vertical_inset(max_lines);
  auto const budget = max_lines > lines.size() + reserved_footer ? max_lines - lines.size() - reserved_footer : 0;
  auto const content_rows = select_list_content_rows(view, budget);

  if (matches.empty())
  {
    if (lines.size() + reserved_footer <= max_lines)
    {
      auto empty = view.empty_text.empty() ? std::string("No matches") : sanitize_terminal_text(view.empty_text);
      lines.push_back(select_modal_line(std::string(kSgrMuted) + std::move(empty) + std::string(kSgrReset), width));
    }
  }
  else
  {
    for (auto const& row : content_rows)
    {
      if (row.item_index)
      {
        auto const item_index = *row.item_index;
        if (row.launch_detail)
          lines.push_back(select_item_launch_line(view.items[item_index], width));
        else
          lines.push_back(select_item_line(view.items[item_index], item_index == selected, width));
      }
      else
      {
        lines.push_back(select_group_line(row.group, width));
      }
    }
  }

  if (lines.size() < max_lines)
    lines.push_back(select_footer_line(view, width));
  if (modal_vertical_inset(max_lines) != 0 && lines.size() < max_lines)
    lines.push_back(composer_surface_line({}, width));
  return lines;
}

}  // namespace detail

}  // namespace ava::tui
