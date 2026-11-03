#include "sys.h"
#include "SelectorHost.h"

#include "debug.h"

namespace ava::tui {

SelectorHost::SelectorHost(ComposerSnapshot& snapshot) : snapshot_(snapshot)
{
  publish();
}

ActiveSelectList SelectorHost::kind() const noexcept
{
  return kind_;
}

std::optional<SelectListView> const& SelectorHost::view() const noexcept
{
  return view_;
}

SelectListItemView const* SelectorHost::selected_item(std::size_t index) const noexcept
{
  return view_ && index < view_->items.size() ? &view_->items[index] : nullptr;
}

std::vector<std::string> SelectorHost::visible_values(std::string const& query) const
{
  std::vector<std::string> values;
  if (!view_)
    return values;
  auto filtered = *view_;
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
  view_ = std::move(view);
  publish();
}

void SelectorHost::replace(SelectListView view)
{
  // Open a selector with its kind before replacing its view.
  ASSERT(kind_ != ActiveSelectList::None);
  view_ = std::move(view);
  publish();
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
  view_.reset();
  publish();
}

void SelectorHost::update_selection(std::size_t index, std::string query)
{
  // Selection input must target an open selector; open() supplies its initial state.
  ASSERT(view_);
  view_->selected_item_index = index;
  view_->query = std::move(query);
  publish();
}

void SelectorHost::set_subtitle(std::string subtitle)
{
  if (!view_)
    return;
  view_->subtitle = std::move(subtitle);
  publish();
}

void SelectorHost::publish()
{
  if (view_)
    snapshot_.select_list = *view_;
  else
    snapshot_.select_list.reset();
}

} // namespace ava::tui
