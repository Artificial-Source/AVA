#pragma once

#include "Position.h"
#include "ava/debug/print_members_on.h"

#include <cstdint>
#include <optional>

namespace ava::tui::terminal {

enum class MouseButtonEvent : uint8_t
{
  pressed,
  released,
  clicked,
  double_clicked,
  triple_clicked,
  moved
};

enum class MouseButtonModifier : uint8_t
{
  None = 0,
  Shift = 1,
  Alt = 2,
  Ctrl = 4,
  Super = 8
};

// A decoded screen-relative mouse report, with one button action and a modifier bit mask.
// Motion-only reports use button zero; ncurses cannot report the Super modifier.
class MouseEvent
{
 private:
  Position position_;                   // Event coordinates.
  uint16_t button_;                     // 1 through 5, or zero for motion-only reports.
  MouseButtonEvent event_;              // The type of the event.
  MouseButtonModifier modifiers_;       // Modifier keys that were pressed.

 public:
  // Construct an event from screen position, button number, event type, and modifier mask.
  MouseEvent(Position position, uint16_t button, MouseButtonEvent event, MouseButtonModifier modifiers)
      : position_(position), button_(button), event_(event), modifiers_(modifiers)
  {
  }

  // Inspect the decoded report without changing it or consuming further terminal input.
  Position position() const { return position_; }
  uint16_t button() const { return button_; }
  MouseButtonEvent event() const { return event_; }
  MouseButtonModifier modifiers() const { return modifiers_; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Consume one mouse report from the active ncurses screen and return its decoded event.
//
// Call on the terminal input thread after receiving Key::Mouse with mouse reporting enabled.
// Returns nullopt when getmouse fails, coordinates are negative, or the report cannot represent a single action.
// Modifier values are bitwise combined; motion without a button action is returned as moved with button zero.
// Without ncurses mouse support this always returns nullopt; unavailable Shift or button-five support is not decoded.
[[nodiscard]] std::optional<MouseEvent> get_mouse_event();

} // namespace ava::tui::terminal
