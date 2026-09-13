#include "sys.h"
#include "ColorPalette.h"
#include "Context.h"

#include <array>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <limits>

// This header must be included last.
#include "private_convert.h"

namespace ava::tui::terminal {

Context::Context(FILE* outfd, FILE* infd) : output_file_(outfd == nullptr ? stdout : outfd), default_rendition_(ColorPair{{}, 0})
{
  DoutEntering(dc::notice, "Context::Context(" << outfd << ", " << infd << ")");

  setlocale(LC_ALL, "");

  // From https://invisible-island.net/ncurses/man/curs_util.3x.html
  // use_env   use_tioctl   Summary
  // TRUE      TRUE         ncurses updates LINES and COLUMNS based on operating system calls.
  ::use_env(TRUE);
  ::use_tioctl(TRUE);

  // Use initscr or newterm?
  bool use_initscr = outfd == nullptr && infd == nullptr;

  if (use_initscr)
    initscr();
  else
  {
    BasicScreen first_screen(nullptr, outfd, infd);
    first_screen_ = std::move(first_screen);
  }

  // Initialize the stdsrc_ handle.
  stdscr_.init_as_stdscr();

  if (has_colors())
  {
    start_color();
    // Enable ncurses' default-color extension: after this succeeds, color
    // number -1 in init_pair/init_extended_pair means the terminal's default
    // foreground or background color instead of an RGB/direct-color index.
    default_colors_enabled_ = ::use_default_colors() == OK;
  }

  cbreak();
  noecho();
  nl();                 // Always translate the Enter key to a linefeed.
  meta(::stdscr, TRUE); // Always return 8-bit character codes.

  // Refresh stdscr once to consume its initial all-touched state, and to clear
  // whatever the previous program left on the physical terminal.
  // A still-touched stdscr would stage blanks over cells that other windows
  // already published to the virtual screen — explicitly, or implicitly from
  // the wrefresh(stdscr) that get_wch performs before blocking for input.
  // After this, application windows own all staging; never write through stdscr.
  stdscr_.refresh();

  // If this isn't a direct-color terminal, probe its color palette using OSC 4.
  if (COLORS < 0x1000000)
    color_palette_ = ColorPalette::create(*this);

  // Initialize default_rendition_ after creating the color_palette_ (if any).
  if (has_colors())
    default_rendition_ = Rendition{create_color_pair({}, {})};
}

Context::~Context()
{
  // Restore mutable palette entries before endwin returns terminal presentation to the invoking process.
  color_palette_.reset();
  endwin();
}

uint32_t Context::rows() const
{
  return LINES;
}

uint32_t Context::cols() const
{
  return COLS;
}

int Context::colors() const
{
  return COLORS;
}

//static
bool Context::have_direct_color()
{
  return COLORS == 0x1000000;
}

int Context::get_wch() const
{
  wint_t wch = 0;
  [[maybe_unused]] int res = ::get_wch(&wch);
  // Call blocking get_wch only while stdscr has no timeout and terminal input remains available; use try_get_wch for fallible reads.
  ASSERT(res != ERR);
  return static_cast<int>(wch);
}

//static
int Context::try_get_wch()
{
  wint_t wch = 0;
  if (::get_wch(&wch) == ERR)
    return -1;
  return static_cast<int>(wch);
}

// Convert `color` to a terminal color index.
//
// Direct-color terminals use RGB values as indices. Other terminals reuse or program an exact palette entry when possible, then fall
// back to their nearest current entry.
int Context::terminal_color_index(Color color)
{
  bool const direct_color = COLORS == 0x1000000;
  if (color.is_default())
    return -1;
  // Paranoia check: on a non-direct-color terminal we should always have a color_palette_.
  ASSERT(direct_color || color_palette_);
  return direct_color ? color.as_int() : color_palette_->nearest_indexed_color(color);
}

ColorPair Context::create_color_pair(Color foreground, Color background)
{
  DoutEntering(dc::notice|continued_cf, "Context::create_color_pair(" << foreground << ", " << background << ") = ");

  int foreground_index = terminal_color_index(foreground);
  int background_index = terminal_color_index(background);
  // terminal_color_index returns -1 for "default color"s.
  // Replace that with a "random" white or black if default colors are not supported by this terminal.
  if (!default_colors_enabled_)
  {
    // Assume a dark theme for now.
    if (foreground_index == -1)
      foreground_index = COLOR_WHITE;
    if (background_index == -1)
      background_index = COLOR_BLACK;
  }

  if (color_palette_)
  {
    color_palette_->reserve_index(foreground_index);
    color_palette_->reserve_index(background_index);
  }

  // On a direct-color terminal an RGB value is itself the color index. Indexed terminals instead use their nearest palette color.
  int const color_pair_index = static_cast<int>(color_pairs_.size()) + 1;
  [[maybe_unused]] int const status = ::init_extended_pair(color_pair_index, foreground_index, background_index);
  // init_extended_pair returns ERR when the pair index exceeds COLOR_PAIRS or a color index is out of range; keep
  // the number of created pairs within the terminal limit and pass valid Color values.
  ASSERT(status == OK);

  color_pairs_.push_back(ConvertToColorPair{color_pair_index});
  ColorPair result = color_pairs_.back();

  Dout(dc::finish, result);
  return result;
}

//static
void Context::doupdate()
{
  ::doupdate();
}

bool Context::has_colors() const
{
  return ::has_colors();
}

bool Context::can_change_colors() const
{
  // Do not call this function if we don't have a color palette; for example when a direct-color terminal is being used (COLORS == 0x1000000).
  ASSERT(color_palette_);
  return color_palette_->last_mutable_palette_index() > 0;
}

} // namespace ava::tui::terminal
