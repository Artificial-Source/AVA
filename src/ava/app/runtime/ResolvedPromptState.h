#pragma once

#include "BasePromptMetadata.h"
#include "ContextSourceMetadata.h"
#include "FreshnessSourceMetadata.h"
#include "ava/debug/print_members_on.h"
#include "ava/agent/mode.h"

#include <string>
#include <vector>

namespace ava::app::runtime {

// Resolved prompt state.
//
// The agent mode and assembled prompt material currently in effect for the
// session: base-prompt metadata, contributing context and freshness sources,
// the ordinary system prompt, and its ambient-extension-free runtime variant.
// This whole bundle is recomputed
// whenever the mode or model changes (see apply_prompt_state and
// switch_model) and is otherwise the session's live resolved state.
//
// It is field-compatible with the transient PromptState returned by
// load_runtime_prompt_state, which apply_prompt_state moves into this
// member as a single object rather than copying fields one at a time.
struct ResolvedPromptState
{
  ava::agent::Mode mode = ava::agent::Mode::Build;
  BasePromptMetadata base_prompt;
  std::vector<ContextSourceMetadata> context_sources;
  std::vector<FreshnessSourceMetadata> freshness_sources;
  std::string system_prompt;
  std::string ambient_extension_free_system_prompt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::app::runtime
