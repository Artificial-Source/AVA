#pragma once

#include "Data.h"
#include "ava/debug/print_members_on.h"
#include <string>
#include <vector>

namespace ava::app::frontend {

struct CommandListEntry
{
  std::string command_;                         // The full name of the command without leading '/'.
  std::string description_;                     // A short human-readable description of the command.

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandsListSnapshot
{
  std::vector<CommandListEntry> commands_;      // List of all available slash commands.

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class CommandsList : public Data
{
 public:
  using Data::Data;

  CommandsListSnapshot snapshot() const;

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(Data)
};

} // namespace ava::app::frontend
