#include "sys.h"
#include "SelectorHost.h"

#include <algorithm>
#include <iterator>
#include "debug.h"

namespace ava::tui {

SelectorHost::SelectorHost(ComposerSnapshot& snapshot) : snapshot_(snapshot)
{
  snapshot_.select_list.reset();
}

ActiveSelectList SelectorHost::kind() const noexcept
{
  return kind_;
}

std::optional<SelectListView> const& SelectorHost::view() const noexcept
{
  return snapshot_.select_list;
}

SelectListItemView const* SelectorHost::selected_item(std::size_t index) const noexcept
{
  return snapshot_.select_list && index < snapshot_.select_list->items.size() ? &snapshot_.select_list->items[index] : nullptr;
}

std::vector<std::string> SelectorHost::visible_values(std::string const& query) const
{
  std::vector<std::string> values;
  if (!snapshot_.select_list)
    return values;
  auto filtered = *snapshot_.select_list;
  filtered.query = query;
  for (auto const index : filter_select_list_items(filtered))
  {
    if (index < filtered.items.size() && filtered.items[index].enabled && !filtered.items[index].value.empty())
      values.push_back(filtered.items[index].value);
  }
  return values;
}

void SelectorHost::open(ActiveSelectList kind, SelectListView view)
{
  // A visible selector needs a concrete kind; use close() to remove it.
  ASSERT(kind != ActiveSelectList::None);
  kind_ = kind;
  snapshot_.select_list = std::move(view);
}

void SelectorHost::replace(SelectListView view)
{
  // Open a selector with its kind before replacing its view.
  ASSERT(kind_ != ActiveSelectList::None);
  snapshot_.select_list = std::move(view);
}

void SelectorHost::replace_preserving_selection(SelectListView view, std::size_t selected_index, std::string query)
{
  view.query = std::move(query);
  if (auto const* item = selected_item(selected_index); item && !item->value.empty())
  {
    for (std::size_t index = 0; index < view.items.size(); ++index)
    {
      if (view.items[index].value == item->value)
      {
        view.selected_item_index = index;
        break;
      }
    }
  }
  view.selected_item_index = clamp_select_list_selection(view, view.selected_item_index);
  replace(std::move(view));
}

void SelectorHost::close()
{
  kind_ = ActiveSelectList::None;
  snapshot_.select_list.reset();
}

void SelectorHost::update_selection(std::size_t index, std::string query)
{
  // Selection input must target an open selector; open() supplies its initial state.
  ASSERT(snapshot_.select_list);
  snapshot_.select_list->selected_item_index = index;
  snapshot_.select_list->query = std::move(query);
}

void SelectorHost::set_subtitle(std::string subtitle)
{
  if (!snapshot_.select_list)
    return;
  snapshot_.select_list->subtitle = std::move(subtitle);
}

void SelectorHost::replace_rows(std::size_t first_changed, std::vector<SelectListItemView> rows, std::size_t selected, std::string query, std::string subtitle)
{
  // Open the selector before replacing rows; its title and layout must already exist.
  ASSERT(snapshot_.select_list);
  auto& view = *snapshot_.select_list;
  view.items.resize(std::min(first_changed, view.items.size()));
  view.items.insert(view.items.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
  view.selected_item_index = selected;
  view.query = std::move(query);
  view.subtitle = std::move(subtitle);
}

} // namespace ava::tui
