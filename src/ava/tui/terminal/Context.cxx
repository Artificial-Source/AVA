#include "sys.h"
#include "ColorPalette.h"
#include "Context.h"
#include "ava/tui/config.h"
#include "utils/to_string.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <poll.h>
#include <unistd.h>

// This header must be included last.
#include "private_convert.h"

namespace ava::tui::terminal {

Context::Context(utils::Badge<core::Application>) : default_rendition_(ColorPair{{}, 0})
{
}

Context::Context(FILE* outfd, FILE* infd) : default_rendition_(ColorPair{{}, 0})
{
  // This constructor is intended for CTests; pass appropriate FILE*'s.
  ASSERT(outfd != nullptr && infd != nullptr);
  initialize(outfd, infd);
}

void Context::initialize(FILE* outfd, FILE* infd)
{
  DoutEntering(dc::notice, "Context::initialize(" << outfd << ", " << infd << ")");

  setlocale(LC_ALL, "");

  // From https://invisible-island.net/ncurses/man/curs_util.3x.html
  // use_env   use_tioctl   Summary
  // TRUE      TRUE         ncurses updates LINES and COLUMNS based on operating system calls.
  ::use_env(TRUE);
  ::use_tioctl(TRUE);

  // If one of the arguments is nullptr then so must be the other one.
  ASSERT((outfd == nullptr) == (infd == nullptr));
  // Use initscr or newterm?
  bool use_initscr = outfd == nullptr;
  input_file_ = use_initscr ? stdin : infd;
  output_file_ = use_initscr ? stdout : outfd;

  if (use_initscr)
    initscr();
  else
  {
    BasicScreen first_screen(nullptr, outfd, infd);
    first_screen_ = std::move(first_screen);
  }

  // Preserve ncurses' environment-derived value when the user explicitly configured ESCDELAY.
  // Otherwise replace its one-second default with AVA's shorter standalone-Escape delay.
  if (std::getenv("ESCDELAY") == nullptr)
    static_cast<void>(::set_escdelay(config::default_terminal_escape_delay_ms));

  // Determine available capabilities.
  char* cap = tigetstr("cvvis");
  has_cvvis_cap_ = cap != nullptr && cap != reinterpret_cast<char*>(-1);

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

  raw();                // Prefer to receive ctrl-c, ctrl-s, ctrl-q etc as key codes instead of signals.
  noecho();
  nonl();               // Do not translate the Enter key to a linefeed.
  meta(::stdscr, TRUE); // Always return 8-bit character codes.
  curs_set(FALSE);      // The cursor is turned on as soon as the composer area is created.

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

  // Enable mouse reporting and bracketed paste for the lifetime of this Context.
  mouse_input_.start(*this);

  // Start keyboard input mode to disambiguate escape codes, most notably to
  // be able to tell the difference between Enter and cntrl-Enter.
  keyboard_protocols_.start(*this);

  // Tell the destructor that initialized was called.
  initialized_ = true;
}

Context::~Context()
{
  if (initialized_)
  {
    // Stop keyboard protocols before ncurses restores the terminal.
    keyboard_protocols_.stop();
    // Disable terminal input protocols before ncurses restores the terminal.
    mouse_input_.stop();
    // Restore the cursor to its default value.
    apply_cursor_settings({CursorStyle::Default});
    // Restore cursor visibility.
    curs_set(TRUE);
    // Restore mutable palette entries before endwin returns terminal presentation to the invoking process.
    color_palette_.reset();
    bool use_initscr = output_file_ == stdout;
    if (use_initscr)
    {
      // Attempt to restore the terminal.
      [[maybe_unused]] int res = endwin();
#ifdef CWDEBUG
      if (res == ERR)
        Dout(dc::warning, "Context::~Context(): endwin() unsuccessful: terminal possibly not restored.");
#endif
    }
  }
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

int Context::try_get_wch()
{
  wint_t wch = 0;

  // First try if there is still any input buffered on keyboard_protocols_.
  if (AI_UNLIKELY(keyboard_protocols_.try_get_wch(&wch)))
    return static_cast<int>(wch);

  // If not, try reading a character directly from the terminal.
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
  return priv_create_color_pair(terminal_color_index(foreground), terminal_color_index(background));
}

// Create a pair from one portable palette foreground and one resolved RGB/default background.
ColorPair Context::create_color_pair(ColorIndex foreground, Color background)
{
  DoutEntering(dc::notice|continued_cf, "Context::create_color_pair(" << utils::to_string(foreground) << ", " << background << ") = ");
  return priv_create_color_pair(static_cast<int>(foreground), terminal_color_index(background));
}

// Create a pair from one resolved RGB/default foreground and one portable palette background.
ColorPair Context::create_color_pair(Color foreground, ColorIndex background)
{
  DoutEntering(dc::notice|continued_cf, "Context::create_color_pair(" << foreground << ", " << utils::to_string(background) << ") = ");
  return priv_create_color_pair(terminal_color_index(foreground), static_cast<int>(background));
}

// Create a pair directly from two portable palette indexes.
ColorPair Context::create_color_pair(ColorIndex foreground, ColorIndex background)
{
  DoutEntering(dc::notice|continued_cf, "Context::create_color_pair(" << utils::to_string(foreground) << ", " << utils::to_string(background) << ") = ");
  return priv_create_color_pair(static_cast<int>(foreground), static_cast<int>(background));
}

// Register a pair after reserving any mutable indexed-palette entries that it exposes.
ColorPair Context::priv_create_color_pair(int foreground_index, int background_index)
{
  // Replace default-color sentinels with a dark-theme fallback if ncurses could not enable terminal defaults.
  if (!default_colors_enabled_)
  {
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

// Read the terminal color indexes currently registered for a ColorPair.
std::optional<ColorPairContent> Context::color_pair_content(ColorPair color_pair) const
{
  int foreground_index = 0;
  int background_index = 0;
  if (::extended_pair_content(static_cast<int>(color_pair.index()), &foreground_index, &background_index) == ERR)
    return std::nullopt;
  return ColorPairContent{foreground_index, background_index};
}

// Read ncurses' scaled RGB components for one terminal color index.
std::optional<ColorContent> Context::color_content(int color_index) const
{
  ColorContent result{};
  if (::extended_color_content(color_index, &result.red, &result.green, &result.blue) == ERR)
    return std::nullopt;
  return result;
}

//static
void Context::doupdate()
{
  ::doupdate();
}

int Context::beep()
{
  return ::beep();
}

int Context::flash()
{
  return ::flash();
}

int Context::get_escdelay() const
{
  return ::get_escdelay();
}

bool Context::write_raw_sequence(std::string_view sequence)
{
  if (!output_file_)
    return false;
  return std::fwrite(sequence.data(), 1, sequence.size(), output_file_) == sequence.size() && std::fflush(output_file_) == 0;
}

// Read a bounded raw byte batch from the configured terminal input descriptor.
//
// EINTR retries consume the original absolute deadline rather than restarting the caller's timeout. All failures intentionally
// collapse to an empty result; callers use this only for optional terminal-feature negotiation.
std::string Context::read_raw_input_for(std::chrono::milliseconds timeout) const
{
  constexpr std::size_t kMaximumRawRead = 4096;

  if (!input_file_)
    return {};
  int const descriptor = ::fileno(input_file_);
  if (descriptor < 0)
    return {};

  using Clock = std::chrono::steady_clock;
  auto const nonnegative_timeout = std::max(timeout, std::chrono::milliseconds::zero());
  auto const deadline = Clock::now() + nonnegative_timeout;
  pollfd item{descriptor, POLLIN, 0};

  while (true)
  {
    auto const remaining = deadline - Clock::now();
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (remaining > std::chrono::steady_clock::duration::zero() && wait < remaining)
      ++wait;
    int const poll_timeout = remaining <= Clock::duration::zero() ? 0 : static_cast<int>(std::min<long long>(wait.count(), std::numeric_limits<int>::max()));

    int const ready = ::poll(&item, 1, poll_timeout);
    if (ready < 0 && errno == EINTR)
    {
      if (Clock::now() >= deadline)
        return {};
      continue;
    }
    if (ready <= 0 || (item.revents & (POLLIN | POLLHUP)) == 0)
      return {};

    std::array<char, kMaximumRawRead> bytes;
    ssize_t const count = ::read(descriptor, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR)
    {
      if (Clock::now() >= deadline)
        return {};
      item.revents = 0;
      continue;
    }
    if (count <= 0)
      return {};
    return std::string(bytes.data(), static_cast<std::size_t>(count));
  }
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
