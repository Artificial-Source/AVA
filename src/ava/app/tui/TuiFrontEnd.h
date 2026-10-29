#pragma once

#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/composer.h"           // tui::SelectListView
#include "ava/app/command_palette.h"    // ApplicationCatalogCoordinator, SessionSelectorSort
#include "ava/app/tui/display_reload.h" // LocalTuiDisplaySettings

namespace ava::app {

class TuiFrontEnd final : public tui::TuiFrontEnd
{
 private:
  runtime::session_ts& unlocked_session_;
  LocalTuiDisplaySettings display_settings_;
  ApplicationCatalogCoordinator application_catalog_;
  SessionSelectorSort session_selector_sort_ = SessionSelectorSort::Recent;
  bool session_selector_named_only_ = false;
  bool session_selector_show_paths_ = false;
  bool session_selector_show_archived_ = false;
  bool session_selector_show_label_time_ = false;

 public:
  tui::SelectListView session_selector_view_from_catalog() const;
  tui::SelectListView TuiFrontEnd::session_selector_snapshot() const;

 public:
  TuiFrontEnd(
      runtime::session_ts& unlocked_session,
      ApplicationCatalogCache&& application_catalog_cache,
      std::size_t title_catalog_cursor,
      tui::TuiRuntimeOptions&& options);

  // Implement application integration; interaction with the backend and other AVA-specific state.

  void load_display_settings(std::string& status_line) override;
  tui::terminal::CursorSettings cursor_settings() const override;
  tui::TuiLocalDisplaySettings display_settings_snapshot() const override;
  tui::TuiMermaidRenderBridge mermaid_bridge() const override;
  std::filesystem::path keybinds_json_path() const override;
  std::string read_committed_session_id() const override;
  tui::TuiCatalogDelivery read_catalog_delivery() override;
  std::vector<agent::TodoItem> read_committed_todos() const override;
  tui::StartupOverviewSnapshot read_startup_overview() const override;
  core::Result<tui::TuiLocalDisplaySettingsResult> on_reload_display_settings() override;
  core::Result<std::optional<tui::TuiLocalDisplaySettingsResult>> on_maybe_reload_display_settings() override;
  core::Result<tui::TuiLocalDisplaySettingsResult> on_local_display_settings_action(std::string_view) override;
  void on_before_tui_shutdown() override;
  tui::SelectListView session_selector_view() const override;

  // Accessors (this can be removed once all std::function's of tui::TuiRuntimeOptions have been turned into member functions).
  ApplicationCatalogCoordinator& application_catalog() { return application_catalog_; }
  ApplicationCatalogCoordinator const& application_catalog() const { return application_catalog_; }
  SessionSelectorSort& session_selector_sort() { return session_selector_sort_; }
  SessionSelectorSort const& session_selector_sort() const { return session_selector_sort_; }
  bool session_selector_named_only() const { return session_selector_named_only_; }
  bool session_selector_show_paths() const { return session_selector_show_paths_; }
  bool session_selector_show_archived() const { return session_selector_show_archived_; }
  bool session_selector_show_label_time() const { return session_selector_show_label_time_; }

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(tui::TuiFrontEnd)
};

} // namespace ava::app
