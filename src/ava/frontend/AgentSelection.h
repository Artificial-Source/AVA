#pragma once

#include "FrontEnd.h"
#include "ava/frontend/UnlockedData.h"
#include "ava/agent/agent_definition_types.h"
#include "utils/macros.h"

#include <atomic>
#include <string>

namespace ava::app::runtime {
struct AgentSelection;
}

namespace ava::frontend {

// Bounded frontend copy of the selected primary agent's display metadata.
struct SelectedAgentMetadata
{
  std::string name;
  agent::AgentDefinitionProvenance provenance = agent::AgentDefinitionProvenance::Unknown;
  agent::AgentToolPreset tool_preset = agent::AgentToolPreset::Inherit;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Project the canonical nonoptional primary-agent receiver without retaining its prompt or source path.
class AgentSelection
{
 private:
  FrontEnd const* frontend_;
  // Exact copies of ava::app::runtime::AgentSelection::selected_primary_agent.{name, provenance, tool_preset}, protected by AgentSelection_ts.
  SelectedAgentMetadata selected_agent_snapshot_;
  DataListIndex index_;
  std::atomic_bool dirty_;

  // Cached strings for immediate display.
  DisplayString agent_;
  DisplayString provenance_;
  DisplayString tool_preset_;

  // Copy the three display-safe fields under one canonical read guard.
  void refresh();

 public:
  // Construct a projection for frontend, which must outlive this object.
  explicit AgentSelection(FrontEnd const* frontend) : frontend_(frontend) { }

  // Connect once to the selected-agent wrapper at its stable registered address.
  void initialize(UnlockedData<app::runtime::AgentSelection> const* unlocked_selection);

  // Mark all selected-agent display fields potentially stale from a backend notification thread.
  void dirty() { dirty_ = true; }

  // Return the primary agent that receives the next submitted turn.
  RefreshResult<std::string const&> agent_refresh()
  {
    if (dirty_.exchange(false))
      refresh();
    bool const changed = agent_.clear_changed();
    if (AI_UNLIKELY(changed))
      agent_.display_ = selected_agent_snapshot_.name;
    return {agent_.display_, changed};
  }

  // Return the selected definition's provenance, consuming only that field's change flag.
  RefreshResult<std::string const&> provenance_refresh()
  {
    if (dirty_.exchange(false))
      refresh();
    bool const changed = provenance_.clear_changed();
    if (AI_UNLIKELY(changed))
      provenance_.display_ = agent::to_string(selected_agent_snapshot_.provenance);
    return {provenance_.display_, changed};
  }

  // Return the selected definition's tool preset, consuming only that field's change flag.
  RefreshResult<std::string const&> tool_preset_refresh()
  {
    if (dirty_.exchange(false))
      refresh();
    bool const changed = tool_preset_.clear_changed();
    if (AI_UNLIKELY(changed))
      tool_preset_.display_ = agent::to_string(selected_agent_snapshot_.tool_preset);
    return {tool_preset_.display_, changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::frontend
