#pragma once

#include "BasicScreen.h"
#include "BasicWindow.h"
#include "Color.h"
#include "ColorPair.h"
#include "ComplexChar.h"

#include <memory>
#include <optional>
#include <vector>

namespace ava::tui::terminal {

class ColorPalette;

// Context
//
// Represents the terminal. It's lifetime is equivalent with the
// time that the terminal is under the control of this application.
//
class Context final
{
  friend class ColorPalette;

 private:
  BasicScreen first_screen_;                                            // Owns the screen iff `outfd` and `infd` are passed to the constructor.
  BasicWindow stdscr_;                                                  // The entire surface of the current terminal screen.
  FILE* output_file_;                                                   // Non-owning stream connected to the terminal emulator.
  Rendition default_rendition_;                                         // The rendition to use for text that doesn't have any defined of its own.
  bool default_colors_enabled_ = false;                                 // Whether -1 selects the terminal's default colors.
  std::vector<ColorPair> color_pairs_;                                  // All registered foreground/background color pairs so far.
  std::unique_ptr<ColorPalette> color_palette_;                         // The live color palette of stdscr if the terminal isn't direct-color.

 public:
  Context(FILE* outfd = nullptr, FILE* infd = nullptr);
  ~Context();

  Rendition const& default_rendition() const { return default_rendition_; }

  // Return a ColorPair for `foreground` and `background`, using exact RGB on direct-color terminals and exact or nearest palette
  // colors otherwise. Mutable indexed palettes are programmed on demand when no exact entry exists.
  //
  // The terminal must support colors and have room for another color pair. The default terminal color is preserved on both paths.
  ColorPair create_color_pair(Color foreground, Color background);      // init_extended_pair

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

  bool has_colors() const;                                              // has_colors
  bool can_change_colors() const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Return the next input value, or -1 when the configured stdscr timeout expires or input fails.
  static int try_get_wch();                                             // get_wch

  // Convert `color` to a direct color or stable palette index. Called by create_color_pair.
  int terminal_color_index(Color color);
};

} // namespace ava::tui::terminal
