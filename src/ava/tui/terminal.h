#pragma once

#include "terminal/Context.h"
#include "terminal/MouseEvent.h"
#include "ava/debug/print_members_on.h"
#include "ava/core/Signals.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <termios.h>

namespace ava::tui {

struct InputEvent
{
  terminal::Key key = terminal::Key::Unknown;
  char character = '\0';
  std::string text = {};
  std::size_t mouse_column = 0;
  std::size_t mouse_row = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TerminalBackgroundColor
{
  int red = 0;
  int green = 0;
  int blue = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

void erase_last_utf8_codepoint(std::string& text);
// Pure environment gate for the startup OSC 11 query. nullopt means the variable
// is absent; empty means present-but-empty. Non-empty TMUX or TERM starting with
// "tmux" suppresses the query; absent/empty TMUX with direct TERM (or absent/empty
// TERM) allows it.
[[nodiscard]] bool terminal_background_probe_environment_allows_query(std::optional<std::string_view> tmux, std::optional<std::string_view> term);
[[nodiscard]] std::optional<TerminalBackgroundColor> terminal_osc11_background_response(std::string_view sequence);
void arm_terminal_background_response_handler();
void disarm_terminal_background_response_handler();
[[nodiscard]] bool terminal_background_response_handler_armed();
[[nodiscard]] bool terminal_background_response_handle(std::string_view sequence);
[[nodiscard]] InputEvent terminal_escape_sequence_event(std::string_view sequence);
// Normalize the decoded mouse_event into frontend input, updating left-button ownership.
//
// Motion is a drag only after an owned left press; Shift cancels ownership but preserves wheel scrolling.
// Releases close ownership, and unsupported actions return Unknown input.
// Screen positions are converted to the frontend's one-based mouse coordinates.
[[nodiscard]] InputEvent terminal_ncurses_mouse_event(terminal::MouseEvent const& mouse_event);
// Clears owned left-button press tracking and any incomplete drag lifecycle. Safe
// before/after protocol disable/rearm and after Shift-modified reports so a later
// unmodified hover/release cannot extend a cancelled interaction.
void terminal_reset_mouse_tracking() noexcept;
// Synchronize the active ncurses screen's dimensions with the kernel without queuing redundant resize events.
//
// Call after terminal initialization. Missing terminal geometry is ignored; ncurses resize failures are best-effort.
void refresh_geometry_from_kernel() noexcept;
[[nodiscard]] terminal::Key terminal_escape_sequence_key(std::string_view sequence);
[[nodiscard]] bool terminal_escape_sequence_complete(std::string_view sequence);
[[nodiscard]] bool terminal_escape_sequence_should_discard(std::string_view sequence);
[[nodiscard]] bool terminal_is_tty();

constexpr auto terminal_signals = core::Signals::to_mask(SIGINT) | core::Signals::to_mask(SIGTERM);

}  // namespace ava::tui
