#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <string>

namespace ava::event {

// Extracted reasoning slice of an Event: visible reasoning text/status, optional error/stop fields from the stream bridge, and frontend-safe reasoning
// metadata.
struct ReasoningPayload
{
  ava::core::NoLogString text;
  std::string status;
  ava::core::NoLogString error_message;
  ava::core::NoLogString stop_reason;
  std::string reasoning_format;
  bool reasoning_redacted = false;
  bool reasoning_signature_present = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::event
