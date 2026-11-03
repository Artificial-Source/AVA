// Keeps Settings navigation and reversible display previews in one TUI owner.
// See docs/engineering/tui-architecture.md for the owner relationships.
#include "sys.h"
#include "SelectorHost.h"
#include "SettingsDialog.h"
#include "TuiFrontEnd.h"
#include "runtime_state_internal.h"
#include "terminal/Context.h"

namespace ava::tui {

SettingsDialog::SettingsDialog(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, terminal::Context& terminal_context)
    : frontend_(frontend), presentation_state_(presentation), selectors_(selectors), terminal_context_(terminal_context)
{
}

SettingsDialog::~SettingsDialog()
{
  retire();
  clear_tui_theme_preview();
}

bool SettingsDialog::is_open() const noexcept
{
  return open_;
}

void SettingsDialog::open()
{
  retire();
  if (selectors_.kind() == ActiveSelectList::Overview)
    presentation_state_.clear_startup_overview();
  // Workspace trust is popup-only; a close/reopen reads current backend facts.
  presentation_state_.publish_project_trust(frontend_.read_project_trust());
  rebuild(SettingsSection::Root);
  presentation_state_.publish_status("settings opened");
}

void SettingsDialog::synchronize_selector()
{
  if (selectors_.kind() == ActiveSelectList::Settings)
    ensure_open();
  else
    retire();
}

void SettingsDialog::highlight(std::size_t index, std::string query)
{
  selectors_.update_selection(index, std::move(query));
  preview_highlight();
}

void SettingsDialog::clear_preview()
{
  navigation_.preview.cancel();
  presentation_state_.publish_display_preview(navigation_.preview);
}

void SettingsDialog::begin_baseline()
{
  navigation_.preview.begin(runtime_views::DisplayPresentationBaseline{.show_images = presentation_state_.screen().show_images,
                                                                       .image_width_cells = presentation_state_.screen().image_width_cells,
                                                                       .cursor = presentation_state_.screen().cursor});
}

void SettingsDialog::ensure_open()
{
  if (open_)
    return;
  navigation_.reset();
  begin_baseline();
  open_ = true;
}

void SettingsDialog::rebuild(SettingsSection section, std::string query, std::optional<std::size_t> selected)
{
  ensure_open();
  auto settings_snapshot = presentation_state_.screen();
  settings_snapshot.sidebar = presentation_state_.sidebar();
  // Build rows from authoritative presentation so preview overlays do not rewrite candidate labels.
  settings_snapshot.show_images = navigation_.preview.authoritative.show_images;
  settings_snapshot.image_width_cells = navigation_.preview.authoritative.image_width_cells;
  settings_snapshot.cursor = navigation_.preview.authoritative.cursor;
  // active_tui_theme() consults the presentation overlay; clear it only while constructing rows.
  auto const restage_theme_preview = navigation_.preview.active();
  navigation_.preview.clear_theme_overlay();
  auto view = settings_select_list_view_for_section(&frontend_, section, settings_snapshot, frontend_.key_bindings());
  if (restage_theme_preview)
    navigation_.preview.apply_theme_overlay();
  view.query = std::move(query);
  if (selected)
    view.selected_item_index = *selected;
  view.selected_item_index = clamp_select_list_selection(view, view.selected_item_index);
  selectors_.open(ActiveSelectList::Settings, std::move(view));
  navigation_.section = section;
  presentation_state_.publish_display_preview(navigation_.preview);
}

void SettingsDialog::preview_highlight()
{
  if (selectors_.kind() != ActiveSelectList::Settings || !selectors_.view() || !navigation_.in_display())
  {
    if (navigation_.preview.active())
    {
      navigation_.preview.cancel();
      presentation_state_.publish_display_preview(navigation_.preview);
    }
    return;
  }
  auto const index = selectors_.view()->selected_item_index;
  if (index >= selectors_.view()->items.size())
  {
    navigation_.preview.cancel();
    presentation_state_.publish_display_preview(navigation_.preview);
    return;
  }
  // Only supported Display actions produce an overlay. A non-previewable row cancels
  // the prior highlight so it cannot leave unrelated display values staged.
  auto const& item = selectors_.view()->items[index];
  if (auto overlay = runtime_views::settings_preview_overlay_for_action(item.value, presentation_state_.screen()))
    navigation_.preview.update(std::move(*overlay));
  else
    navigation_.preview.cancel();
  presentation_state_.publish_display_preview(navigation_.preview);
}

void SettingsDialog::close()
{
  if (selectors_.kind() == ActiveSelectList::Settings)
    selectors_.close();
  retire();
}

void SettingsDialog::retire()
{
  if (!open_)
    return;
  clear_preview();
  navigation_.reset();
  open_ = false;
}

void SettingsDialog::cancel()
{
  // Esc in a section returns to root (restoring root query/selection when available).
  // Esc at root closes. If a section is open without a root frame (reload/confirm edge),
  // still pop to root rather than trapping the user in the section.
  if (!navigation_.is_root())
  {
    std::string root_query;
    std::optional<std::size_t> root_selected;
    if (navigation_.root_frame)
    {
      root_query = navigation_.root_frame->query;
      root_selected = navigation_.root_frame->selected_item_index;
    }
    navigation_.root_frame.reset();
    clear_preview();
    begin_baseline();
    rebuild(SettingsSection::Root, std::move(root_query), root_selected);
    presentation_state_.publish_status("settings");
  }
  else
  {
    close();
    presentation_state_.publish_status("view canceled");
  }
}

SettingsDialogResult SettingsDialog::resolve(SelectListInputResult const& input)
{
  using namespace runtime_views;

  // Copy action and navigation before rebuilding/closing can invalidate row storage.
  // The enabled check below uses the pointer only on paths that have not replaced it.
  auto const* selected_item = selectors_.selected_item(input.selected_item_index);
  std::string selected_value = selected_item ? selected_item->value : std::string{};
  auto const current_query = input.query;
  auto const current_selected = input.selected_item_index;

  if (auto section = runtime_views::settings_section_for_action(selected_value))
  {
    // Section Enter rebuilds in place rather than generic teardown-first Resolve.
    navigation_.root_frame =
        runtime_views::SettingsFrameState{.section = SettingsSection::Root, .query = current_query, .selected_item_index = current_selected};
    clear_preview();
    begin_baseline();
    rebuild(*section);
    presentation_state_.publish_status("settings section opened");
  }
  // A cross-feature request retires the preview first. Session receives a typed request
  // or owned DraftCommand, so it does not infer behavior from command spelling.
  else if (selected_value == kSettingsOpenModels)
  {
    close();
    return SettingsDialogRequest::Models;
  }
  else if (selected_value == kSettingsOpenScopedModels)
  {
    close();
    return SettingsDialogRequest::ScopedModels;
  }
  else if (selected_value == kSettingsOpenReasoning)
  {
    close();
    return SettingsDialogRequest::Reasoning;
  }
  else if (selected_value == kSettingsOpenKeybindings)
  {
    close();
    return SettingsDialogRequest::Hotkeys;
  }
  else if (selected_value == kSettingsEditKeybindings)
  {
    close();
    return DraftCommand{.command = "/keybindings set ", .status = "keybinding edit command drafted"};
  }
  else if (selected_value == kSettingsReloadKeybindings)
  {
    close();
    auto reloaded = frontend_.on_reload_key_bindings();
    if (!reloaded)
    {
      presentation_state_.publish_status(reloaded.error().format());
      terminal_context_.beep();
    }
    else
    {
      presentation_state_.apply_local_key_bindings(frontend_, std::move(*reloaded));
      presentation_state_.publish_status("keybindings reloaded");
    }
  }
  else if (selected_value == kSettingsDraftPermissions || selected_value == kSettingsDraftTools || selected_value == kSettingsDraftPlugins ||
           selected_value == kSettingsDraftMcp || selected_value == kSettingsDraftJobs || selected_value == kSettingsDraftSessions ||
           selected_value == kSettingsDraftThinking || selected_value == kSettingsDraftDetails)
  {
    std::string draft_command = "/help";
    if (selected_value == kSettingsDraftPermissions)
      draft_command = "/permissions";
    else if (selected_value == kSettingsDraftTools)
      draft_command = "/details";
    else if (selected_value == kSettingsDraftPlugins)
      draft_command = "/plugins";
    else if (selected_value == kSettingsDraftMcp)
      draft_command = "/mcp";
    else if (selected_value == kSettingsDraftJobs)
      draft_command = "/jobs";
    else if (selected_value == kSettingsDraftSessions)
      draft_command = "/sessions";
    else if (selected_value == kSettingsDraftThinking)
      draft_command = "/thinking";
    else if (selected_value == kSettingsDraftDetails)
      draft_command = "/details";
    close();
    return DraftCommand{.command = std::move(draft_command), .status = "command drafted"};
  }
  else if (selected_value.empty() || (selected_item && !selected_item->enabled))
  {
    presentation_state_.publish_status("settings action unavailable from this row");
    terminal_context_.beep();
  }
  // A preview is temporary presentation only. Restore its baseline before the app
  // persists the confirmed action, then adopt the returned settings as the new baseline.
  else if (navigation_.in_display())
  {
    auto const confirm_token = selected_value;
    clear_preview();
    auto selected = frontend_.on_local_display_settings_action(confirm_token);
    if (selected)
    {
      auto status = selected->status;
      presentation_state_.apply_local_display_settings(frontend_, std::move(selected->settings));
      begin_baseline();
      rebuild(navigation_.section, current_query, current_selected);
      presentation_state_.publish_status(status.empty() ? std::string("display setting saved") : std::move(status));
    }
    else
    {
      presentation_state_.publish_status(selected.error().format());
      begin_baseline();
      terminal_context_.beep();
    }
  }
  else
  {
    // Confirm persists exactly once through the app callback; preview is cleared first so
    // authoritative reload becomes the new baseline.
    auto const confirm_token = selected_value;
    clear_preview();
    auto selected = frontend_.on_settings_selected(confirm_token);
    if (selected)
    {
      presentation_state_.refresh_after_backend_action(frontend_);
      auto status = std::move(*selected);
      presentation_state_.publish_status(status);
      begin_baseline();
      if (navigation_.in_display())
      {
        // Remain in the presentation section with refreshed authoritative rows after a
        // successful confirm. Feedback stays on the transient status surface; local
        // settings actions never append chat/transcript items.
        auto const receipt = status.empty() ? std::string("display setting saved") : status;
        rebuild(navigation_.section, current_query, current_selected);
        presentation_state_.publish_status(receipt);
      }
      else
      {
        close();
      }
    }
    else
    {
      // Error restores latest authoritative presentation and keeps the section open.
      begin_baseline();
      presentation_state_.publish_display_preview(navigation_.preview);
      presentation_state_.publish_status(selected.error().format());
      terminal_context_.beep();
    }
  }
  return SettingsDialogRequest::None;
}

void SettingsDialog::rebase_after_display_reload()
{
  // Applied is an explicit app signal. Always rebase/reapply while settings are open — never
  // infer from post-hydration value equality (overlay can mask true→false authority changes).
  if (open_ && selectors_.kind() == ActiveSelectList::Settings)
  {
    // Capture the actionable selection identity before hydration rebuild can shift indexes.
    std::string selected_action_value;
    std::optional<std::size_t> prior_selected_index;
    std::string query;
    std::string staged_overlay_action;
    if (selectors_.view())
    {
      query = selectors_.view()->query;
      prior_selected_index = selectors_.view()->selected_item_index;
      if (*prior_selected_index < selectors_.view()->items.size())
      {
        auto const& item = selectors_.view()->items[*prior_selected_index];
        if (!item.value.empty())
          selected_action_value = item.value;
      }
    }
    if (navigation_.preview.overlay && !navigation_.preview.overlay->action_token.empty())
      staged_overlay_action = navigation_.preview.overlay->action_token;

    // The reload already hydrated authoritative values. Rebase first, then rebuild rows
    // and align the overlay with the restored action identity before Session paints.
    presentation_state_.reapply_display_preview(navigation_.preview);
    if (navigation_.in_display() && selectors_.view())
    {
      rebuild(navigation_.section, query, std::nullopt);
      if (selectors_.view())
      {
        selectors_.update_selection(
            runtime_views::reselect_settings_display_row_after_rebuild(*selectors_.view(), selected_action_value, staged_overlay_action, prior_selected_index),
            selectors_.view()->query);
        // Keep overlay and Enter target aligned with the restored row identity.
        preview_highlight();
      }
    }
  }
}

} // namespace ava::tui
