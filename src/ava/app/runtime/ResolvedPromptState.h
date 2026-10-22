#pragma once

#include "BasePromptMetadata.h"
#include "ContextSourceMetadata.h"
#include "FreshnessSourceMetadata.h"
#include "PromptReadiness.h"
#include "ava/debug/print_members_on.h"
#include "ava/frontend/UnlockedData.h"
#include "ava/agent/mode.h"

#include <string>
#include <vector>

namespace ava::app::runtime {

struct PromptModeAndContextSources
{
  ava::agent::Mode mode = ava::agent::Mode::Build;
  std::vector<ContextSourceMetadata> context_sources;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using PromptModeAndContextSources_ts = frontend::UnlockedData<PromptModeAndContextSources>;

// Resolved prompt state.
//
// The prompt readiness, agent mode, and assembled prompt material currently in
// effect for the session: base-prompt metadata, contributing context and
// freshness sources, the ordinary system prompt, and its ambient-extension-free
// runtime variant. Empty prompt text remains valid when readiness is Ready.
// This whole bundle is recomputed
// whenever the mode or model changes (see apply_prompt_state and
// switch_model) and is otherwise the session's live resolved state.
//
// PromptState material is applied in place so the nested observable object's
// mutex, registration, and address survive session updates and replacement.
struct ResolvedPromptState
{
  PromptReadiness readiness = PromptReadiness::Ready;
  BasePromptMetadata base_prompt;
  std::vector<FreshnessSourceMetadata> freshness_sources;
  std::string system_prompt;
  std::string ambient_extension_free_system_prompt;
  // Keep observable prompt metadata last so its address remains stable while
  // the surrounding resolved state is updated in place.
  PromptModeAndContextSources_ts mode_and_context_sources;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::app::runtime
