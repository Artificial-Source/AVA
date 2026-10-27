#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <string>

namespace ava::event {

// Extracted cancellation slice of an Event: human-readable text, status, the error classification fields and the triggering reason for the cancellation.
struct CancellationPayload
{
  ava::core::NoLogString text;
  std::string status;
  std::string error_category;
  std::string error_code;
  ava::core::NoLogString error_message;
  ava::core::NoLogString error_details;
  std::string trigger;
  ava::core::NoLogString reason;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::event
