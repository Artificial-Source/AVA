#pragma once

namespace ava::tui::config {

// The maximum number of rows of text that fit in the composer window's pad viewport.
constexpr uint32_t max_composer_viewport_height = 17;

// 1: visible, 2: very visible. "Very visibible" required term capability "cvvis".
constexpr int cursor_visibility = 1;

// Whether or not by default the cursor blinks or not.
constexpr bool default_cursor_blink = true;

} // namespace ava::tui::config
