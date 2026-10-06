#pragma once

#include "Data.h"
#include "ava/agent/agent_config.h"
#include "ava/debug/print_members_on.h"
#include <vector>

namespace ava::app::frontend {

struct AgentsListSnapshot
{
  std::vector<ava::agent::AgentDefinition> primary_agents_;     // List of primary agents.
  std::vector<ava::agent::AgentDefinition> subagents_;          // List of currently available subagents.

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class AgentsList : public Data
{
 public:
  using Data::Data;

  AgentsListSnapshot snapshot() const;

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(Data)
};

} // namespace ava::app::frontend

