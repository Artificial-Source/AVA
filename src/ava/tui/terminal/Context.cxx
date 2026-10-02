#include "sys.h"
#include "ColorPalette.h"
#include "Context.h"
#include "ava/tui/config.h"
#include "ava/tui/encode_wide_character.h"
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
#include <sys/ioctl.h>
#include <unistd.h>

// This header must be included last.
#include "private_convert.h"

namespace ava::tui::terminal {
namespace {

constexpr std::string_view kQueryBackgroundColor = "\x1b]11;?\x1b\\";

} // namespace

Context::Context(utils::Badge<core::Application>) : default_rendition_(ColorPair{{}, 0})
{
}

void Context::initialize(FILE* outfd, FILE* infd)
{
  DoutEntering(dc::notice, "Context::initialize(" << outfd << ", " << infd << ")");

  // Only initialize the terminal::Context once.
  ASSERT(!initialized_);

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

// Leave ncurses before disabling AVA-owned protocols so the handed-off process receives shell presentation with clean input modes.
void Context::leave_terminal_for_handoff()
{
  static_cast<void>(::endwin());
  keyboard_protocols_.stop();
  mouse_input_.stop();
  cursor_state_.release(this);
}

// Resume through doupdate so ncurses repaints its retained virtual screen before AVA-owned protocols become active again.
void Context::restore_terminal_after_handoff()
{
  refresh_geometry_from_kernel();
  static_cast<void>(::clearok(::curscr, TRUE));
  static_cast<void>(::doupdate());
  mouse_input_.start(*this);
  keyboard_protocols_.start(*this);
  cursor_state_.reapply(this);
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
  DoutEntering(dc::notice | continued_cf, "Context::create_color_pair(" << foreground << ", " << background << ") = ");
  return priv_create_color_pair(terminal_color_index(foreground), terminal_color_index(background));
}

// Create a pair from one portable palette foreground and one resolved RGB/default background.
ColorPair Context::create_color_pair(ColorIndex foreground, Color background)
{
  DoutEntering(dc::notice | continued_cf, "Context::create_color_pair(" << utils::to_string(foreground) << ", " << background << ") = ");
  return priv_create_color_pair(static_cast<int>(foreground), terminal_color_index(background));
}

// Create a pair from one resolved RGB/default foreground and one portable palette background.
ColorPair Context::create_color_pair(Color foreground, ColorIndex background)
{
  DoutEntering(dc::notice | continued_cf, "Context::create_color_pair(" << foreground << ", " << utils::to_string(background) << ") = ");
  return priv_create_color_pair(terminal_color_index(foreground), static_cast<int>(background));
}

// Create a pair directly from two portable palette indexes.
ColorPair Context::create_color_pair(ColorIndex foreground, ColorIndex background)
{
  DoutEntering(dc::notice | continued_cf, "Context::create_color_pair(" << utils::to_string(foreground) << ", " << utils::to_string(background) << ") = ");
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

// Synchronize ncurses geometry only when the kernel reports a real size change, avoiding synthetic KEY_RESIZE events on no-op refreshes.
//static
void Context::refresh_geometry_from_kernel() noexcept
{
  winsize size{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_row == 0 || size.ws_col == 0 || ::stdscr == nullptr)
    return;
  auto const rows = static_cast<int>(size.ws_row);
  auto const cols = static_cast<int>(size.ws_col);
  if (::is_term_resized(rows, cols) == FALSE)
    return;
  static_cast<void>(::resizeterm(rows, cols));
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

std::string_view Context::keyname(int c) const
{
  char const* result = ::keyname(c);
  return result ? result : "UNKNOWN KEY";
}

bool Context::flush_raw()
{
  // Initialize the terminal or bind an external test screen's output before writing.
  ASSERT(output_file_);
  bool success = std::fflush(output_file_) == 0;
  Dout(dc::warning(!success)|error_cf, "std::fflush");
  return success;
}

bool Context::write_raw_sequence(std::string_view sequence, bool flush)
{
  // Initialize the terminal or bind an external test screen's output before writing.
  ASSERT(output_file_);
  size_t count = std::fwrite(sequence.data(), 1, sequence.size(), output_file_);
  Dout(dc::warning(count != sequence.size())|error_cf, "std::fwrite(..., 1, " << sequence.size() << ", output_file_) = " << count);
  return count == sequence.size() && (!flush || flush_raw());
}

// Ask the terminal for its default background color without exposing raw escape ownership outside Context.
bool Context::query_background_color()
{
  return write_raw_sequence(kQueryBackgroundColor);
}

// Replay one byte retained by the bounded keyboard negotiation before ncurses reads the terminal descriptor.
bool Context::try_get_buffered_keyboard_input(wint_t* wch)
{
  return keyboard_protocols_.try_get_wch(wch);
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

int Context::read_wch(wint_t* wch_out)
{
  // Canonical Context replay has first authority over bytes consumed by bounded startup negotiation. It returns logical wide characters
  // with ncurses' ordinary OK status; only after that queue is empty may wget_wch read the terminal and report OK, KEY_CODE_YES, or ERR.
  if (AI_UNLIKELY(keyboard_protocols_.try_get_wch(wch_out)))
    return OK;
  return ::get_wch(wch_out);
}

Key Context::read_curses_key(wint_t& value)
{
  int const result = read_wch(&value);
  if (result == ERR)
    return Key::Unknown;
  if (result == KEY_CODE_YES)
  {
    struct Entry
    {
      std::string_view name;
      Key key;
    };

    static constexpr std::array<Entry, 35> table = {{
      {}, {}, {},
      {"kri", Key::ShiftArrowUp},
      {"kind", Key::ShiftArrowDown},
      {"kEND6", Key::ShiftCtrlEnd},
      {}, {},
      {"kEND5", Key::CtrlEnd},
      {"kHOM6", Key::ShiftCtrlHome},
      {"kRIT6", Key::ShiftCtrlArrowRight},
      {"kRIT4", Key::ShiftAltArrowRight},
      {"kHOM5", Key::CtrlHome},
      {"kRIT5", Key::CtrlArrowRight},
      {},
      {"kLFT6", Key::ShiftCtrlArrowLeft},
      {"kLFT4", Key::ShiftAltArrowLeft},
      {"kDN3", Key::AltArrowDown},
      {"kLFT5", Key::CtrlArrowLeft},
      {"kDN2", Key::ShiftArrowDown},
      {"kEND2", Key::ShiftEnd},
      {}, {},
      {"kRIT3", Key::AltArrowRight},
      {"kHOM2", Key::ShiftHome},
      {"kRIT2", Key::ShiftArrowRight},
      {},
      {"kUP3", Key::AltArrowUp},
      {"kLFT3", Key::AltArrowLeft},
      {"kUP2", Key::ShiftArrowUp},
      {"kLFT2", Key::ShiftArrowLeft},
      {},
      {"kDC3", Key::AltDelete},
      {},
      {"kDC2", Key::ShiftDelete},
    }};

    std::string_view const key_name = keyname(value);

    if (3 <= key_name.size() && key_name.size() <= 5)
    {
      static std::array<unsigned char, 256> asso_values = {
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        15, 13,  1,  3,  0, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 15, 35, 35,
        10, 35, 35,  5, 35, 35, 35, 35,  0,  4,
        10, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
         0, 35, 35, 35, 35,  0, 35, 35, 35, 35,
         0, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
        35, 35, 35, 35, 35, 35
      };

      unsigned int const key =
        key_name.size() + asso_values[static_cast<unsigned char>(key_name[2])] + asso_values[static_cast<unsigned char>(key_name.back())];

      if (key < table.size())
      {
        Entry const& entry = table[key];
        if (key_name == entry.name)
          return entry.key;
      }
    }

    switch (static_cast<int>(value))
    {
      case KEY_ENTER:
        return Key::Enter;
      case KEY_BACKSPACE:
        return Key::Backspace;
#ifdef KEY_BTAB
      case KEY_BTAB:
        return Key::ShiftTab;
#endif
#if defined(KEY_SDC) && (!defined(KEY_DC) || KEY_SDC != KEY_DC)
      case KEY_SDC:
        return Key::ShiftDelete;
#endif
#ifdef KEY_DC
      case KEY_DC:
        return Key::Delete;
#endif
#ifdef KEY_IC
      case KEY_IC:
        return Key::Insert;
#endif
#ifdef KEY_CLEAR
      case KEY_CLEAR:
        return Key::Clear;
#endif
      case KEY_UP:
        return Key::ArrowUp;
      case KEY_DOWN:
        return Key::ArrowDown;
      case KEY_LEFT:
        return Key::ArrowLeft;
      case KEY_RIGHT:
        return Key::ArrowRight;
#ifdef KEY_SLEFT
      case KEY_SLEFT:
        return Key::ShiftArrowLeft;
#endif
#ifdef KEY_SRIGHT
      case KEY_SRIGHT:
        return Key::ShiftArrowRight;
#endif
#ifdef KEY_SUP
      case KEY_SUP:
        return Key::ShiftArrowUp;
#endif
#ifdef KEY_SDOWN
      case KEY_SDOWN:
        return Key::ShiftArrowDown;
#endif
#ifdef KEY_SR
      case KEY_SR:
        return Key::ShiftArrowUp;
#endif
#ifdef KEY_SF
      case KEY_SF:
        return Key::ShiftArrowDown;
#endif
#ifdef KEY_SHOME
      case KEY_SHOME:
        return Key::ShiftHome;
#endif
#ifdef KEY_SEND
      case KEY_SEND:
        return Key::ShiftEnd;
#endif
      case KEY_PPAGE:
        return Key::PageUp;
      case KEY_NPAGE:
        return Key::PageDown;
#ifdef KEY_HOME
      case KEY_HOME:
        return Key::Home;
#endif
#ifdef KEY_END
      case KEY_END:
        return Key::End;
#endif
#ifdef KEY_F
      case KEY_F(1):
        return Key::F1;
      case KEY_F(2):
        return Key::F2;
      case KEY_F(3):
        return Key::F3;
      case KEY_F(4):
        return Key::F4;
      case KEY_F(5):
        return Key::F5;
      case KEY_F(6):
        return Key::F6;
      case KEY_F(7):
        return Key::F7;
      case KEY_F(8):
        return Key::F8;
      case KEY_F(9):
        return Key::F9;
      case KEY_F(10):
        return Key::F10;
      case KEY_F(11):
        return Key::F11;
      case KEY_F(12):
        return Key::F12;
#endif
#ifdef KEY_RESIZE
      case KEY_RESIZE:
        return Key::Resize;
#endif
#ifdef KEY_MOUSE
      case KEY_MOUSE:
        return Key::Mouse;
#endif
      default:
        return Key::Unknown;
    }
  }

  wchar_t const character = static_cast<wchar_t>(value);
  if (character == L'\r')
    return Key::Enter;
  if (character == L'\n')
    return Key::ShiftEnter;
  if (character == L'\t')
    return Key::Tab;
  if (character == L' ')
    return Key::Space;
  if (character == 0x00)
    return Key::CtrlSpace;
  if (character == 0x1B)
    return Key::Escape;
  if (character == 0x01)
    return Key::CtrlA;
  if (character == 0x02)
    return Key::CtrlB;
  if (character == 0x03)
    return Key::CtrlC;
  if (character == 0x04)
    return Key::CtrlD;
  if (character == 0x05)
    return Key::CtrlE;
  if (character == 0x06)
    return Key::CtrlF;
  if (character == 0x07)
    return Key::CtrlG;
  if (character == 0x08)
    return Key::CtrlH;
  if (character == 0x0B)
    return Key::CtrlK;
  if (character == 0x0C)
    return Key::CtrlL;
  if (character == 0x1F)
    return Key::CtrlMinus;
  if (character == 0x0E)
    return Key::CtrlN;
  if (character == 0x0F)
    return Key::CtrlO;
  if (character == 0x10)
    return Key::CtrlP;
  if (character == 0x11)
    return Key::CtrlQ;
  if (character == 0x12)
    return Key::CtrlR;
  if (character == 0x13)
    return Key::CtrlS;
  if (character == 0x14)
    return Key::CtrlT;
  if (character == 0x15)
    return Key::CtrlU;
  if (character == 0x16)
    return Key::CtrlV;
  if (character == 0x17)
    return Key::CtrlW;
  if (character == 0x18)
    return Key::CtrlX;
  if (character == 0x19)
    return Key::CtrlY;
  if (character == 0x1A)
    return Key::CtrlZ;
  if (character == 0x1C)
    return Key::Unknown;                // Not handled.
  if (character == 0x1D)
    return Key::CtrlRightBracket;
  if (character == 0x1E)
    return Key::Unknown;                // Not handled.
  if (character == 0x7F)
    return Key::Backspace;
  // Paranoia check: all control characters should be handled.
  ASSERT(character >= 0x20);
  return Key::WideCharacter;
}

std::optional<wchar_t> Context::read_plain_wide_character()
{
  wint_t value = 0;
  auto const result = read_wch(&value);
  if (result == ERR || result == KEY_CODE_YES)
    return std::nullopt;
  return static_cast<wchar_t>(value);
}

bool Context::append_escape_sequence_character(std::string& consumed_out)
{
  wint_t value = 0;
  auto const result = read_wch(&value);
  if (result == ERR)
    return false;
  if (result == KEY_CODE_YES)
  {
    if (static_cast<int>(value) == KEY_BACKSPACE)
      consumed_out.push_back('\x7f');
    else
      return false;
  }
  else if (auto encoded = runtime_input::encode_wide_character(static_cast<wchar_t>(value)))
    consumed_out += *encoded;
  return true;
}

} // namespace ava::tui::terminal
