#include "sys.h"
#include "SelectorHost.h"
#include "SessionSelector.h"
#include "TuiFrontEnd.h"
#include "runtime_internal.h"
#include "runtime_render_internal.h"
#include "terminal/Context.h"

#include <functional>

namespace ava::tui {

SessionSelector::SessionSelector(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                                 terminal::Context& terminal_context)
    : frontend_(frontend), presentation_state_(presentation), selectors_(selectors), renderer_(renderer), terminal_context_(terminal_context)
{
}

bool SessionSelector::open()
{
  cancel_confirmation();
  selectors_.open(ActiveSelectList::Session, frontend_.session_selector_view());
  presentation_state_.snapshot.status = "session selector opened";
  renderer_.transcript_scroll_offset = 0;
  return renderer_.request_render();
}

void SessionSelector::cancel_confirmation()
{
  archive_confirmation_.reset();
}

void SessionSelector::preserve_selection(SelectListInputResult const& input, SelectListView view, std::string status)
{
  cancel_confirmation();
  selectors_.replace_preserving_selection(std::move(view), input.selected_item_index, input.query);
  presentation_state_.snapshot.status = std::move(status);
}

void SessionSelector::browse(SelectListInputResult const& input)
{
  switch (input.action)
  {
    case SelectListInputAction::CycleSort:
      preserve_selection(input, frontend_.on_session_selector_sort_cycle(), "session selector sort cycled");
      break;
    case SelectListInputAction::ToggleNamedFilter:
      preserve_selection(input, frontend_.on_session_selector_named_filter_toggle(), "session selector filter toggled");
      break;
    case SelectListInputAction::TogglePathDisplay:
      preserve_selection(input, frontend_.on_session_selector_path_display_toggle(), "session selector path display toggled");
      break;
    case SelectListInputAction::ToggleArchivedFilter:
      preserve_selection(input, frontend_.on_session_selector_archived_filter_toggle(), "session selector archived filter toggled");
      break;
    case SelectListInputAction::ToggleLabelTimestamp:
      preserve_selection(input, frontend_.on_session_selector_label_timestamp_toggle(), "session selector label timestamps toggled");
      break;
    default:
      break;
  }
}

std::optional<std::string> SessionSelector::draft_command(SelectListInputResult const& input)
{
  bool const rename = input.action == SelectListInputAction::Rename;
  auto const* item = selectors_.selected_item(input.selected_item_index);
  if (!item || !item->enabled || item->value.empty())
  {
    presentation_state_.snapshot.status = rename ? "session cannot be renamed from this row" : "session cannot be labeled from this row";
    terminal_context_.beep();
    return std::nullopt;
  }
  auto const selected_value = item->value;
  selectors_.close();
  presentation_state_.snapshot.status = rename ? "session rename draft ready" : "session labels draft ready";
  return std::string(rename ? "/sessions rename " : "/sessions labels ") + selected_value + " ";
}

core::Result<std::string> SessionSelector::resolve(SelectListInputResult const& input)
{
  cancel_confirmation();
  auto const* item = selectors_.selected_item(input.selected_item_index);
  std::string selected_value = item ? item->value : std::string{};
  return dispatch_tui_selector_authority(presentation_state_.snapshot, selectors_, "opening session…", std::bind_front(&RuntimeRenderer::render, &renderer_),
                                         std::bind_front(&TuiFrontEnd::on_session_selected, &frontend_, std::move(selected_value)));
}

std::optional<core::Result<std::string>> SessionSelector::navigate_branch(SelectListInputResult const& input)
{
  cancel_confirmation();
  auto const* item = selectors_.selected_item(input.selected_item_index);
  if (!item || !item->enabled || item->value.empty())
  {
    presentation_state_.snapshot.status = "session branch navigation unavailable from this row";
    terminal_context_.beep();
    return std::nullopt;
  }
  auto const selected_value = item->value;
  auto callback =
      input.action == SelectListInputAction::BranchParent ? &TuiFrontEnd::on_session_selector_branch_parent : &TuiFrontEnd::on_session_selector_branch_child;
  return dispatch_tui_selector_authority(presentation_state_.snapshot, selectors_, "opening session…", std::bind_front(&RuntimeRenderer::render, &renderer_),
                                         std::bind_front(callback, &frontend_, selected_value));
}

void SessionSelector::archive(SelectListInputResult const& input)
{
  bool const noninvasive_archive = input.action == SelectListInputAction::ArchiveNoninvasive;
  auto const* selected_item = selectors_.selected_item(input.selected_item_index);
  if (selectors_.kind() != ActiveSelectList::Session || !selectors_.view())
  {
    selectors_.close();
    archive_confirmation_.reset();
    presentation_state_.snapshot.status = "view canceled";
  }
  else if (!selected_item || !selected_item->enabled || selected_item->value.empty())
  {
    archive_confirmation_.reset();
    presentation_state_.snapshot.status = "session cannot be archived or restored from this row";
    terminal_context_.beep();
  }
  else
  {
    bool const archive = selected_item->badge != "archived";
    if (archive && selected_item->current)
    {
      archive_confirmation_.reset();
      presentation_state_.snapshot.status = "switch sessions before archiving the active session";
      terminal_context_.beep();
    }
    else if (archive_confirmation_ && archive_confirmation_->session_id == selected_item->value && archive_confirmation_->archive == archive)
    {
      auto updated = archive ? frontend_.on_session_selector_archive(selected_item->value) : frontend_.on_session_selector_unarchive(selected_item->value);
      archive_confirmation_.reset();
      if (updated)
      {
        preserve_selection(input, std::move(*updated), archive ? "session archived" : "session restored");
      }
      else
      {
        presentation_state_.snapshot.status = updated.error().format();
        terminal_context_.beep();
      }
    }
    else
    {
      archive_confirmation_ = PendingSessionArchiveAction{.session_id = selected_item->value, .archive = archive};
      presentation_state_.snapshot.status = std::string("press ") + (noninvasive_archive ? "Ctrl+Backspace" : "Ctrl+D") + " again to " +
                                            (archive ? "archive " : "restore ") + selected_item->value;
    }
  }
}

} // namespace ava::tui
