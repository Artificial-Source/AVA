#pragma once

#include "BasicScreen.h"
#include "BasicWindow.h"
#include "Color.h"
#include "ColorPair.h"
#include "ComplexChar.h"
#include "Cursor.h"
#include "KeyboardInputMode.h"
#include "MouseInputMode.h"
#include "ScopedTimeout.h"
#include "utils/Badge.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ava::core {
// Forward declaration.
class Application;
} // namespace ava::core

namespace ava::tui::terminal {

class ColorPalette;

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
  CtrlQ,
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
  Unknown,
  Resize,
  Mouse,
  WideCharacter
};

// Context
//
// Represents the terminal. It's lifetime is equivalent with the
// time that the terminal is under the control of this application.
//
class Context final
{
  friend class ColorPalette;

 private:
  BasicScreen first_screen_;                                    // Owns the screen iff initialize receives explicit terminal streams.
  BasicWindow stdscr_;                                          // The entire surface of the current terminal screen.
  FILE* input_file_ = nullptr;                                  // Non-owning stream from the terminal emulator.
  FILE* output_file_ = nullptr;                                 // Non-owning stream connected to the terminal emulator.
  Rendition default_rendition_;                                 // The rendition to use for text that doesn't have any defined of its own.
  bool has_cvvis_cap_ = false;                                  // True iff the current TERM supports cvvis, meaning that `cursor_visibility = 2` can be used.
  bool default_colors_enabled_ = false;                         // Whether -1 selects the terminal's default colors.
  bool initialized_ = false;                                    // Whether ncurses initialization reached a state that requires terminal restoration.
  std::vector<ColorPair> color_pairs_;                          // All registered foreground/background color pairs so far.
  std::unique_ptr<ColorPalette> color_palette_;                 // The live color palette of stdscr if the terminal isn't direct-color.
  CursorState cursor_state_;                                    // The current cursor state, corresponding to the last call to apply_cursor_settings.
  MouseInputMode mouse_input_;                                  // RAI object enabling mouse reporting and bracketed paste while Context is active.
  KeyboardInputMode keyboard_protocols_;                        // RAI object to bring terminal into a state to disambiguate escape codes.

 public:
  Context(utils::Badge<core::Application>);
  void initialize(FILE* outfd = nullptr, FILE* infd = nullptr);

  ~Context();

  // Return the borrowed streams supplied at initialization for protocol IO and test fixtures.
  // Both are non-null after initialize and remain owned by the caller for the Context's entire lifetime.
  FILE* input_stream() const { return input_file_; }
  FILE* output_stream() const { return output_file_; }

  Rendition const& default_rendition() const { return default_rendition_; }

  // Return a ColorPair for `foreground` and `background`, using exact RGB on direct-color terminals and exact or nearest palette
  // colors otherwise. A live mutable indexed palette may be programmed on demand when no exact entry exists; when live palette probing
  // is unavailable, concrete RGB colors are approximated from ncurses-readable entries without reprogramming the terminal palette.
  //
  // The terminal must support colors and have room for another color pair. The default terminal color is preserved on both paths.
  ColorPair create_color_pair(Color foreground, Color background);              // init_extended_pair
  ColorPair create_color_pair(ColorIndex foreground, Color background);         // init_extended_pair
  ColorPair create_color_pair(Color foreground, ColorIndex background);         // init_extended_pair
  ColorPair create_color_pair(ColorIndex foreground, ColorIndex background);    // init_extended_pair

  // Return the terminal foreground and background indexes assigned to `color_pair`.
  //
  // An empty result means ncurses rejected the pair index.
  std::optional<ColorPairContent> color_pair_content(ColorPair color_pair) const;    // extended_pair_content

  // Return the terminal-reported RGB intensities for `color_index`.
  //
  // Components use the inclusive 0 through 1000 ncurses scale. An empty result means the terminal does not expose that index.
  std::optional<ColorContent> color_content(int color_index) const;                  // extended_color_content

  BasicScreen const& first_screen() const { return first_screen_; }
  BasicScreen& first_screen() { return first_screen_; }

  BasicWindow const& stdscr() const { return stdscr_; }                 // stdscr
  BasicWindow& stdscr() { return stdscr_; }                             //

  uint32_t rows() const;                                                // LINES
  uint32_t cols() const;                                                // COLS
  int colors() const;                                                   // COLORS
  Dimension size() const { return {rows(), cols()}; }

  // Convenience accessor that tests if COLORS equals 0x1000000.
  static bool have_direct_color();                                      // COLORS

  // Return the next input value; blocks if there is no input.
  int get_wch() const;                                                  // get_wch

  // Synchronize the virtual screen with the physical screen.
  static void doupdate();                                               // doupdate

  // Resize the active ncurses screen to the given rows and cols.
  //
  // Returns true on success, false for invalid dimensions or an ncurses failure.
  // Updates screen/window geometry and queues KEY_RESIZE; does not resize the physical terminal or repaint it.
  static bool resizeterm(int rows, int cols);                            // resizeterm

  // Return whether the given rows and cols differ from the active ncurses screen's dimensions.
  //
  // Does not resize the screen or queue an input event. Invalid dimensions or an uninitialized screen return false.
  static bool is_term_resized(int rows, int cols);                       // is_term_resized

  // Sound the terminal's audible alarm.
  int beep();                                                           // beep
  // Alert the terminal user by visibly attracting attention.
  int flash();                                                          // flash
  // Return the ESC delay being used.
  int get_escdelay() const;                                             // get_escdelay
  // Returns a character string corresponding to the key c.
  std::string_view keyname(int c) const;                                // keyname

  // Flush output_file_ after a call to write_raw_sequence(..., false).
  bool flush_raw();
  // Write a raw sequence of characters to the terminal and flush it. Returns true upon success.
  bool write_raw_sequence(std::string_view sequence, bool flush = true);

  // Emit the OSC 11 terminal-background query and flush it. Theme policy and reply interpretation remain TUI concerns.
  [[nodiscard]] bool query_background_color();

  // Return one wide character preserved while KeyboardInputMode negotiated, exactly once and before callers read ncurses input.
  // UTF-8 split across the negotiation buffer and terminal descriptor is completed here. `wch` is an output parameter; a null pointer
  // returns false without consuming input. Invalid or truncated UTF-8 is returned byte-by-byte as U+FFFD and cannot alias controls.
  [[nodiscard]] bool try_get_buffered_keyboard_input(wint_t* wch);

  // Read at most 4096 raw bytes currently available from the configured terminal input, waiting no longer than `timeout`.
  //
  // This directly polls and reads the input stream descriptor without changing its blocking flags. It is intended only for short
  // startup protocol negotiation before normal ncurses input begins and must not run concurrently with ncurses reads. Timeout, EOF,
  // invalid streams, and poll/read errors all return an empty string because this best-effort startup seam has no error channel.
  std::string read_raw_input_for(std::chrono::milliseconds timeout) const;

  bool has_colors() const;                                              // has_colors
  bool can_change_colors() const;

  // Cursor control.

  void apply_cursor_settings(CursorSettings const& settings) { cursor_state_.apply(this, settings); }
  // Emit the retained cursor settings even when they have not changed, unless AVA has only observed the untouched default.
  //
  // Use this after terminal operations such as curs_set that can alter cursor shape or blinking behind CursorState's back.
  void reapply_cursor_settings() { cursor_state_.reapply(this); }
  CursorSettings const& cursor_settings() const { return cursor_state_.cursor_settings_; }

  // Set physical cursor visibility and restore any explicitly applied cursor shape when making it visible.
  //
  // ncurses may alter shape or blinking as a side effect of changing visibility, so callers must use this instead of BasicWindow::curs_set.
  void set_cursor_visible(bool visible)
  {
    BasicWindow::curs_set(visible ? 1 : 0);
    if (visible)
      cursor_state_.reapply(this);
  }

  // Return terminal presentation to the invoking environment and release every AVA-owned input and cursor protocol.
  void leave_terminal_for_handoff();

  // Re-enter ncurses program mode, repaint its retained virtual screen, and explicitly restore every AVA-owned protocol.
  void restore_terminal_after_handoff();

  // Read one ncurses Key from the terminal input. Called from read_curses_input_from_terminal.
  Key read_curses_key(wint_t& value);

  // Read the next character iff that is a plain wide character (not a KEY_CODE).
  std::optional<wchar_t> read_plain_wide_character();

  // Read one character of an escape/control sequence after ESC was already consumed.
  // Returns true if another character could be appended.
  bool append_escape_sequence_character(std::string& consumed_out);

  // Set a temporary timeout on stdsrc.
  ScopedTimeout timeout(std::chrono::milliseconds delay_ms) { return {delay_ms}; }

 private:
  // Return the next input value, or -1 when the configured stdscr timeout expires or input fails.
  int try_get_wch();                                                    // get_wch

  // Return ncurses-compatible status while reading through the canonical Context first.
  // `wch_out` is a required non-null output parameter, matching wget_wch's caller contract.
  int read_wch(wint_t* wch_out);

  // Convert `color` to a direct color or stable palette index. Called by create_color_pair.
  int terminal_color_index(Color color);

  // Register one pair from already resolved terminal color indexes.
  ColorPair priv_create_color_pair(int foreground_index, int background_index);

 public:
  AVA_DEBUG_PRINT_MEMBERS_ON

#if CW_DEBUG
  // Used by the accessor core::Application::terminal_context.
  bool is_initialized() const { return initialized_; }
#endif
};

} // namespace ava::tui::terminal
