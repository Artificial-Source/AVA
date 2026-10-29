#include "sys.h"
#include "TuiFrontEnd.h"
#include "command_palette_views.h"      // project_slash_command_items, project_file_reference_items
#include "startup_overview.h"           // build_startup_overview_snapshot
#include "ava/app/runtime/Session.h"
#include "ava/app/command_palette.h"    // SessionSelectorSort
#include "ava/agent/todo.h"             // agent::latest_committed_todowrite_snapshot

namespace ava::app {
namespace {

void append_status_line(std::string& target, std::string line)
{
  if (line.empty())
    return;
  if (!target.empty())
    target += '\n';
  target += std::move(line);
}

std::string session_selector_footer_hint(SessionSelectorSort sort, bool named_only, bool show_paths, bool show_archived, bool show_label_time)
{
  static_cast<void>(sort);
  static_cast<void>(named_only);
  static_cast<void>(show_paths);
  static_cast<void>(show_archived);
  static_cast<void>(show_label_time);
  return "↑↓ navigate · Enter open · type filter · Esc close · Ctrl+D archive";
}

} // namespace

TuiFrontEnd::TuiFrontEnd(
    runtime::session_ts& unlocked_session,
    ApplicationCatalogCache&& application_catalog_cache,
    std::size_t title_catalog_cursor,
    tui::TuiRuntimeOptions&& options) :
  tui::TuiFrontEnd(std::move(options)),
  unlocked_session_(unlocked_session),
  display_settings_(LocalTuiDisplaySettings::create(unlocked_session)),
  application_catalog_(std::move(application_catalog_cache), title_catalog_cursor)
{
}

tui::SelectListView TuiFrontEnd::session_selector_view_from_catalog() const
{
  return application_catalog_session_view(application_catalog(), session_selector_sort_,
                                          session_selector_footer_hint(session_selector_sort_, session_selector_named_only_, session_selector_show_paths_,
                                                                       session_selector_show_archived_, session_selector_show_label_time_),
                                          session_selector_named_only_, session_selector_show_paths_, session_selector_show_archived_,
                                          session_selector_show_label_time_,
                                          ava::tui::keys_display(key_bindings_, ava::tui::TuiAction::SessionSummarizeParent));
}

std::filesystem::path TuiFrontEnd::keybinds_json_path() const
{
  auto const paths = runtime::session_ts::crat(unlocked_session_)->paths();
  return paths.ava_config_dir / "keybinds.json";
}

void TuiFrontEnd::load_display_settings(std::string& status_line)
{
  if (auto loaded = display_settings_.reload({}); !loaded)
    append_status_line(status_line, loaded.error().format());
}

tui::terminal::CursorSettings TuiFrontEnd::cursor_settings() const
{
  return display_settings_.presentation().cursor;
}

tui::TuiLocalDisplaySettings TuiFrontEnd::display_settings_snapshot() const
{
  return display_settings_.presentation();
}

tui::TuiMermaidRenderBridge TuiFrontEnd::mermaid_bridge() const
{
  return display_settings_.mermaid_bridge();
}

std::string TuiFrontEnd::read_committed_session_id() const
{
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);
  return session_r->store.options_r()->session_id;
}

tui::TuiCatalogDelivery TuiFrontEnd::read_catalog_delivery()
{
  auto delivery = application_catalog_.delivery_snapshot();
  return tui::TuiCatalogDelivery{.slash_commands = project_slash_command_items(std::move(delivery.slash_commands)),
                                 .slash_catalog_generation = delivery.slash_catalog_generation,
                                 .file_references = project_file_reference_items(std::move(delivery.file_references)),
                                 .workspace_catalog_generation = delivery.workspace_catalog_generation};
}

std::vector<agent::TodoItem> TuiFrontEnd::read_committed_todos() const
{
  auto read_authority = runtime::session_ts::crat(unlocked_session_)->read_authority_1();
  if (!read_authority)
    return {};
  auto entries = read_authority->load();
  if (!entries)
    return {};
  auto snapshot = agent::latest_committed_todowrite_snapshot(*entries);
  return std::move(snapshot.todos);
}

tui::StartupOverviewSnapshot TuiFrontEnd::read_startup_overview() const
{
  // The builder holds one Session read guard while performing only bounded,
  // in-memory safe aggregation and returns an owning path-filtered DTO.

  // Static empty bindings keep backend-only overview construction free of
  // mutable frontend preferences while avoiding a per-snapshot allocation.
  static tui::TuiKeyBindings const kNoBindings;
  static tui::TuiThemeInfo const kNoTheme{.kind = tui::TuiThemeKind::Dark, .name = {}, .detail = {}, .badge = {}, .palette = {}, .revision = {}};
  return build_startup_overview_snapshot(unlocked_session_, kNoBindings, kNoTheme);
}

core::Result<tui::TuiLocalDisplaySettingsResult> TuiFrontEnd::on_reload_display_settings()
{
  return display_settings_.reload();
}

core::Result<std::optional<tui::TuiLocalDisplaySettingsResult>> TuiFrontEnd::on_maybe_reload_display_settings()
{
  return display_settings_.maybe_reload();
}

core::Result<tui::TuiLocalDisplaySettingsResult> TuiFrontEnd::on_local_display_settings_action(std::string_view action)
{
  return display_settings_.apply_action(action);
}

void TuiFrontEnd::on_before_tui_shutdown()
{
  display_settings_.shutdown();
//  branch_summary_coordinator->shutdown();     FIXME: see atl-list
}

tui::SelectListView TuiFrontEnd::session_selector_snapshot() const
{
  static_cast<void>(refresh_title_catalog());
  return session_selector_view_from_catalog();
}

tui::SelectListView TuiFrontEnd::session_selector_view() const
{
  session_selector_sort_ = ava::app::SessionSelectorSort::Recent;
  session_selector_named_only_ = false;
  session_selector_show_paths_ = false;
  session_selector_show_archived_ = false;
  session_selector_show_label_time_ = false;
  return session_selector_snapshot();
}

} // namespace ava::app
