// Applies shared reasoning feedback to the TUI composer snapshot.
#include "sys.h"
#include "composer.h"
#include "runtime_internal.h"

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

}  // namespace ava::tui
