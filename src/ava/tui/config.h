// Shared terminal and composer defaults used by the TUI owners and input decoder.
#pragma once

#include <cstdint>

namespace ava::tui::config {

// The maximum number of rows of text that fit in the composer window's pad viewport.
constexpr uint32_t max_composer_viewport_height = 17;

// 1: visible, 2: very visible. "Very visibible" required term capability "cvvis".
constexpr int cursor_visibility = 1;

// Whether or not by default the cursor blinks or not.
constexpr bool default_cursor_blink = true;

// Default ncurses delay for distinguishing Escape from a key sequence; ESCDELAY
// overrides that ncurses setting. The TUI decoder independently uses this value
// for each continuation read, even when ESCDELAY is set; it is not a total budget.
constexpr int default_terminal_escape_delay_ms = 100;

// The "info" color as used by opencode.
constexpr uint32_t opencode_info_color = 0x56b6c2;

} // namespace ava::tui::config
