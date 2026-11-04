// Applies command-output modal input before the session dispatches editor keys.
#include "sys.h"
#include "CommandOutput.h"
#include "command_output.h"
#include "runtime_state_internal.h"

namespace ava::tui {
CommandOutput::CommandOutput(RuntimePresentationState& presentation) : presentation_(presentation)
{
}

CommandOutputDisposition CommandOutput::handle_input(InputEvent const& event)
{
  // Host permission/question prompts keep priority over output capture. Borrow the
  // screen view only for this call; presentation methods perform every modal change.
  auto const& screen = presentation_.screen();
  if (!screen.command_output || screen.permission_prompt || screen.question_prompt)
    return CommandOutputDisposition::Pass;
  auto const geometry = command_output_geometry(screen.width, screen.height);
  auto const result = handle_command_output_input(*screen.command_output, event, geometry.width, geometry.height, screen.tool_presentation);
  if (result.action == CommandOutputInputAction::Dismiss)
  {
    presentation_.publish_command_output(std::nullopt);
    presentation_.publish_status("command output closed");
    return CommandOutputDisposition::Redraw;
  }
  if (result.action == CommandOutputInputAction::Redraw)
  {
    presentation_.publish_command_output_scroll(result.scroll_offset);
    return CommandOutputDisposition::Redraw;
  }
  // Close before forwarding typing so the editor can publish its own feedback.
  // Pointer reports remain captured, avoiding hits on content hidden by the modal.
  using Key = terminal::Key;
  if (event.key == Key::Character || event.key == Key::Space || event.key == Key::CtrlU)
  {
    presentation_.publish_command_output(std::nullopt);
    presentation_.publish_status({});
    return CommandOutputDisposition::Pass;
  }
  return CommandOutputDisposition::Consumed;
}
} // namespace ava::tui
