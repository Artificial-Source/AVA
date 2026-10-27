#include "sys.h"
#include "TuiFrontEnd.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/tui/command_palette_views.h"  // project_slash_command_items, project_file_reference_items
#include "ava/app/tui/interactive_tui_internal.h"

namespace ava::app {

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
  auto snapshot = ava::agent::latest_committed_todowrite_snapshot(*entries);
  return std::move(snapshot.todos);
}

} // namespace ava::app
