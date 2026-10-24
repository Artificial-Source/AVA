#include "sys.h"
#include "AgentSelection.h"
#include "FrontEnd.h"
#include "ava/app/runtime/AgentSelection.h"

#include <utility>

namespace ava::frontend {

// Copy only bounded selection metadata while one read guard protects the complete source definition.
void AgentSelection::refresh()
{
  Data const* data = frontend_->get_data(index_);
  // index_ must identify the stable AgentSelection_ts registered during frontend initialization.
  ASSERT(dynamic_cast<app::runtime::AgentSelection_ts const*>(data) != nullptr);
  auto const* unlocked_selection = static_cast<app::runtime::AgentSelection_ts const*>(data);
  app::runtime::AgentSelection_ts::crat selection_r(*unlocked_selection);
  auto const& definition = selection_r->selected_primary_agent;
  SelectedAgentMetadata next{.name = definition.name, .provenance = definition.provenance, .tool_preset = definition.tool_preset};

  agent_.changed_ = agent_.changed_ || selected_agent_snapshot_.name != next.name;
  provenance_.changed_ = provenance_.changed_ || selected_agent_snapshot_.provenance != next.provenance;
  tool_preset_.changed_ = tool_preset_.changed_ || selected_agent_snapshot_.tool_preset != next.tool_preset;
  selected_agent_snapshot_ = std::move(next);
}

// Store the registered index and require an initial bounded snapshot.
void AgentSelection::initialize(app::runtime::AgentSelection_ts const* unlocked_selection)
{
  // Call initialize exactly once for each frontend projection.
  ASSERT(index_.undefined());
  index_ = unlocked_selection->index();
  dirty_ = true;
}

}  // namespace ava::frontend
