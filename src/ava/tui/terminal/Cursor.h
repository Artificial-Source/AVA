#pragma once

#include "ava/tui/config.h"
#include "debug.h"

namespace ava::tui::terminal {

enum class CursorStyle
{
  Default,
  Block,
  Underline,
  Bar
};

class CursorSettings
{
 private:
  CursorStyle style_;           // The shape of the cursor.
  bool blink_;                  // True for a blinking cursor; ignored if style_ is Default.

 public:
  CursorSettings(CursorStyle style, bool blink = config::default_cursor_blink) : style_(style), blink_(blink) { }

  bool operator==(CursorSettings const&) const = default;

  // Accessors.
  CursorStyle style() const { return style_; }
  bool blink() const { return blink_; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CursorState
{
  CursorSettings cursor_settings_{CursorStyle::Default};                // The last CursorSettings that were applied.

  void apply(CursorSettings const& cursor_settings);
  void reset();

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui::terminal
