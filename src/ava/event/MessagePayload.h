#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <string>

namespace ava::event {

// Extracted message slice of an Event: streamed or final text, status, optional error message, and stop reason as emitted by the runtime prompt message path.
struct MessagePayload
{
  ava::core::NoLogString text;
  std::string status;
  ava::core::NoLogString error_message;
  ava::core::NoLogString stop_reason;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::event
