#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/frontend/UnlockedData.h"
#include "ava/agent/agent_config.h"

#include <ostream>

namespace ava::app::runtime {

// Canonical primary-agent execution selection. The definition is always usable;
// invocation intent remains separate in InvocationInputs.
struct AgentSelection
{
  ava::agent::AgentDefinition selected_primary_agent;

  // Agent instructions and source paths are sensitive and potentially large;
  // diagnostics expose only the definition's bounded metadata printer.
  void print_on(std::ostream& os) const
  {
    os << "{selected_primary_agent:";
    selected_primary_agent.print_on(os);
    os << '}';
  }

  // We have a custom print_on.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using AgentSelection_ts = frontend::UnlockedData<AgentSelection>;

} // namespace ava::app::runtime
