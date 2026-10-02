#include "sys.h"
#include "MouseEvent.h"
#include "utils/macros.h"

#include <array>
#include <optional>
#include <curses.h>

namespace ava::tui::terminal {

// Read the native queue once and translate named ncurses masks rather than relying on their bit layout.
// A native report is consumed even when validation fails; no partially decoded event escapes to callers.
std::optional<MouseEvent> get_mouse_event()
{
  MEVENT native{};
  if (::getmouse(&native) == ERR)
    return std::nullopt;
  if (AI_UNLIKELY(native.x < 0 || native.y < 0))
    return std::nullopt;

  // Identify the button independently of its action, using all action bits belonging to each button.
  constexpr std::array<mmask_t, 5> button_masks{
      BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED,
      BUTTON2_PRESSED | BUTTON2_RELEASED | BUTTON2_CLICKED | BUTTON2_DOUBLE_CLICKED | BUTTON2_TRIPLE_CLICKED,
      BUTTON3_PRESSED | BUTTON3_RELEASED | BUTTON3_CLICKED | BUTTON3_DOUBLE_CLICKED | BUTTON3_TRIPLE_CLICKED,
      BUTTON4_PRESSED | BUTTON4_RELEASED | BUTTON4_CLICKED | BUTTON4_DOUBLE_CLICKED | BUTTON4_TRIPLE_CLICKED,
      BUTTON5_PRESSED | BUTTON5_RELEASED | BUTTON5_CLICKED | BUTTON5_DOUBLE_CLICKED | BUTTON5_TRIPLE_CLICKED,
  };
  // Identify the action independently of its button, using the corresponding bit from every button.
  constexpr std::array<mmask_t, 5> action_masks{
      BUTTON1_PRESSED | BUTTON2_PRESSED | BUTTON3_PRESSED | BUTTON4_PRESSED | BUTTON5_PRESSED,
      BUTTON1_RELEASED | BUTTON2_RELEASED | BUTTON3_RELEASED | BUTTON4_RELEASED | BUTTON5_RELEASED,
      BUTTON1_CLICKED | BUTTON2_CLICKED | BUTTON3_CLICKED | BUTTON4_CLICKED | BUTTON5_CLICKED,
      BUTTON1_DOUBLE_CLICKED | BUTTON2_DOUBLE_CLICKED | BUTTON3_DOUBLE_CLICKED | BUTTON4_DOUBLE_CLICKED | BUTTON5_DOUBLE_CLICKED,
      BUTTON1_TRIPLE_CLICKED | BUTTON2_TRIPLE_CLICKED | BUTTON3_TRIPLE_CLICKED | BUTTON4_TRIPLE_CLICKED | BUTTON5_TRIPLE_CLICKED,
  };
  constexpr std::array actions{MouseButtonEvent::pressed, MouseButtonEvent::released, MouseButtonEvent::clicked, MouseButtonEvent::double_clicked,
                               MouseButtonEvent::triple_clicked};
  uint16_t button = 0;
  for (std::size_t index = 0; index < button_masks.size(); ++index)
    if ((native.bstate & button_masks[index]) != 0)
    {
      button = static_cast<uint16_t>(index + 1);
      // From https://invisible-island.net/ncurses/man/curs_mouse.3x.html
      // The bstate member has exactly one bit set indicating the event type [, and up to 3 additional bits indicating the use of modifier keys].
      break;
    }
  std::optional<MouseButtonEvent> action;
  for (std::size_t type = 0; type < action_masks.size(); ++type)
    if ((native.bstate & action_masks[type]) != 0)
    {
      action = actions[type];
      break;
    }
  if (!action)
  {
    if ((native.bstate & REPORT_MOUSE_POSITION) == 0)
      return std::nullopt;
    action = MouseButtonEvent::moved;
  }

  uint8_t modifiers = 0;
  if ((native.bstate & BUTTON_SHIFT) != 0)
    modifiers |= static_cast<uint8_t>(MouseButtonModifier::Shift);
  if ((native.bstate & BUTTON_ALT) != 0)
    modifiers |= static_cast<uint8_t>(MouseButtonModifier::Alt);
  if ((native.bstate & BUTTON_CTRL) != 0)
    modifiers |= static_cast<uint8_t>(MouseButtonModifier::Ctrl);

  return MouseEvent{Position{static_cast<uint32_t>(native.y), static_cast<uint32_t>(native.x)}, button, *action, static_cast<MouseButtonModifier>(modifiers)};
}

} // namespace ava::tui::terminal
