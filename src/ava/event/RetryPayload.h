#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <string>

namespace ava::event {

// Extracted retry slice of an Event: human-readable text, status, the error classification fields, the triggering reason and the attempt/backoff accounting.
struct RetryPayload
{
  ava::core::NoLogString text;
  std::string status;
  std::string error_category;
  std::string error_code;
  ava::core::NoLogString error_message;
  ava::core::NoLogString error_details;
  std::string trigger;
  ava::core::NoLogString reason;
  std::size_t attempt = 0;
  std::size_t max_attempts = 0;
  std::size_t delay_ms = 0;
  std::size_t remaining_ms = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::event
