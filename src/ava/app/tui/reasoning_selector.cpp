#include "sys.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/tui/reasoning_selector.h"
#include "ava/config/provider_profiles.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

std::string reasoning_level_qualifier(std::string_view level)
{
  constexpr std::size_t kMaximumQualifierLength = 24;
  std::string qualifier;
  qualifier.reserve(std::min(level.size(), kMaximumQualifierLength));
  for (auto const ch : level)
  {
    if (qualifier.size() == kMaximumQualifierLength)
      break;
    auto const ascii_alphanumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    qualifier.push_back(ascii_alphanumeric || ch == '-' || ch == '_' || ch == '.' || ch == '+' ? ch : '-');
  }
  return qualifier.empty() ? std::string("level") : qualifier;
}

void disambiguate_reasoning_labels(std::vector<tui::SelectListItemView>& items)
{
  std::vector<std::string> base_labels;
  base_labels.reserve(items.size());
  for (auto const& item : items)
    base_labels.push_back(item.label);
  std::vector<std::string> used_labels;
  used_labels.reserve(items.size());
  for (std::size_t index = 0; index < items.size(); ++index)
  {
    auto& item = items[index];
    if (!item.enabled)
      continue;
    std::size_t collisions = 0;
    for (std::size_t candidate = 0; candidate < items.size(); ++candidate)
      collisions += items[candidate].enabled && base_labels[candidate] == base_labels[index] ? 1 : 0;
    if (collisions > 1)
      item.label += " (" + (index == 0 ? std::string("automatic") : reasoning_level_qualifier(item.value)) + ")";
    auto const candidate = item.label;
    std::size_t suffix = 2;
    while (std::ranges::find(used_labels, item.label) != used_labels.end())
      item.label = candidate + " " + std::to_string(suffix++);
    used_labels.push_back(item.label);
  }
}

}  // namespace

std::string reasoning_level_label(std::string_view level)
{
  if (level == "xhigh")
    return "Extra high";
  std::string label;
  bool capitalize = true;
  for (auto const ch : level)
  {
    auto const ascii_letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    auto const ascii_digit = ch >= '0' && ch <= '9';
    if (!ascii_letter && !ascii_digit)
    {
      if (!label.empty() && label.back() != ' ')
        label.push_back(' ');
      capitalize = true;
      continue;
    }
    auto const lowercase = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
    label.push_back(capitalize && lowercase >= 'a' && lowercase <= 'z' ? static_cast<char>(lowercase - ('a' - 'A')) : lowercase);
    capitalize = false;
  }
  if (!label.empty() && label.back() == ' ')
    label.pop_back();
  return label.empty() ? std::string("Custom") : label;
}

std::optional<tui::SelectListView> reasoning_selector_view(ava::config::ModelInfo const& model, std::optional<runtime::ReasoningSelection> const& current,
                                                           std::string footer_hint)
{
  std::vector<std::string> levels;
  for (auto const& level : ava::config::supported_reasoning_levels(model))
    if (level != "off" && level != "disabled")
      levels.push_back(level);
  if (levels.empty())
    return std::nullopt;
  tui::SelectListView view{.title = "Select thinking mode",
                           .subtitle = {},
                           .items = {},
                           .selected_item_index = 0,
                           .query = {},
                           .placeholder = "Filter thinking modes",
                           .empty_text = "No thinking modes match",
                           .footer_hint = std::move(footer_hint)};
  auto make_item = [](std::string value, std::string label, bool current_item) {
    return tui::SelectListItemView{.value = std::move(value),
                                   .label = std::move(label),
                                   .description = {},
                                   .group = {},
                                   .detail = {},
                                   .badge = {},
                                   .current = current_item,
                                   .enabled = true,
                                   .disabled_reason = {}};
  };
  view.items.push_back(make_item("default", "Default", !current));
  for (auto const& level : levels)
  {
    auto const is_current = current && current->level == level;
    if (is_current)
      view.selected_item_index = view.items.size();
    view.items.push_back(make_item(level, reasoning_level_label(level), is_current));
  }
  disambiguate_reasoning_labels(view.items);
  return view;
}

std::optional<tui::SelectListView> reasoning_selector_view(runtime::session_ts const& unlocked_session, std::string footer_hint)
{
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
  return reasoning_selector_view(session_r->model(), session_r->reasoning(), std::move(footer_hint));
}

}  // namespace ava::app
