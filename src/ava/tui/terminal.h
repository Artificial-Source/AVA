#pragma once

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

enum class Key
{
  Character,
  Enter,
  Backspace,
  ShiftBackspace,
  CtrlBackspace,
  Delete,
  ShiftDelete,
  Insert,
  Clear,
  Tab,
  Space,
  CtrlSpace,
  Ctrl0,
  Ctrl1,
  Ctrl2,
  Ctrl3,
  Ctrl4,
  Ctrl5,
  Ctrl6,
  Ctrl7,
  Ctrl8,
  Ctrl9,
  ShiftTab,
  ShiftL,
  ShiftT,
  Escape,
  ArrowUp,
  ArrowDown,
  ArrowLeft,
  ArrowRight,
  ShiftArrowUp,
  ShiftArrowDown,
  ShiftArrowLeft,
  ShiftArrowRight,
  ShiftCtrlArrowLeft,
  ShiftCtrlArrowRight,
  ShiftAltArrowLeft,
  ShiftAltArrowRight,
  CtrlArrowLeft,
  CtrlArrowRight,
  AltArrowUp,
  AltArrowDown,
  AltArrowLeft,
  AltArrowRight,
  PageUp,
  PageDown,
  Home,
  End,
  CtrlHome,
  CtrlEnd,
  ShiftHome,
  ShiftEnd,
  ShiftCtrlHome,
  ShiftCtrlEnd,
  MouseWheelUp,
  MouseWheelDown,
  MouseLeftPress,
  MouseLeftClick,
  MouseLeftDrag,
  MouseLeftRelease,
  // Cancels an in-progress AVA press/drag/header-arm without starting selection.
  // Emitted for Shift-modified button reports and shared protocol handoff boundaries.
  MousePointerCancel,
  ShiftEnter,
  CtrlEnter,
  AltEnter,
  CtrlA,
  CtrlB,
  CtrlC,
  CtrlD,
  CtrlE,
  CtrlF,
  CtrlG,
  CtrlH,
  CtrlK,
  CtrlL,
  CtrlMinus,
  CtrlSlash,
  CtrlN,
  CtrlO,
  CtrlP,
  CtrlShiftP,
  CtrlR,
  CtrlRightBracket,
  CtrlS,
  CtrlT,
  CtrlU,
  CtrlV,
  CtrlW,
  CtrlX,
  CtrlY,
  CtrlZ,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  AltBackspace,
  AltB,
  AltD,
  AltDelete,
  AltF,
  AltH,
  AltJ,
  AltK,
  AltL,
  AltW,
  CtrlAltRightBracket,
  AltY,
  Unknown
};

struct InputEvent
{
  Key key = Key::Unknown;
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
// Stateful ncurses mouse normalization. Motion is a drag only while AVA owns a
// preceding left press; Shift reports are ignored and releases close ownership.
[[nodiscard]] InputEvent terminal_ncurses_mouse_event(std::uint64_t button_state, std::size_t column, std::size_t row);
// Clears owned left-button press tracking and any incomplete drag lifecycle. Safe
// before/after protocol disable/rearm and after Shift-modified reports so a later
// unmodified hover/release cannot extend a cancelled interaction.
void terminal_reset_mouse_tracking() noexcept;
[[nodiscard]] Key terminal_escape_sequence_key(std::string_view sequence);
[[nodiscard]] bool terminal_escape_sequence_complete(std::string_view sequence);
[[nodiscard]] bool terminal_escape_sequence_should_discard(std::string_view sequence);
[[nodiscard]] bool terminal_is_tty();

constexpr auto terminal_signals = core::Signals::to_mask(SIGINT) | core::Signals::to_mask(SIGTERM);

namespace detail {
[[nodiscard]] bool force_terminal_cursor_visible() noexcept;
}  // namespace detail

}  // namespace ava::tui
