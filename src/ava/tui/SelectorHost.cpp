// Coordinates selector input around the single view stored for display.
// See docs/engineering/tui-architecture.md for the owner relationships.
#include "sys.h"
#include "SelectorHost.h"
#include "SelectorInputController.h"
#include "SessionSelector.h"
#include "SettingsDialog.h"

#include <algorithm>
#include <iterator>
#include "debug.h"

namespace ava::tui {

SelectorHost::SelectorHost(RuntimePresentationState& presentation) : presentation_(presentation)
{
  presentation_.publish_selector(std::nullopt);
}

ActiveSelectList SelectorHost::kind() const noexcept
{
  return kind_;
}

std::optional<SelectListView> const& SelectorHost::view() const noexcept
{
  return presentation_.screen().select_list;
}

SelectListItemView const* SelectorHost::selected_item(std::size_t index) const noexcept
{
  return presentation_.screen().select_list && index < presentation_.screen().select_list->items.size() ? &presentation_.screen().select_list->items[index]
                                                                                                        : nullptr;
}

std::vector<std::string> SelectorHost::visible_values(std::string const& query) const
{
  std::vector<std::string> values;
  if (!presentation_.screen().select_list)
    return values;
  auto filtered = *presentation_.screen().select_list;
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
  presentation_.publish_selector(std::move(view));
}

void SelectorHost::replace(SelectListView view)
{
  // Open a selector with its kind before replacing its view.
  ASSERT(kind_ != ActiveSelectList::None);
  presentation_.publish_selector(std::move(view));
}

void SelectorHost::replace_preserving_selection(SelectListView view, std::size_t selected_index, std::string query)
{
  // Resolve identity while the old view is still alive; replacing it invalidates
  // its row pointers. A missing identity falls back to the new view selection, then clamps.
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
  presentation_.publish_selector(std::nullopt);
}

void SelectorHost::update_selection(std::size_t index, std::string query)
{
  // Selection input must target an open selector; open() supplies its initial state.
  ASSERT(presentation_.screen().select_list);
  presentation_.publish_selector_selection(index, std::move(query));
}

void SelectorHost::set_subtitle(std::string subtitle)
{
  if (!presentation_.screen().select_list)
    return;
  presentation_.publish_selector_subtitle(std::move(subtitle));
}

void SelectorHost::replace_rows(std::size_t first_changed, std::vector<SelectListItemView> rows, std::size_t selected, std::string query, std::string subtitle)
{
  // Open the selector before replacing rows; its title and layout must already exist.
  ASSERT(presentation_.screen().select_list);
  presentation_.publish_selector_rows(first_changed, std::move(rows), selected, std::move(query), std::move(subtitle));
}

SelectorDispatchResult SelectorHost::dispatch(runtime_input::RuntimeInput const& input, SelectorInputController& classifier, SettingsDialog& settings,
                                              SessionSelector& sessions)
{
  // Retire a displaced Settings preview before this input can open another surface.
  // Archive confirmation also belongs only to the currently hosted session list.
  settings.synchronize_selector();
  if (kind() != ActiveSelectList::Session)
    sessions.cancel_confirmation();
  auto classified = classifier.classify(input);
  using Action = SelectListInputAction;
  using Request = SelectorDispatchRequest;
  // Selection movement disarms two-press archive intent. Settings additionally previews
  // the highlight; generic lists only publish selection/query into the existing rows.
  if (classified.action == Action::Redraw && view())
  {
    sessions.cancel_confirmation();
    if (kind() == ActiveSelectList::Settings)
      settings.highlight(classified.selected_item_index, std::move(classified.query));
    else
      update_selection(classified.selected_item_index, std::move(classified.query));
  }
  else if (classified.action == Action::Cancel && kind() == ActiveSelectList::Settings)
    settings.cancel();
  else if (classified.action == Action::Resolve && kind() == ActiveSelectList::Settings)
  {
    auto result = settings.resolve(classified);
    if (auto* draft = std::get_if<DraftCommand>(&result))
      return SelectorDraftCompletion{.command = std::move(draft->command), .status = std::move(draft->status)};
    switch (std::get<SettingsDialogRequest>(result))
    {
      case SettingsDialogRequest::None:
        return {};
      case SettingsDialogRequest::Models:
        return Request::Models;
      case SettingsDialogRequest::ScopedModels:
        return Request::ScopedModels;
      case SettingsDialogRequest::Reasoning:
        return Request::Reasoning;
      case SettingsDialogRequest::Hotkeys:
        return Request::Hotkeys;
    }
  }
  // Summary preparation needs an owned restoration checkpoint because progress replaces
  // the current view; ordinary selection updates need no such copy.
  else if (classified.action == Action::SummarizeParent && kind() == ActiveSelectList::Session && view())
  {
    sessions.cancel_confirmation();
    auto const* item = selected_item(classified.selected_item_index);
    if (!item || !item->enabled || item->value.empty())
    {
      presentation_.publish_status("parent summary is unavailable from this row");
      return Request::Beep;
    }
    // Copy only when transferring this selector into the summary operation. Ordinary
    // selection changes keep the host's single row allocation in place.
    auto prior = *view();
    prior.query = std::move(classified.query);
    prior.selected_item_index = classified.selected_item_index;
    auto identity = item->value;
    return SelectorParentSummary{.selected_source = std::move(identity), .previous_view = std::move(prior), .selected_index = classified.selected_item_index};
  }
  else if (kind() == ActiveSelectList::Session &&
           (classified.action == Action::CycleSort || classified.action == Action::ToggleNamedFilter || classified.action == Action::TogglePathDisplay ||
            classified.action == Action::ToggleArchivedFilter || classified.action == Action::ToggleLabelTimestamp))
    sessions.browse(classified);
  else if (kind() == ActiveSelectList::Session && (classified.action == Action::Rename || classified.action == Action::Label))
  {
    if (auto command = sessions.draft_command(classified))
      return SelectorDraftCommand{.command = std::move(*command)};
  }
  else if (kind() == ActiveSelectList::Session && (classified.action == Action::BranchParent || classified.action == Action::BranchChild))
  {
    if (auto result = sessions.navigate_branch(classified))
      return *result ? SelectorDispatchResult{SelectorOpenedSession{.feedback = std::move(**result)}}
                     : SelectorDispatchResult{SelectorSessionError{.feedback = result->error().format()}};
  }
  else if (classified.action == Action::Archive || classified.action == Action::ArchiveNoninvasive)
  {
    // Closing nested Settings must retire its temporary preview before another
    // surface can observe local display settings.
    if (kind() == ActiveSelectList::Settings)
    {
      settings.close();
      presentation_.publish_status("view canceled");
    }
    else
      sessions.archive(classified);
  }
  else if (classified.action == Action::Resolve && kind() == ActiveSelectList::Session)
  {
    auto result = sessions.resolve(classified);
    return result ? SelectorDispatchResult{SelectorOpenedSession{.feedback = std::move(*result)}}
                  : SelectorDispatchResult{SelectorSessionError{.feedback = result.error().format()}};
  }
  else
  {
    if (classified.action == Action::Resolve || classified.action == Action::Cancel)
      sessions.cancel_confirmation();
    // Catalog policy returns owned typed outcomes. Translate them after any view closure,
    // leaving draft installation and committed-session reconciliation to Session.
    auto result = classifier.handle(classified);
    if (auto const* request = std::get_if<SelectorInputRequest>(&result))
    {
      if (*request == SelectorInputRequest::RenderFailed)
        return Request::RenderFailed;
      if (*request == SelectorInputRequest::OpenReasoning)
        return Request::CurrentReasoning;
    }
    else
    {
      auto& [kind, text] = std::get<std::pair<SelectorInputTextKind, std::string>>(result);
      switch (kind)
      {
        case SelectorInputTextKind::DraftCommand:
          return SelectorDraftCompletion{.command = std::move(text), .status = "keybinding edit command drafted"};
        case SelectorInputTextKind::OpenedSession:
          return SelectorOpenedSession{.feedback = std::move(text)};
        case SelectorInputTextKind::ReconcileSessionError:
          return SelectorSessionError{.feedback = std::move(text)};
      }
    }
  }
  return {};
}

} // namespace ava::tui
