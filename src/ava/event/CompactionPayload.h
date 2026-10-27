#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <string>

namespace ava::event {

// Extracted compaction slice of an Event: provider/model identity, status, trigger/reason, attempt accounting, and token/summary sizing for start and end
// events.
struct CompactionPayload
{
  std::string provider;
  std::string model;
  std::string status;
  std::string trigger;
  ava::core::NoLogString reason;
  std::size_t attempt = 0;
  std::size_t max_attempts = 0;
  std::size_t estimated_tokens = 0;
  std::size_t threshold_tokens = 0;
  std::size_t retained_tokens = 0;
  std::size_t post_compaction_tokens = 0;
  std::size_t summary_bytes = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::event
