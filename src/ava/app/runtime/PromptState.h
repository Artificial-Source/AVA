#pragma once

#include "BasePromptMetadata.h"
#include "ContextSourceMetadata.h"
#include "FreshnessSourceMetadata.h"
#include "PromptReadiness.h"
#include "ava/debug/print_members_on.h"
#include "ava/agent/mode.h"

#include <string>
#include <vector>

namespace ava::app::runtime {

// Bundle the assembled prompts for one mode: reconstruction readiness, the active agent mode, base prompt metadata, contributing context and freshness
// sources, the ordinary system prompt, and its ambient-extension-free runtime variant. Readiness is independent of text emptiness.
//
// select_runtime_prompt_state builds this aggregate; apply_prompt_state copies it back into a Session.
struct PromptState
{
  PromptReadiness readiness = PromptReadiness::Ready;
  ava::agent::Mode mode = ava::agent::Mode::Build;
  BasePromptMetadata base_prompt;
  std::vector<ContextSourceMetadata> context_sources;
  std::vector<FreshnessSourceMetadata> freshness_sources;
  std::string system_prompt;
  std::string ambient_extension_free_system_prompt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::app::runtime
