#include "sys.h"
#include "Context.h"
#include "MouseInputMode.h"

#include <string_view>
#include <utility>
#include "debug.h"

// This header must be included last.
#include "private_convert.h"

namespace ava::tui::terminal {
namespace {

constexpr std::string_view kMouseEnableSequence = "\x1b[?1003l\x1b[?1000h\x1b[?1002h\x1b[?1006h";
constexpr std::string_view kMouseDisableSequence = "\x1b[?1003l\x1b[?1006l\x1b[?1002l\x1b[?1000l";
constexpr std::string_view kBracketedPasteEnableSequence = "\x1b[?2004h";
constexpr std::string_view kBracketedPasteDisableSequence = "\x1b[?2004l";

} // namespace

// Restore terminal mouse and paste modes on scope exit.
MouseInputMode::~MouseInputMode() noexcept
{
  stop();
}

// Enable ncurses decoding before asking the terminal to begin reporting mouse and bracketed-paste events.
void MouseInputMode::start(Context& context)
{
  if (context_)
  {
    // Call stop() before reusing a MouseInputMode with another start() invocation.
    ASSERT(context_ == nullptr);
    return;
  }

  context_ = &context;
  mmask_t previous_mask = 0;
  // Configure ncurses to decode left-button, wheel, and pointer-motion reports from the active screen.
  mmask_t const mask = BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON1_CLICKED | REPORT_MOUSE_POSITION | BUTTON4_PRESSED | BUTTON5_PRESSED;
  static_cast<void>(::mousemask(mask, &previous_mask));
  static_cast<void>(::mouseinterval(0));
  static_cast<void>(context.write_raw_sequence(kMouseEnableSequence));
  static_cast<void>(context.write_raw_sequence(kBracketedPasteEnableSequence));
}

// Best-effort reverse terminal modes in the opposite order from activation.
void MouseInputMode::stop() noexcept
{
  Context* const context = std::exchange(context_, nullptr);
  if (!context)
    return;

  static_cast<void>(context->write_raw_sequence(kBracketedPasteDisableSequence));
  static_cast<void>(::mousemask(0, nullptr));
  static_cast<void>(context->write_raw_sequence(kMouseDisableSequence));
}

} // namespace ava::tui::terminal
