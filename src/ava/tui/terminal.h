#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"
#include "ava/core/Signals.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
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

enum class KeyboardProtocolResponseAction
{
  None,
  EnableModifyOtherKeys,
  DisableModifyOtherKeys
};

enum class TerminalCursorStyle
{
  Default,
  Block,
  Underline,
  Bar
};

struct TerminalCursorSettings
{
  TerminalCursorStyle style = TerminalCursorStyle::Default;
  bool blink = true;

  bool operator==(TerminalCursorSettings const&) const = default;

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
[[nodiscard]] int terminal_escape_delay_ms();
[[nodiscard]] std::string_view terminal_kitty_keyboard_push_sequence();
[[nodiscard]] std::string_view terminal_kitty_keyboard_query_sequence();
[[nodiscard]] std::string_view terminal_kitty_keyboard_pop_sequence();
[[nodiscard]] std::string_view terminal_alacritty_da2_query_sequence();
[[nodiscard]] std::optional<int> terminal_alacritty_da2_version(std::string_view sequence);
// DA2 version bytes are trusted only for a positively identified direct Alacritty.
[[nodiscard]] bool terminal_alacritty_da2_probe_environment_allows_query(std::optional<std::string_view> tmux, std::optional<std::string_view> term,
                                                                         std::optional<std::string_view> term_program);
[[nodiscard]] std::string_view terminal_modify_other_keys_enable_sequence();
[[nodiscard]] std::string_view terminal_modify_other_keys_disable_sequence();
[[nodiscard]] std::string_view terminal_bracketed_paste_enable_sequence();
[[nodiscard]] std::string_view terminal_bracketed_paste_disable_sequence();
[[nodiscard]] std::string_view terminal_mouse_enable_sequence();
[[nodiscard]] std::string_view terminal_mouse_disable_sequence();
[[nodiscard]] std::optional<int> terminal_kitty_keyboard_flags_response(std::string_view sequence);
[[nodiscard]] bool terminal_device_attributes_response(std::string_view sequence);
[[nodiscard]] KeyboardProtocolResponseAction terminal_keyboard_protocol_response_action(std::string_view sequence, bool kitty_response_seen,
                                                                                        bool modify_other_keys_enabled);
[[nodiscard]] bool terminal_keyboard_protocol_handle_response(std::string_view sequence);

// Snapshot of AVA-owned interactive terminal protocol state. Used by suspend/
// external-editor handoff and by deterministic lifecycle tests.
struct TerminalProtocolOwnership
{
  bool kitty_keyboard_pushed = false;
  int kitty_keyboard_active_flags = 0;
  int kitty_keyboard_desired_flags = 7;
  bool alacritty_da2_probe_armed = false;
  bool kitty_keyboard_supported = false;
  bool keyboard_protocol_kitty_response_seen = false;
  bool modify_other_keys_enabled = false;
  bool modify_other_keys_desired = false;
  bool bracketed_paste_enabled = false;
  bool mouse_enabled = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] TerminalProtocolOwnership terminal_protocol_ownership() noexcept;
// Fresh interactive enter: enable paste/mouse, reset negotiation, push Kitty with
// query/DA once. Idempotent with respect to already-pushed Kitty stack entries.
void arm_owned_terminal_protocols_on_enter() noexcept;
// Shell/editor handoff: balance and disable AVA-owned protocols without clearing
// negotiated keyboard preferences. Safe to call repeatedly.
void release_owned_terminal_protocols() noexcept;
// After reset_prog_mode: re-enable paste/mouse and re-arm negotiated keyboard
// modes exactly once. Never re-probes OSC 11 and never grows the Kitty stack.
void rearm_owned_terminal_protocols() noexcept;
// Final session teardown of owned protocols: release plus clear negotiation memory.
void restore_owned_terminal_protocols() noexcept;
// TUI-thread-only cursor ownership. Default never writes a reset unless AVA
// previously forced a DECSCUSR style. Release/reset is shared with handoff and teardown.
void apply_terminal_cursor_settings(TerminalCursorSettings settings) noexcept;
[[nodiscard]] TerminalCursorSettings terminal_cursor_settings() noexcept;
[[nodiscard]] bool terminal_cursor_style_forced() noexcept;
[[nodiscard]] std::string_view terminal_cursor_style_sequence(TerminalCursorSettings settings) noexcept;
[[nodiscard]] std::string_view terminal_cursor_style_reset_sequence() noexcept;
// Apply TIOCGWINSZ via resizeterm only when the kernel size differs from current
// ncurses geometry. Same-size calls are no-ops so repeated W6 fit refreshes cannot
// flood KEY_RESIZE. Fail-soft without a TTY or before curses init.
void refresh_terminal_geometry_from_kernel() noexcept;
// Nonblocking discard of pending curses then kernel input. No sleep, read loop,
// output drain, or throw. Safe on partial enter / repeated calls.
void discard_pending_terminal_input() noexcept;

[[nodiscard]] std::string_view terminal_background_query_sequence();
// Pure environment gate for the startup OSC 11 query. nullopt means the variable
// is absent; empty means present-but-empty. Non-empty TMUX or TERM starting with
// "tmux" suppresses the query; absent/empty TMUX with direct TERM (or absent/empty
// TERM) allows it.
[[nodiscard]] bool terminal_background_probe_environment_allows_query(std::optional<std::string_view> tmux, std::optional<std::string_view> term);
// Write the exact OSC 11 query bytes and flush. Returns false on null out, short
// write, or flush failure.
[[nodiscard]] bool write_terminal_background_query(FILE* out);
// Gate then write: returns true only when the environment allows the query and the
// write succeeds. Writes nothing when the gate suppresses.
[[nodiscard]] bool emit_terminal_background_query_if_environment_allows(std::optional<std::string_view> tmux, std::optional<std::string_view> term, FILE* out);
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
using TerminalSequenceWriter = void (*)(std::string_view sequence);
// Test seam: capture protocol sequence emission without touching stdout.
void set_terminal_sequence_writer_for_test(TerminalSequenceWriter writer) noexcept;
void reset_terminal_sequence_writer_for_test() noexcept;
using TerminalFlushinpHook = void (*)() noexcept;
using TerminalTcflushHook = int (*)(int fd, int queue_selector) noexcept;
void set_terminal_input_flush_hooks_for_test(TerminalFlushinpHook flushinp_hook, TerminalTcflushHook tcflush_hook) noexcept;
void reset_terminal_input_flush_hooks_for_test() noexcept;
// Reset ownership globals between pure lifecycle tests.
void reset_terminal_protocol_ownership_for_test() noexcept;
}  // namespace detail

}  // namespace ava::tui
