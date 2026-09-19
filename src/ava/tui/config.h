#pragma once

#include <cstdint>

namespace ava::tui::config {

// The maximum number of rows of text that fit in the composer window's pad viewport.
constexpr uint32_t max_composer_viewport_height = 17;

// 1: visible, 2: very visible. "Very visibible" required term capability "cvvis".
constexpr int cursor_visibility = 1;

// Whether or not by default the cursor blinks or not.
constexpr bool default_cursor_blink = true;

// Time to wait after ESC is pressed until a single escape key press is considered not part of an escape sequence.
// If the environment variable ESCDELAY is set, it takes precedence and this value is ignored.
constexpr int default_terminal_escape_delay_ms = 100;

} // namespace ava::tui::config
