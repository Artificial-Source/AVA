#pragma once

#include "ava/debug/print_members_on.h"

#include <string>

namespace ava::observability {

struct TraceContext
{
  std::string run_id;
  std::string turn_id;
  std::string session_id;
  std::string provider_id;
  // Child runs always own their IDs. These optional fields only correlate a
  // child to its parent and never participate in its lifecycle identity.
  std::string parent_run_id;
  std::string parent_turn_id;
  std::string parent_session_id;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::observability
