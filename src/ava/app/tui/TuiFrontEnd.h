#pragma once

#include "ava/tui/TuiFrontEnd.h"
#include "ava/app/command_palette.h"    // app::ApplicationCatalogCoordinator

namespace ava::app {

class TuiFrontEnd final : public tui::TuiFrontEnd
{
 private:
  app::runtime::session_ts& unlocked_session_;
  app::ApplicationCatalogCoordinator application_catalog_;

 public:
  TuiFrontEnd(
      app::runtime::session_ts& unlocked_session,
      ApplicationCatalogCache&& application_catalog_cache,
      std::size_t title_catalog_cursor,
      tui::TuiRuntimeOptions&& options) :
    tui::TuiFrontEnd(std::move(options)),
    unlocked_session_(unlocked_session),
    application_catalog_(std::move(application_catalog_cache), title_catalog_cursor)
  { }

  // Implement application integration; interaction with the backend and other AVA-specific state.

  std::string read_committed_session_id() const override;
  tui::TuiCatalogDelivery read_catalog_delivery() override;
  std::vector<agent::TodoItem> read_committed_todos() const override;

  // Accessor (this can be removed once all std::function's of tui::TuiRuntimeOptions have been turned into member functions).
  app::ApplicationCatalogCoordinator& application_catalog() { return application_catalog_; }
  app::ApplicationCatalogCoordinator const& application_catalog() const { return application_catalog_; }

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(tui::TuiFrontEnd)
};

} // namespace ava::app
