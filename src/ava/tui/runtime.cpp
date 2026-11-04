// Applies shared TUI feedback and paints selector handoffs before backend actions.
#include "sys.h"
#include "SelectorHost.h"
#include "composer.h"
#include "runtime_state_internal.h"
#include "ava/core/result.h"

#include <string>

namespace ava::tui {

void apply_reasoning_cycle_success(ComposerSnapshot& snapshot, std::string feedback)
{
  snapshot.status.clear();
  snapshot.reasoning_feedback = std::move(feedback);
}

void clear_reasoning_feedback_for_user_input(ComposerSnapshot& snapshot)
{
  snapshot.reasoning_feedback.reset();
  snapshot.local_command_feedback.reset();
}

ava::core::Result<std::string> dispatch_tui_selector_authority(RuntimePresentationState& presentation, SelectorHost& selectors, std::string pending_status,
                                                               std::function<bool()> const& render,
                                                               std::function<ava::core::Result<std::string>()> const& callback)
{
  // Clear the old selector before painting pending status, so a blocking backend
  // action cannot leave the previous choice looking interactive.
  selectors.close();
  presentation.publish_status(std::move(pending_status));
  if (!render())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to paint selector authority status");
    presentation.publish_status(error.format());
    return std::unexpected(std::move(error));
  }
  auto result = callback();
  if (!result)
    presentation.publish_status(result.error().format());
  return result;
}

}  // namespace ava::tui
