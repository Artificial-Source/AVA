#include "sys.h"
#include "SelectorHost.h"
#include "composer.h"
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

ava::core::Result<std::string> dispatch_tui_selector_authority(ComposerSnapshot& snapshot, SelectorHost& selectors, std::string pending_status,
                                                               std::function<bool()> const& render,
                                                               std::function<ava::core::Result<std::string>()> const& callback)
{
  selectors.close();
  snapshot.status = std::move(pending_status);
  if (!render())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to paint selector authority status");
    snapshot.status = error.format();
    return std::unexpected(std::move(error));
  }
  auto result = callback();
  if (!result)
    snapshot.status = result.error().format();
  return result;
}

}  // namespace ava::tui
