#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <optional>
#include <string>
#include <vector>

namespace ava::app::runtime {

// Carry user-supplied overrides for the assembled system prompt: a full replacement and/or appended fragments.
struct PromptOverrides
{
  std::optional<ava::core::NoLogString> system_prompt = std::nullopt;
  std::vector<ava::core::NoLogString> append_system_prompts;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::app::runtime
