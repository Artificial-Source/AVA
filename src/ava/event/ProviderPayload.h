#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <string>
#include <vector>

namespace ava::event {

// Extracted provider-stream and TUI permission-audit slice of an Event: text/call/tool/status, error message/details, stop reason, reason, reasoning metadata,
// and permission request ids. Omits tool-result/diff accounting and error category/code/trigger fields not produced for ProviderEvent today.
struct ProviderPayload
{
  ava::core::NoLogString text;
  std::string call_id;
  std::string tool;
  std::string status;
  ava::core::NoLogString error_message;
  ava::core::NoLogString error_details;
  ava::core::NoLogString stop_reason;
  ava::core::NoLogString reason;
  std::string reasoning_format;
  std::vector<std::string> permission_request_ids;
  bool reasoning_redacted = false;
  bool reasoning_signature_present = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::event
