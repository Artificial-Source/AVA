#include "sys.h"
#include "BasicWindow.h"
#include "utils/macros.h"

#include <array>
#include <cstdarg>
#include <string>
#include <utility>
#include <vector>
#include "debug.h"

// This header must be included last.
#include "private_convert.h"

namespace ava::tui::terminal {

struct BasicWindow::Handle
{
 private:
  WINDOW* handle_;

 private:
  static int screen_max_row(Position screen_pos, Dimension screen_size)
  {
    // The caller must pass a positive screen height; a zero height would compute an inclusive maximum row above the window.
    ASSERT(screen_size.height() > 0);
    return static_cast<int>(screen_pos.row() + screen_size.height() - 1);
  }

  static int screen_max_col(Position screen_pos, Dimension screen_size)
  {
    // The caller must pass a positive screen width; a zero width would compute an inclusive maximum column left of the window.
    ASSERT(screen_size.width() > 0);
    return static_cast<int>(screen_pos.col() + screen_size.width() - 1);
  }

  static std::vector<cchar_t> convert_to_cchar_vector(ComplexChar const* str, int n)
  {
    // The caller must pass a non-null ComplexChar array; do not forward a nullable pointer.
    ASSERT(str);
    // The caller must pass a non-negative element count; validate the length at the call site.
    ASSERT(n >= 0);
    std::vector<cchar_t> result;
    result.reserve(static_cast<size_t>(n) + 1);
    for (int i = 0; i < n; ++i)
      result.push_back(convert_to_cchar(str[i]));
    result.push_back({});
    return result;
  }

  static void convert_from_cchar_array(cchar_t const* src, ComplexChar* dest, int n)
  {
    // The caller must pass a non-null source cchar_t array; do not forward a nullable buffer.
    ASSERT(src);
    // The caller must pass a non-null destination buffer with room for n ComplexChar elements; allocate it before calling.
    ASSERT(dest);
    // The caller must pass a non-negative element count; validate the length at the call site.
    ASSERT(n >= 0);
    for (int i = 0; i < n; ++i)
      dest[i] = convert_to_ComplexChar(src[i]);
  }

 private:
  void default_window_initialization()
  {
    DoutEntering(dc::terminal, "BasicWindow::Handle::default_window_initialization() [" << this << " with handle_ = " << handle_ << "]");

    [[maybe_unused]] int res;
    res = ::keypad(handle_, TRUE);
    // keypad returns ERR when the WINDOW handle is invalid; call default_window_initialization only from an Impl constructor holding a live ncurses window.
    ASSERT(res == OK);
    // Block on calls to get_wch: use a dedicated thread to get input.
    res = ::nodelay(handle_, FALSE);
    // nodelay returns ERR when the WINDOW handle is invalid; this initializer requires a live ncurses window.
    ASSERT(res == OK);
    // Wait after seeing an ESC for more characters to allow a keyboard to send a full escape sequence.
    res = ::notimeout(handle_, FALSE);
    // notimeout returns ERR when the WINDOW handle is invalid; this initializer requires a live ncurses window.
    ASSERT(res == OK);
  }

 public:
  // Construct an Handle representing stdscr.
  Handle() : handle_(stdscr) { default_window_initialization(); }

  // Wrap an ncurses WINDOW handle returned by a window-creation function.
  // The pointer must be non-null and is owned by this Handle, except for stdscr which is owned by ncurses itself.
  explicit Handle(WINDOW* handle) : handle_(handle)
  {
    // The caller passed a null WINDOW handle; construct Impl only from a successful ncurses window-creation call (or stdscr).
    ASSERT(handle_);
  }

  static WINDOW* newpad(Dimension size)
  {
    // https://invisible-island.net/ncurses/man/curs_pad.3x.html
    //
    // newpad creates and returns a pointer to a new pad data structure with the given number of lines and columns.
    // A pad is not restricted by the screen size and is refreshed with explicit source and destination rectangles.
    return ::newpad(size.height(), size.width());
  }

  Handle(Dimension size, Position pos)
  {
    // https://invisible-island.net/ncurses/man/curs_window.3x.html
    //
    // Calling newwin creates and returns a pointer to a new window with the given number of lines and columns. The
    // upper left-hand corner of the window is at line begin_y, column begin_x. If either nlines or ncols is zero,
    // they default to LINES - begin_y and COLS - begin_x.
    handle_ = ::newwin(size.height(), size.width(), pos.row(), pos.col());
    // newwin returns null when the requested window does not fit on the screen; clamp the requested size and position to the screen before constructing a
    // Window.
    ASSERT(handle_);
    default_window_initialization();
  }

  ~Handle()
  {
    if (handle_ == stdscr)
      return;
    // https://invisible-island.net/ncurses/man/curs_window.3x.html
    //
    // Calling delwin deletes the named window, freeing all memory associated with it. Subwindows must be deleted
    // before the main window can be deleted.
    ::delwin(handle_);
  }

  WINDOW* subwin(Dimension size, Position pos)
  {
    // https://invisible-island.net/ncurses/man/curs_window.3x.html
    //
    // Calling subwin creates and returns a pointer to a new window with the given number of lines and columns. The
    // window is at position (begin_y, begin_x) on the screen. The subwindow shares memory with the window orig, its
    // ancestor, so changes made to one window will affect both windows.
    return ::subwin(handle_, size.height(), size.width(), pos.row(), pos.col());
  }

  WINDOW* derwin(Dimension size, Position pos)
  {
    // https://invisible-island.net/ncurses/man/curs_window.3x.html
    //
    // Calling derwin is the same as calling subwin, except that begin_y and begin_x are relative to the origin of the
    // window orig rather than the screen. There is no difference between the subwindows and the derived windows.
    return ::derwin(handle_, size.height(), size.width(), pos.row(), pos.col());
  }

  int derwin(Position pos)
  {
    // https://invisible-island.net/ncurses/man/curs_window.3x.html
    //
    // Calling mvderwin moves a derived window (or subwindow) inside its parent window. The screen-relative parameters
    // of the window are not changed. This routine is used to display different parts of the parent window at the same
    // physical position on the screen.
    return ::mvderwin(handle_, pos.row(), pos.col());
  }

  void syncup()
  {
    // https://invisible-island.net/ncurses/man/curs_window.3x.html
    //
    // Calling wsyncup touches all locations in ancestors of win that are changed in win. If syncok is called with
    // second argument TRUE then wsyncup is called automatically whenever there is a change in the window.
    ::wsyncup(handle_);
  }

  void cursyncup()
  {
    // https://invisible-island.net/ncurses/man/curs_window.3x.html
    //
    // wcursyncup updates the current cursor position of all ancestors of the window to reflect the current cursor
    // position of this window.
    ::wcursyncup(handle_);
  }

  int syncok(bool enabled)
  {
    // https://invisible-island.net/ncurses/man/curs_window.3x.html
    //
    // If syncok is called with second argument TRUE then wsyncup is called automatically whenever there is a change in
    // the window.
    return ::syncok(handle_, enabled);
  }

  void erase()
  {
    // https://invisible-island.net/ncurses/man/curs_clear.3x.html
    //
    // The erase and werase routines copy blanks to every position in the window, clearing the screen.
    ::werase(handle_);
  }

  int clear()
  {
    // https://invisible-island.net/ncurses/man/curs_clear.3x.html
    //
    // wclear clears the window like werase and also arranges for the next refresh to clear and repaint the screen.
    return ::wclear(handle_);
  }

  int clrtobot()
  {
    // https://invisible-island.net/ncurses/man/curs_clear.3x.html
    //
    // wclrtobot clears from the cursor to the end of the window, inclusive of the cursor line after the cursor.
    return ::wclrtobot(handle_);
  }

  int clrtoeol()
  {
    // https://invisible-island.net/ncurses/man/curs_clear.3x.html
    //
    // wclrtoeol clears from the cursor to the end of the current line.
    return ::wclrtoeol(handle_);
  }

  void refresh()
  {
    // https://invisible-island.net/ncurses/man/curs_refresh.3x.html
    //
    // The refresh and wrefresh routines (or wnoutrefresh and doupdate) must be called to get any output on the
    // terminal, as other routines merely manipulate data structures. The routine wrefresh copies the named window to
    // the physical terminal screen.
    ::wrefresh(handle_);
  }

  int wnoutrefresh()
  {
    // https://invisible-island.net/ncurses/man/curs_refresh.3x.html
    //
    // wnoutrefresh copies the window to the virtual screen without updating the physical terminal until doupdate.
    return ::wnoutrefresh(handle_);
  }

  int redrawwin()
  {
    // https://invisible-island.net/ncurses/man/curs_refresh.3x.html
    //
    // redrawwin marks the entire window as changed so the next refresh repaints it completely.
    return ::redrawwin(handle_);
  }

  int wredrawln(int beg_line, int num_lines)
  {
    // https://invisible-island.net/ncurses/man/curs_refresh.3x.html
    //
    // wredrawln marks a range of lines as changed so the next refresh repaints those lines.
    return ::wredrawln(handle_, beg_line, num_lines);
  }

  int clearok(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_outopts.3x.html
    //
    // clearok controls whether the next refresh clears and repaints the screen from scratch.
    return ::clearok(handle_, bf);
  }

  void idcok(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_outopts.3x.html
    //
    // idcok controls use of the terminal insert/delete character feature for this window.
    ::idcok(handle_, bf);
  }

  int idlok(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_outopts.3x.html
    //
    // idlok controls use of the terminal insert/delete line feature for this window.
    return ::idlok(handle_, bf);
  }

  void immedok(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_outopts.3x.html
    //
    // immedok controls whether each window change automatically refreshes the window immediately.
    ::immedok(handle_, bf);
  }

  int leaveok(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_outopts.3x.html
    //
    // leaveok controls whether refresh leaves the physical cursor where ncurses happens to leave it.
    return ::leaveok(handle_, bf);
  }

  int scrollok(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_outopts.3x.html
    //
    // scrollok controls whether output may scroll the window when the cursor moves past the bottom edge.
    return ::scrollok(handle_, bf);
  }

  int setscrreg(int top, int bot)
  {
    // https://invisible-island.net/ncurses/man/curs_outopts.3x.html
    //
    // wsetscrreg sets the scrolling region, limiting line scroll operations to the given inclusive line range.
    return ::wsetscrreg(handle_, top, bot);
  }

  void border_set(std::array<cchar_t, 8> const& b)
  {
    // https://invisible-island.net/ncurses/man/curs_border_set.3x.html
    //
    // The border_set and wborder_set functions draw a border around the edges of the current or specified window.
    ::wborder_set(handle_, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
  }

  int hline_set(ComplexChar const& complex_char, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_border_set.3x.html
    //
    // whline_set draws up to n horizontal line cells starting at the cursor without moving the cursor.
    cchar_t wch = convert_to_cchar(complex_char);
    return ::whline_set(handle_, &wch, n);
  }

  int hline_set(Position pos, ComplexChar const& complex_char, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_border_set.3x.html
    //
    // mvwhline_set first moves the cursor, then draws up to n horizontal line cells without moving the cursor.
    cchar_t wch = convert_to_cchar(complex_char);
    return ::mvwhline_set(handle_, pos.row(), pos.col(), &wch, n);
  }

  int vline_set(ComplexChar const& complex_char, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_border_set.3x.html
    //
    // wvline_set draws up to n vertical line cells downward from the cursor without moving the cursor.
    cchar_t wch = convert_to_cchar(complex_char);
    return ::wvline_set(handle_, &wch, n);
  }

  int vline_set(Position pos, ComplexChar const& complex_char, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_border_set.3x.html
    //
    // mvwvline_set first moves the cursor, then draws up to n vertical line cells without moving the cursor.
    cchar_t wch = convert_to_cchar(complex_char);
    return ::mvwvline_set(handle_, pos.row(), pos.col(), &wch, n);
  }

  void set_background(ComplexChar background, bool erase)
  {
    cchar_t const wch = convert_to_cchar(background);
    if (erase)
    {
      // https://invisible-island.net/ncurses/man/curs_bkgrnd.3x.html
      //
      // The wbkgrndset function manipulates the background of the named window. The background becomes a property of
      // the character and moves with the character through any scrolling and insert/delete line/character operations.
      ::wbkgrndset(handle_, &wch);
      this->erase();
    }
    else
    {
      // https://invisible-island.net/ncurses/man/curs_bkgrnd.3x.html
      //
      // The wbkgrnd function turns off the previous background attributes, logically ORs the requested attributes into
      // the window rendition, and applies this setting to every character position in that window.
      ::wbkgrnd(handle_, &wch);
    }
  }

  ComplexChar get_background() const
  {
    // https://invisible-island.net/ncurses/man/curs_bkgrnd.3x.html
    //
    // The getbkgrnd and wgetbkgrnd functions obtain the window's current background character and rendition.
    cchar_t background;
    ::wgetbkgrnd(handle_, &background);
    return convert_to_ComplexChar(background);
  }

  int attr_set(Rendition rendition)
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // wattr_set sets the window's current attributes and color pair; subsequent characters added to the window use
    // this rendition until it is changed again.
    int color_pair = rendition.color_pair().index();
    return ::wattr_set(handle_, convert_to_attr(rendition.attributes()), 0, &color_pair);
  }

  int attr_get(Rendition& rendition) const
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // wattr_get returns the window's current attributes and color pair used for subsequent output.
    attr_t attrs = A_NORMAL;
    NCURSES_PAIRS_T pair = 0;
    int extended_pair = 0;
    int res = ::wattr_get(handle_, &attrs, &pair, &extended_pair);
    if (res == OK)
    {
      // With valid ncurses usage and res == OK, extended_pair should be nonnegative.
      ASSERT(extended_pair >= 0);
      rendition = Rendition{{{}, static_cast<uint32_t>(extended_pair)}, convert_to_Attributes(attrs)};
    }
    return res;
  }

  int attr_on(Attributes attributes)
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // wattr_on turns on the named attributes of the window without disturbing other attributes or the color pair.
    return ::wattr_on(handle_, convert_to_attr(attributes), nullptr);
  }

  int attr_off(Attributes attributes)
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // wattr_off turns off the named attributes of the window without disturbing other attributes or the color pair.
    return ::wattr_off(handle_, convert_to_attr(attributes), nullptr);
  }

  int color_set(ColorPair color_pair)
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // wcolor_set sets the current color pair of the window for characters written after the call.
    int extended_pair = color_pair.index();
    return ::wcolor_set(handle_, 0, &extended_pair);
  }

  int chgat(int n, Rendition rendition)
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // wchgat changes the rendition of a given number of characters starting at the current cursor position; negative
    // n changes characters through the end of the line.
    return ::wchgat(handle_, n, convert_to_attr(rendition.attributes()), rendition.color_pair().index(), nullptr);
  }

  int chgat(Position pos, int n, Rendition rendition)
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // mvwchgat first moves the cursor to the requested window-relative position and then changes character rendition.
    return ::mvwchgat(handle_, pos.row(), pos.col(), n, convert_to_attr(rendition.attributes()), rendition.color_pair().index(), nullptr);
  }

  int standout()
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // wstandout turns on the best highlighting mode of the terminal for the window's subsequent output.
    return ::wstandout(handle_);
  }

  int standend()
  {
    // https://invisible-island.net/ncurses/man/curs_attr.3x.html
    //
    // wstandend turns off all attributes for subsequent output on the window.
    return ::wstandend(handle_);
  }

  // https://invisible-island.net/ncurses/man/curs_addstr.3x.html
  //
  // The addstr, addnstr, waddstr, and waddnstr routines write all characters of the null-terminated string str on the given window.
  // The n variants write at most n characters. The mv variants first move the cursor to the given Position.
  //
  void addstr(char const* str) { ::waddstr(handle_, str); }
  void addstr(char const* str, int n) { ::waddnstr(handle_, str, n); }
  void addstr(char8_t const* utf8_str) { ::waddstr(handle_, reinterpret_cast<char const*>(utf8_str)); }
  void addstr(char8_t const* utf8_str, int n) { ::waddnstr(handle_, reinterpret_cast<char const*>(utf8_str), n); }
  void addstr(Position pos, char const* str) { ::mvwaddstr(handle_, pos.row(), pos.col(), str); }
  void addstr(Position pos, char const* str, int n) { ::mvwaddnstr(handle_, pos.row(), pos.col(), str, n); }
  // Instead of using waddwstr, which would require application-side conversion from char8_t (utf8) to wchar_t,
  // it is better to just cast to `char const*` and let the terminal do that.
  void addstr(Position pos, char8_t const* utf8_str) { ::mvwaddstr(handle_, pos.row(), pos.col(), reinterpret_cast<char const*>(utf8_str)); }
  void addstr(Position pos, char8_t const* utf8_str, int n) { ::mvwaddnstr(handle_, pos.row(), pos.col(), reinterpret_cast<char const*>(utf8_str), n); }

  // https://invisible-island.net/ncurses/man/curs_addwstr.3x.html
  //
  // waddwstr writes a null-terminated wide-character string to the window starting at the cursor.
  // The n variants write at most n characters. The mv variants first move the cursor to the given Position.
  //
  int addstr(wchar_t const* str) { return ::waddwstr(handle_, str); }
  int addstr(wchar_t const* str, int n) { return ::waddnwstr(handle_, str, n); }
  int addstr(Position pos, wchar_t const* str) { return ::mvwaddwstr(handle_, pos.row(), pos.col(), str); }
  int addstr(Position pos, wchar_t const* str, int n) { return ::mvwaddnwstr(handle_, pos.row(), pos.col(), str, n); }

#if 0
  // This API is deliberately commented out.
  // Instead, set a different rendition with BasicWindow::attr_set and then write a string using addstr to build the cchar_t array.

  // https://invisible-island.net/ncurses/man/curs_add_wchstr.3x.html
  //
  // The wadd_wchstr functions copy the array of complex characters into the window image structure at and after the cursor position.
  // The n variants write at most n characters. The mv variants first move the cursor to the given Position.
  //
  int addstr(ComplexChar const* str)
  {
    // https://invisible-island.net/ncurses/man/curs_add_wchstr.3x.html
    //
    // The caller must pass a non-null, null-terminated ComplexChar array; do not forward a nullable pointer.
    ASSERT(str);
    int n = 0;
    while (str[n].cell_character().length() != 0) ++n;
    auto converted = convert_to_cchar_vector(str, n);
    return ::wadd_wchstr(handle_, converted.data());
  }

  int addstr(ComplexChar const* str, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_add_wchstr.3x.html
    //
    // wadd_wchnstr copies at most n complex characters into the window at and after the cursor without advancing it.
    auto converted = convert_to_cchar_vector(str, n);
    return ::wadd_wchnstr(handle_, converted.data(), n);
  }

  int addstr(Position pos, ComplexChar const* str)
  {
    // https://invisible-island.net/ncurses/man/curs_add_wchstr.3x.html
    //
    // mvwadd_wchstr moves the cursor, then copies complex characters into the window without advancing it.
    // The caller must pass a non-null, null-terminated ComplexChar array; do not forward a nullable pointer.
    ASSERT(str);
    int n = 0;
    while (str[n].cell_character().length() != 0) ++n;
    auto converted = convert_to_cchar_vector(str, n);
    return ::mvwadd_wchstr(handle_, pos.row(), pos.col(), converted.data());
  }

  int addstr(Position pos, ComplexChar const* str, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_add_wchstr.3x.html
    //
    // mvwadd_wchnstr moves the cursor, then copies at most n complex characters into the window.
    auto converted = convert_to_cchar_vector(str, n);
    return ::mvwadd_wchnstr(handle_, pos.row(), pos.col(), converted.data(), n);
  }
#endif

  // https://invisible-island.net/ncurses/man/curs_add_wch.3x.html
  //
  // The wadd_wch function places the complex character at the current cursor position of the specified window, then advances the cursor position.
  // The mv variant first move the cursor to the given Position.
  //
  // The wecho_wchar function is functionally equivalent to calling wadd_wch followed by wrefresh.
  //
  void addch(ComplexChar const& complex_char)
  {
    cchar_t wch = convert_to_cchar(complex_char);
    ::wadd_wch(handle_, &wch);
  }
  void addch(Position pos, ComplexChar const& complex_char)
  {
    cchar_t wch = convert_to_cchar(complex_char);
    ::mvwadd_wch(handle_, pos.row(), pos.col(), &wch);
  }
  void echochar(ComplexChar const& complex_char)
  {
    cchar_t wch = convert_to_cchar(complex_char);
    ::wecho_wchar(handle_, &wch);
  }

  int delch()
  {
    // https://invisible-island.net/ncurses/man/curs_delch.3x.html
    //
    // wdelch deletes the character under the cursor; characters to the right shift left and the last cell becomes
    // blank.
    return ::wdelch(handle_);
  }

  int delch(Position pos)
  {
    // https://invisible-island.net/ncurses/man/curs_delch.3x.html
    //
    // mvwdelch moves to the requested position and then deletes the character under the cursor.
    return ::mvwdelch(handle_, pos.row(), pos.col());
  }

  int insdelln(int n)
  {
    // https://invisible-island.net/ncurses/man/curs_deleteln.3x.html
    //
    // winsdelln inserts n blank lines above the cursor line when positive, or deletes lines when negative.
    return ::winsdelln(handle_, n);
  }

  int get_wch(wint_t& key)
  {
    // https://invisible-island.net/ncurses/man/curs_get_wch.3x.html
    //
    // wget_wch gets a wide character or function-key code from the window's input stream.
    return ::wget_wch(handle_, &key);
  }

  static int unget_wch(wchar_t key)
  {
    // https://invisible-island.net/ncurses/man/curs_get_wch.3x.html
    //
    // unget_wch pushes a wide character back onto the input queue so it is returned by a subsequent input call.
    return ::unget_wch(key);
  }

  int in_wch(ComplexChar& complex_char) const
  {
    // https://invisible-island.net/ncurses/man/curs_in_wch.3x.html
    //
    // win_wch extracts the complex character and rendition at the cursor without altering the window.
    cchar_t wch;
    int res = ::win_wch(handle_, &wch);
    complex_char = convert_to_ComplexChar(wch);
    return res;
  }

  int in_wch(Position pos, ComplexChar& complex_char) const
  {
    // https://invisible-island.net/ncurses/man/curs_in_wch.3x.html
    //
    // mvwin_wch moves to the requested position and extracts the complex character and rendition at that cell.
    cchar_t wch;
    int res = ::mvwin_wch(handle_, pos.row(), pos.col(), &wch);
    complex_char = convert_to_ComplexChar(wch);
    return res;
  }

  int instr(ComplexChar* str) const
  {
    // https://invisible-island.net/ncurses/man/curs_in_wchstr.3x.html
    //
    // win_wchstr reads complex characters from the cursor through the end of the line into the caller's buffer.
    // The caller must provide a non-null output buffer sized for a full window line plus terminator; size it from getmaxyx() before calling.
    ASSERT(str);
    int cols = getmaxx(handle_);
    std::vector<cchar_t> tmp(static_cast<size_t>(cols) + 1);
    int res = ::win_wchstr(handle_, tmp.data());
    convert_from_cchar_array(tmp.data(), str, cols);
    return res;
  }

  int instr(ComplexChar* str, int n) const
  {
    // https://invisible-island.net/ncurses/man/curs_in_wchstr.3x.html
    //
    // win_wchnstr reads at most n complex characters from the cursor into the caller's buffer.
    std::vector<cchar_t> tmp(static_cast<size_t>(n) + 1);
    int res = ::win_wchnstr(handle_, tmp.data(), n);
    convert_from_cchar_array(tmp.data(), str, n);
    return res;
  }

  int instr(Position pos, ComplexChar* str) const
  {
    // https://invisible-island.net/ncurses/man/curs_in_wchstr.3x.html
    //
    // mvwin_wchstr moves to the requested position and reads complex characters through the end of the line.
    // The caller must provide a non-null output buffer sized for a full window line plus terminator; size it from getmaxyx() before calling.
    ASSERT(str);
    int cols = getmaxx(handle_);
    std::vector<cchar_t> tmp(static_cast<size_t>(cols) + 1);
    int res = ::mvwin_wchstr(handle_, pos.row(), pos.col(), tmp.data());
    convert_from_cchar_array(tmp.data(), str, cols);
    return res;
  }

  int instr(Position pos, ComplexChar* str, int n) const
  {
    // https://invisible-island.net/ncurses/man/curs_in_wchstr.3x.html
    //
    // mvwin_wchnstr moves to the requested position and reads at most n complex characters from that cell.
    std::vector<cchar_t> tmp(static_cast<size_t>(n) + 1);
    int res = ::mvwin_wchnstr(handle_, pos.row(), pos.col(), tmp.data(), n);
    convert_from_cchar_array(tmp.data(), str, n);
    return res;
  }

  int ins_wch(ComplexChar const& complex_char)
  {
    // https://invisible-island.net/ncurses/man/curs_ins_wch.3x.html
    //
    // wins_wch inserts a complex character before the cursor and shifts following characters right.
    cchar_t wch = convert_to_cchar(complex_char);
    return ::wins_wch(handle_, &wch);
  }

  int ins_wch(Position pos, ComplexChar const& complex_char)
  {
    // https://invisible-island.net/ncurses/man/curs_ins_wch.3x.html
    //
    // mvwins_wch moves to the requested position and inserts a complex character before that cell.
    cchar_t wch = convert_to_cchar(complex_char);
    return ::mvwins_wch(handle_, pos.row(), pos.col(), &wch);
  }

  int insstr(wchar_t const* str)
  {
    // https://invisible-island.net/ncurses/man/curs_ins_wstr.3x.html
    //
    // wins_wstr inserts a wide-character string before the cursor, shifting existing cells right.
    return ::wins_wstr(handle_, str);
  }

  int insstr(wchar_t const* str, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_ins_wstr.3x.html
    //
    // wins_nwstr inserts at most n wide characters before the cursor, stopping at a null wide character.
    return ::wins_nwstr(handle_, str, n);
  }

  int insstr(Position pos, wchar_t const* str)
  {
    // https://invisible-island.net/ncurses/man/curs_ins_wstr.3x.html
    //
    // mvwins_wstr moves to the requested position and inserts a wide-character string before that cell.
    return ::mvwins_wstr(handle_, pos.row(), pos.col(), str);
  }

  int insstr(Position pos, wchar_t const* str, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_ins_wstr.3x.html
    //
    // mvwins_nwstr moves to the requested position and inserts at most n wide characters before that cell.
    return ::mvwins_nwstr(handle_, pos.row(), pos.col(), str, n);
  }

  int insstr(char const* str)
  {
    // https://invisible-island.net/ncurses/man/curs_insstr.3x.html
    //
    // winsstr inserts a narrow string before the cursor, shifting existing characters right until the line fills.
    return ::winsstr(handle_, str);
  }

  int insstr(char const* str, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_insstr.3x.html
    //
    // winsnstr inserts at most n narrow bytes before the cursor, shifting existing characters right.
    return ::winsnstr(handle_, str, n);
  }

  int insstr(Position pos, char const* str)
  {
    // https://invisible-island.net/ncurses/man/curs_insstr.3x.html
    //
    // mvwinsstr moves to the requested position and inserts a narrow string before that cell.
    return ::mvwinsstr(handle_, pos.row(), pos.col(), str);
  }

  int insstr(Position pos, char const* str, int n)
  {
    // https://invisible-island.net/ncurses/man/curs_insstr.3x.html
    //
    // mvwinsnstr moves to the requested position and inserts at most n narrow bytes before that cell.
    return ::mvwinsnstr(handle_, pos.row(), pos.col(), str, n);
  }

  int inwstr(wchar_t* str) const
  {
    // https://invisible-island.net/ncurses/man/curs_inwstr.3x.html
    //
    // winwstr extracts wide characters from the cursor through the end of the line into the caller's buffer.
    return ::winwstr(handle_, str);
  }

  int inwstr(wchar_t* str, int n) const
  {
    // https://invisible-island.net/ncurses/man/curs_inwstr.3x.html
    //
    // winnwstr extracts at most n wide characters from the cursor into the caller's buffer.
    return ::winnwstr(handle_, str, n);
  }

  int inwstr(Position pos, wchar_t* str) const
  {
    // https://invisible-island.net/ncurses/man/curs_inwstr.3x.html
    //
    // mvwinwstr moves to the requested position and extracts wide characters through the end of the line.
    return ::mvwinwstr(handle_, pos.row(), pos.col(), str);
  }

  int inwstr(Position pos, wchar_t* str, int n) const
  {
    // https://invisible-island.net/ncurses/man/curs_inwstr.3x.html
    //
    // mvwinnwstr moves to the requested position and extracts at most n wide characters from that cell.
    return ::mvwinnwstr(handle_, pos.row(), pos.col(), str, n);
  }

  static int curs_set(int visibility)
  {
    // https://invisible-island.net/ncurses/man/curs_kernel.3x.html
    //
    // curs_set changes the terminal cursor visibility and returns the previous visibility setting when supported.
    return ::curs_set(visibility);
  }

  int printw(char const* fmt, va_list args)
  {
    // https://invisible-island.net/ncurses/man/curs_printw.3x.html
    //
    // vw_printw performs printf-style formatted output to the window using a va_list.
    return ::vw_printw(handle_, fmt, args);
  }

  int printw(Position pos, char const* fmt, va_list args)
  {
    // https://invisible-island.net/ncurses/man/curs_printw.3x.html
    //
    // mvwprintw first moves the cursor to the requested position and then performs printf-style output.
    int res = ::wmove(handle_, pos.row(), pos.col());
    if (res == ERR)
      return res;
    return ::vw_printw(handle_, fmt, args);
  }

  WINDOW* subpad(Dimension size, Position pos)
  {
    // https://invisible-island.net/ncurses/man/curs_pad.3x.html
    //
    // subpad creates a subwindow within a pad; its position is relative to the parent pad and storage is shared.
    return ::subpad(handle_, size.height(), size.width(), pos.row(), pos.col());
  }

  int prefresh(Position pad_pos, Position screen_pos, Dimension screen_size)
  {
    // https://invisible-island.net/ncurses/man/curs_pad.3x.html
    //
    // prefresh copies a rectangle from the pad, starting at pad_pos, to an inclusive rectangle on the physical screen.
    return ::prefresh(handle_, pad_pos.row(), pad_pos.col(), screen_pos.row(), screen_pos.col(), screen_max_row(screen_pos, screen_size),
                      screen_max_col(screen_pos, screen_size));
  }

  int pnoutrefresh(Position pad_pos, Position screen_pos, Dimension screen_size)
  {
    // https://invisible-island.net/ncurses/man/curs_pad.3x.html
    //
    // pnoutrefresh stages a pad rectangle on the virtual screen; doupdate performs the physical update later.
    return ::pnoutrefresh(handle_, pad_pos.row(), pad_pos.col(), screen_pos.row(), screen_pos.col(), screen_max_row(screen_pos, screen_size),
                          screen_max_col(screen_pos, screen_size));
  }

  int pechochar(ComplexChar const& complex_char)
  {
    // https://invisible-island.net/ncurses/man/curs_pad.3x.html
    //
    // pecho_wchar adds a complex character to a pad and refreshes the pad using the viewport remembered by ncurses.
    cchar_t wch = convert_to_cchar(complex_char);
    return ::pecho_wchar(handle_, &wch);
  }

  int scrl(int n)
  {
    // https://invisible-island.net/ncurses/man/curs_scroll.3x.html
    //
    // wscrl scrolls the window up for positive n or down for negative n, subject to scrollok and scrolling region.
    return ::wscrl(handle_, n);
  }

  static char const* key_name(wint_t key)
  {
    // https://invisible-island.net/ncurses/man/curs_util.3x.html
    //
    // key_name returns a printable name for a wide character or function-key code.
    return ::key_name(key);
  }

  int move(Position pos)
  {
    // https://invisible-island.net/ncurses/man/curs_move.3x.html
    //
    // wmove relocates the cursor associated with the curses window win to
    // line y and column x. The terminal's cursor does not move until
    // refresh(3x) is called. The position (y, x) is relative to the upper
    // left-hand corner of the window, which has coordinates (0, 0).
    return ::wmove(handle_, pos.row(), pos.col());
  }

  int resize(Dimension size)
  {
    // https://invisible-island.net/ncurses/man/wresize.3x.html
    //
    // wresize reallocates storage for win, adjusting its dimensions to lines and columns.
    // If either dimension is larger than its current value, ncurses fills the expanded part
    // of the window with the window's background character as configured by wbkgrndset.
    return ::wresize(handle_, size.height(), size.width());
  }

  Position getyx() const
  {
    // https://invisible-island.net/ncurses/man/curs_getyx.3x.html
    //
    // getyx stores the current cursor row and column of the specified window in caller-provided variables.
    int y = ::getcury(handle_);
    int x = ::getcurx(handle_);
    // getcury/getcurx only return negative values for an invalid window; call getyx on a live Window.
    ASSERT(y >= 0 && x >= 0);
    return Position(static_cast<uint32_t>(y), static_cast<uint32_t>(x));
  }

  Position getbegyx() const
  {
    // https://invisible-island.net/ncurses/man/curs_getyx.3x.html
    //
    // getbegyx stores the beginning row and column of the specified window in screen coordinates.
    int y = ::getbegy(handle_);
    int x = ::getbegx(handle_);
    // getbegy/getbegx only return negative values for an invalid window; call getbegyx on a live Window.
    ASSERT(y >= 0 && x >= 0);
    return Position(static_cast<uint32_t>(y), static_cast<uint32_t>(x));
  }

  Dimension getmaxyx() const
  {
    // https://invisible-island.net/ncurses/man/curs_getyx.3x.html
    //
    // getmaxyx stores the size of the specified window as row and column counts.
    int y = ::getmaxy(handle_);
    int x = ::getmaxx(handle_);
    // getmaxy/getmaxx only return negative values for an invalid window; call getmaxyx on a live Window.
    ASSERT(y >= 0 && x >= 0);
    return Dimension(static_cast<uint32_t>(y), static_cast<uint32_t>(x));
  }

  std::optional<Position> getparyx() const
  {
    // https://invisible-island.net/ncurses/man/curs_getyx.3x.html
    //
    // getparyx stores a subwindow's beginning row and column relative to its parent, or (-1, -1) for no parent.
    int y = ::getpary(handle_);
    int x = ::getparx(handle_);
    if (y == -1 && x == -1)
      return std::nullopt;
    // The (-1, -1) no-parent case is handled above; any other negative coordinate means the window handle is invalid, so call getparyx on a live subwindow.
    ASSERT(y >= 0 && x >= 0);
    return Position(static_cast<uint32_t>(y), static_cast<uint32_t>(x));
  }

  bool enclose(Position pos) const
  {
    // https://invisible-island.net/ncurses/man/curs_mouse.3x.html
    //
    // wenclose tests whether the given screen-relative row and column fall inside the specified window.
    return ::wenclose(handle_, pos.row(), pos.col());
  }

  bool mouse_trafo(Position& pos, bool to_screen) const
  {
    // https://invisible-island.net/ncurses/man/curs_mouse.3x.html
    //
    // wmouse_trafo converts coordinates between screen-relative and window-relative coordinate systems when possible.
    int y = static_cast<int>(pos.row());
    int x = static_cast<int>(pos.col());
    bool const res = ::wmouse_trafo(handle_, &y, &x, to_screen);
    if (res)
    {
      // wmouse_trafo reported success, so the converted coordinates must be non-negative; pass a Position that maps inside the target coordinate system.
      ASSERT(y >= 0 && x >= 0);
      pos = Position(static_cast<uint32_t>(y), static_cast<uint32_t>(x));
    }
    return res;
  }

  bool is_cleared() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_cleared returns whether clearok has marked the window to be cleared on the next refresh.
    return ::is_cleared(handle_);
  }

  bool is_idcok() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_idcok returns whether use of terminal insert/delete character capabilities is enabled for the window.
    return ::is_idcok(handle_);
  }

  bool is_idlok() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_idlok returns whether use of terminal insert/delete line capabilities is enabled for the window.
    return ::is_idlok(handle_);
  }

  bool is_immedok() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_immedok returns whether window changes automatically trigger a refresh.
    return ::is_immedok(handle_);
  }

  bool is_keypad() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_keypad returns whether keypad translation is enabled for the window.
    return ::is_keypad(handle_);
  }

  bool is_leaveok() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_leaveok returns whether refresh may leave the physical cursor wherever ncurses chooses.
    return ::is_leaveok(handle_);
  }

  bool is_nodelay() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_nodelay returns whether input reads are configured to return immediately when no input is ready.
    return ::is_nodelay(handle_);
  }

  bool is_notimeout() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_notimeout returns whether escape-sequence timer behavior is disabled for the window.
    return ::is_notimeout(handle_);
  }

  bool is_pad() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_pad returns whether the window is a pad rather than a normal screen-bounded window.
    return ::is_pad(handle_);
  }

  bool is_scrollok() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_scrollok returns whether the window may scroll when output passes the bottom edge.
    return ::is_scrollok(handle_);
  }

  bool is_subwin() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_subwin returns whether the window is a subwindow sharing storage with another window.
    return ::is_subwin(handle_);
  }

  bool is_syncok() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // is_syncok returns whether window changes automatically propagate touched state to ancestors.
    return ::is_syncok(handle_);
  }

  int getdelay() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // wgetdelay returns the input delay for the window as set by nodelay or wtimeout.
    return ::wgetdelay(handle_);
  }

  WINDOW* getparent() const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // wgetparent returns a pointer to the parent WINDOW of a subwindow, or null if there is no parent.
    return ::wgetparent(handle_);
  }

  int getscrreg(ScrollRegion& region) const
  {
    // https://invisible-island.net/ncurses/man/curs_opaque.3x.html
    //
    // wgetscrreg stores the top and bottom row numbers of the window's scrolling region.
    return ::wgetscrreg(handle_, &region.top, &region.bottom);
  }

  int keypad(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_inopts.3x.html
    //
    // keypad enables recognition of a terminal's function keys.
    return ::keypad(handle_, bf);
  }

  int nodelay(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_inopts.3x.html
    //
    // nodelay configures the input character reading function to be non-blocking for window handle_.
    // If no input is ready, the reading function returns ERR. If disabled (bf is FALSE), the reading
    // function does not return until it has input.
    return ::nodelay(handle_, bf);
  }

  int notimeout(bool bf)
  {
    // https://invisible-island.net/ncurses/man/curs_inopts.3x.html
    //
    // When keypad has been called on a window and the input character reading
    // function reads an ambiguous prefix character (typically ESC) from it,
    // ncurses sets a timer while waiting for the next character. If the timer
    // elapses, ncurses interprets the prefix as an explicit press of the key
    // corresponding thereto, such as Escape for ESC.
    return ::notimeout(handle_, bf);
  }

  void timeout(int delay)
  {
    // https://invisible-island.net/ncurses/man/curs_inopts.3x.html
    //
    // wtimeout configures whether a curses input character reading function
    // called  on window win uses blocking or non-blocking reads.
    ::wtimeout(handle_, delay);
  }

#ifdef CWDEBUG
  void print_on(std::ostream& os) const
  {
    os << '{';
    print_members(os, "");
    os << '}';
  }

  void print_members(std::ostream& os, char const* prefix) const
  {
    os << prefix << "handle_:" << handle_;
  }
#endif
};

BasicWindow::BasicWindow(Dimension size, Position pos) : impl_(std::make_unique<Handle>(size, pos))
{
}

BasicWindow::BasicWindow(std::unique_ptr<Handle> impl) : impl_(std::move(impl))
{
}

BasicWindow::BasicWindow() = default;

void BasicWindow::init_as_stdscr()
{
  // Only call this function once and only on a default constructed BasicWindow. This should only be called from Context().
  ASSERT(!impl_);
  impl_ = std::make_unique<Handle>();
}

BasicWindow::~BasicWindow() = default;
BasicWindow::BasicWindow(BasicWindow&&) noexcept = default;
BasicWindow& BasicWindow::operator=(BasicWindow&&) noexcept = default;

BasicWindow BasicWindow::newpad(Dimension size)
{
  WINDOW* res = Handle::newpad(size);
  // newpad returns null when the requested pad dimensions are not positive; construct the pad with a positive Dimension.
  ASSERT(res);
  return BasicWindow(std::make_unique<Handle>(res));
}

BasicWindow BasicWindow::subwin(Dimension size, Position pos)
{
  return BasicWindow(std::make_unique<Handle>(impl_->subwin(size, pos)));
}

BasicWindow BasicWindow::subwin(Margin margin)
{
  Dimension size = getmaxyx();
  // The caller is responsible for making sure this is true.
  ASSERT(margin < size);
  Position pos = getbegyx();
  return subwin(size - margin, pos + margin);
}

BasicWindow BasicWindow::derwin(Dimension size, Position pos)
{
  return BasicWindow(std::make_unique<Handle>(impl_->derwin(size, pos)));
}

BasicWindow BasicWindow::derwin(Margin margin)
{
  Dimension size = getmaxyx();
  // The caller is responsible for making sure this is true.
  ASSERT(margin < size);
  Position pos{0, 0};
  return derwin(size - margin, pos + margin);
}

void BasicWindow::derwin(Position pos)
{
  [[maybe_unused]] int res = impl_->derwin(pos);
  // mvderwin returns ERR when the derived window would move outside its parent; keep pos within the parent's interior.
  ASSERT(res != ERR);
}

void BasicWindow::syncup()
{
  impl_->syncup();
}

void BasicWindow::cursyncup()
{
  impl_->cursyncup();
}

void BasicWindow::syncok(bool enabled)
{
  [[maybe_unused]] int res = impl_->syncok(enabled);
  // syncok returns ERR only when the window handle is invalid; call syncok on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::set_background(ComplexChar background, bool erase)
{
  impl_->set_background(background, erase);
}

ComplexChar BasicWindow::get_background() const
{
  return impl_->get_background();
}

void BasicWindow::attr_set(Rendition rendition)
{
  [[maybe_unused]] int res = impl_->attr_set(rendition);
  // wattr_set returns ERR when the window handle is invalid; call attr_set on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::attr_get(Rendition& rendition) const
{
  [[maybe_unused]] int res = impl_->attr_get(rendition);
  // wattr_get returns ERR when the window handle is invalid; call attr_get on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::attr_on(Attributes attributes)
{
  [[maybe_unused]] int res = impl_->attr_on(attributes);
  // wattr_on returns ERR when the window handle is invalid or the attributes are unsupported; call attr_on on a live Window with valid Attributes.
  ASSERT(res != ERR);
}

void BasicWindow::attr_off(Attributes attributes)
{
  [[maybe_unused]] int res = impl_->attr_off(attributes);
  // wattr_off returns ERR when the window handle is invalid or the attributes are unsupported; call attr_off on a live Window with valid Attributes.
  ASSERT(res != ERR);
}

void BasicWindow::color_set(ColorPair color_pair)
{
  [[maybe_unused]] int res = impl_->color_set(color_pair);
  // wcolor_set returns ERR when the color pair is not initialized or the window handle is invalid; create the pair with Context::create_color_pair and call
  // color_set on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::chgat(int n, Rendition rendition)
{
  [[maybe_unused]] int res = impl_->chgat(n, rendition);
  // wchgat returns ERR when the rendition cannot be applied from the cursor or the window handle is invalid; call chgat on a live Window with an initialized
  // ColorPair and keep n within the remaining line.
  ASSERT(res != ERR);
}

void BasicWindow::chgat(Position pos, int n, Rendition rendition)
{
  [[maybe_unused]] int res = impl_->chgat(pos, n, rendition);
  // mvwchgat returns ERR when pos is outside the window or the rendition cannot be applied; pass a Position inside the window and an initialized ColorPair.
  ASSERT(res != ERR);
}

void BasicWindow::standout()
{
  [[maybe_unused]] int res = impl_->standout();
  // wstandout returns ERR only when the window handle is invalid; call standout on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::standend()
{
  [[maybe_unused]] int res = impl_->standend();
  // wstandend returns ERR only when the window handle is invalid; call standend on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::erase()
{
  impl_->erase();
}

void BasicWindow::clear()
{
  [[maybe_unused]] int res = impl_->clear();
  // wclear returns ERR only when the window handle is invalid; call clear on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::clrtobot()
{
  [[maybe_unused]] int res = impl_->clrtobot();
  // wclrtobot returns ERR only when the window handle is invalid; call clrtobot on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::clrtoeol()
{
  [[maybe_unused]] int res = impl_->clrtoeol();
  // wclrtoeol returns ERR only when the window handle is invalid; call clrtoeol on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::refresh()
{
  impl_->refresh();
}

void BasicWindow::wnoutrefresh()
{
  [[maybe_unused]] int res = impl_->wnoutrefresh();
  // wnoutrefresh returns ERR when the window is a pad or the handle is invalid; call wnoutrefresh on a live non-pad Window.
  ASSERT(res != ERR);
}

void BasicWindow::redrawwin()
{
  [[maybe_unused]] int res = impl_->redrawwin();
  // redrawwin returns ERR only when the window handle is invalid; call redrawwin on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::wredrawln(int beg_line, int num_lines)
{
  [[maybe_unused]] int res = impl_->wredrawln(beg_line, num_lines);
  // wredrawln returns ERR when the line range falls outside the window or the handle is invalid; pass a beg_line/num_lines range within the window height on a
  // live Window.
  ASSERT(res != ERR);
}

void BasicWindow::clearok(bool bf)
{
  [[maybe_unused]] int res = impl_->clearok(bf);
  // clearok returns ERR only when the window handle is invalid; call clearok on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::idcok(bool bf)
{
  impl_->idcok(bf);
}

void BasicWindow::idlok(bool bf)
{
  [[maybe_unused]] int res = impl_->idlok(bf);
  // idlok returns ERR when the window handle is invalid or the terminal lacks insert/delete-line capability; check the terminal capability before enabling
  // idlok.
  ASSERT(res != ERR);
}

void BasicWindow::immedok(bool bf)
{
  impl_->immedok(bf);
}

void BasicWindow::leaveok(bool bf)
{
  [[maybe_unused]] int res = impl_->leaveok(bf);
  // leaveok returns ERR only when the window handle is invalid; call leaveok on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::scrollok(bool bf)
{
  [[maybe_unused]] int res = impl_->scrollok(bf);
  // scrollok returns ERR only when the window handle is invalid; call scrollok on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::setscrreg(int top, int bot)
{
  [[maybe_unused]] int res = impl_->setscrreg(top, bot);
  // wsetscrreg returns ERR when top/bot fall outside the window or top exceeds bot; pass a valid row range within the window height.
  ASSERT(res != ERR);
}

void BasicWindow::set_border(Border const& border)
{
  // Perpare a mask that represents the existence of a margin.
  Margin const margin = border.margin();
  int margin_mask = (margin.top > 0 ? Box::ts : 0) | (margin.bottom > 0 ? Box::bs : 0) | (margin.left > 0 ? Box::ls : 0) | (margin.right > 0 ? Box::rs : 0);

  std::array<cchar_t, 8> complex_characters;
  for (int i = 0; i < 8; ++i)
  {
    int pos = Box::index_to_pos[i] & margin_mask;
    complex_characters[i] = convert_to_cchar(border.box().get_complex_character(pos, Rendition{border.colors(i)}));
  }
  impl_->border_set(complex_characters);
}

void BasicWindow::hline_set(ComplexChar const& complex_char, int n)
{
  [[maybe_unused]] int res = impl_->hline_set(complex_char, n);
  // whline_set returns ERR when n extends past the window edge or the rendition is unusable; keep n within the remaining line width.
  ASSERT(res != ERR);
}

void BasicWindow::hline_set(Position pos, ComplexChar const& complex_char, int n)
{
  [[maybe_unused]] int res = impl_->hline_set(pos, complex_char, n);
  // mvwhline_set returns ERR when pos is outside the window or n extends past the edge; pass a Position inside the window and a fitting count.
  ASSERT(res != ERR);
}

void BasicWindow::vline_set(ComplexChar const& complex_char, int n)
{
  [[maybe_unused]] int res = impl_->vline_set(complex_char, n);
  // wvline_set returns ERR when n extends past the window bottom or the rendition is unusable; keep n within the remaining column height.
  ASSERT(res != ERR);
}

void BasicWindow::vline_set(Position pos, ComplexChar const& complex_char, int n)
{
  [[maybe_unused]] int res = impl_->vline_set(pos, complex_char, n);
  // mvwvline_set returns ERR when pos is outside the window or n extends past the bottom; pass a Position inside the window and a fitting count.
  ASSERT(res != ERR);
}

void BasicWindow::addstr(char const* str)
{
  impl_->addstr(str);
}

void BasicWindow::addstr(char8_t const* wstr)
{
  impl_->addstr(wstr);
}

void BasicWindow::addstr(Position pos, char const* str)
{
  impl_->addstr(pos, str);
}

void BasicWindow::addstr(Position pos, char8_t const* wstr)
{
  impl_->addstr(pos, wstr);
}

void BasicWindow::addstr(char const* str, int n)
{
  impl_->addstr(str, n);
}

void BasicWindow::addstr(char8_t const* wstr, int n)
{
  impl_->addstr(wstr, n);
}

void BasicWindow::addstr(Position pos, char const* str, int n)
{
  impl_->addstr(pos, str, n);
}

void BasicWindow::addstr(Position pos, char8_t const* wstr, int n)
{
  impl_->addstr(pos, wstr, n);
}

#if 0 // This is deliberately commented out.
void BasicWindow::addstr(ComplexChar const* str)
{
  [[maybe_unused]] int res = impl_->addstr(str);
  // wadd_wchstr returns ERR when the string does not fit at the cursor; if this disabled overload is re-enabled, ensure the text fits the window.
  ASSERT(res != ERR);
}

void BasicWindow::addstr(ComplexChar const* str, int n)
{
  [[maybe_unused]] int res = impl_->addstr(str, n);
  // wadd_wchnstr returns ERR when n characters do not fit at the cursor; if this disabled overload is re-enabled, clamp n to the remaining line.
  ASSERT(res != ERR);
}

void BasicWindow::addstr(Position pos, ComplexChar const* str)
{
  [[maybe_unused]] int res = impl_->addstr(pos, str);
  // mvwadd_wchstr returns ERR when pos is outside the window or the string does not fit; if this disabled overload is re-enabled, pass a Position inside the window and fitting text.
  ASSERT(res != ERR);
}

void BasicWindow::addstr(Position pos, ComplexChar const* str, int n)
{
  [[maybe_unused]] int res = impl_->addstr(pos, str, n);
  // mvwadd_wchnstr returns ERR when pos is outside the window or n characters do not fit; if this disabled overload is re-enabled, pass a Position inside the window and clamp n.
  ASSERT(res != ERR);
}
#endif

void BasicWindow::addstr(wchar_t const* str)
{
  [[maybe_unused]] int res = impl_->addstr(str);
  // waddwstr returns ERR when the string does not fit in the window; keep the text within the window bounds or scroll first.
  ASSERT(res != ERR);
}

void BasicWindow::addstr(wchar_t const* str, int n)
{
  [[maybe_unused]] int res = impl_->addstr(str, n);
#if CW_DEBUG
  if (AI_UNLIKELY(res == ERR))
  {
    // The bottom-right corner character was stored, but the cursor could not advance
    // past it (see the comment in BasicWindow.h). Verify that the cursor is indeed stuck
    // at the bottom-right corner, so that any other kind of error still asserts.
    Position const cursor = getyx();
    Dimension const size = getmaxyx();
    // Treat ERR as valid only at the bottom-right cell; investigate any other waddnwstr failure.
    ASSERT(cursor.row() + 1 == size.height() && cursor.col() + 1 == size.width());
  }
#endif
}

void BasicWindow::addstr(Position pos, wchar_t const* str)
{
  [[maybe_unused]] int res = impl_->addstr(pos, str);
  // mvwaddwstr returns ERR when pos is outside the window or the string does not fit; pass a Position inside the window and fitting text.
  ASSERT(res != ERR);
}

void BasicWindow::addstr(Position pos, wchar_t const* str, int n)
{
  [[maybe_unused]] int res = impl_->addstr(pos, str, n);
  // mvwaddnwstr returns ERR when pos is outside the window or n characters do not fit; pass a Position inside the window and clamp n.
  ASSERT(res != ERR);
}

void BasicWindow::addch(ComplexChar const& complex_char)
{
  impl_->addch(complex_char);
}

void BasicWindow::addch(Position pos, ComplexChar const& complex_char)
{
  impl_->addch(pos, complex_char);
}

void BasicWindow::echochar(ComplexChar const& complex_char)
{
  impl_->echochar(complex_char);
}

void BasicWindow::delch()
{
  [[maybe_unused]] int res = impl_->delch();
  // wdelch returns ERR when the cursor is at the lower-right corner of the window; avoid deleting at the last cell.
  ASSERT(res != ERR);
}

void BasicWindow::delch(Position pos)
{
  [[maybe_unused]] int res = impl_->delch(pos);
  // mvwdelch returns ERR when pos is outside the window or at its lower-right corner; pass a deletable Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::insdelln(int n)
{
  [[maybe_unused]] int res = impl_->insdelln(n);
  // winsdelln returns ERR only when the window handle is invalid; call insdelln on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::get_wch(wint_t& key)
{
  [[maybe_unused]] int res = impl_->get_wch(key);
  // wget_wch returns ERR for an invalid window or signal interruption; keep the Window alive and handle input-thread signals before calling get_wch.
  ASSERT(res != ERR);
}

void BasicWindow::unget_wch(wchar_t key)
{
  [[maybe_unused]] int res = Handle::unget_wch(key);
  // unget_wch returns ERR when the input pushback buffer is full; read pushed-back input before pushing another character.
  ASSERT(res != ERR);
}

void BasicWindow::in_wch(ComplexChar& complex_char) const
{
  [[maybe_unused]] int res = impl_->in_wch(complex_char);
  // win_wch returns ERR only when the window handle is invalid; call in_wch on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::in_wch(Position pos, ComplexChar& complex_char) const
{
  [[maybe_unused]] int res = impl_->in_wch(pos, complex_char);
  // mvwin_wch returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::instr(ComplexChar* str) const
{
  [[maybe_unused]] int res = impl_->instr(str);
  // win_wchstr returns ERR only when the window handle is invalid; call instr on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::instr(ComplexChar* str, int n) const
{
  [[maybe_unused]] int res = impl_->instr(str, n);
  // win_wchnstr returns ERR only when the window handle is invalid; call instr on a live Window.
  ASSERT(res != ERR);
}

void BasicWindow::instr(Position pos, ComplexChar* str) const
{
  [[maybe_unused]] int res = impl_->instr(pos, str);
  // mvwin_wchstr returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::instr(Position pos, ComplexChar* str, int n) const
{
  [[maybe_unused]] int res = impl_->instr(pos, str, n);
  // mvwin_wchnstr returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::ins_wch(ComplexChar const& complex_char)
{
  [[maybe_unused]] int res = impl_->ins_wch(complex_char);
  // wins_wch returns ERR when a character cannot be inserted at the cursor; position the cursor where an insert is possible before calling.
  ASSERT(res != ERR);
}

void BasicWindow::ins_wch(Position pos, ComplexChar const& complex_char)
{
  [[maybe_unused]] int res = impl_->ins_wch(pos, complex_char);
  // mvwins_wch returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::insstr(wchar_t const* str)
{
  [[maybe_unused]] int res = impl_->insstr(str);
  // wins_wstr returns ERR when the string cannot be inserted before the cursor; keep the inserted text within the window.
  ASSERT(res != ERR);
}

void BasicWindow::insstr(wchar_t const* str, int n)
{
  [[maybe_unused]] int res = impl_->insstr(str, n);
  // wins_nwstr returns ERR when n characters cannot be inserted; clamp n to what fits in the window.
  ASSERT(res != ERR);
}

void BasicWindow::insstr(Position pos, wchar_t const* str)
{
  [[maybe_unused]] int res = impl_->insstr(pos, str);
  // mvwins_wstr returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::insstr(Position pos, wchar_t const* str, int n)
{
  [[maybe_unused]] int res = impl_->insstr(pos, str, n);
  // mvwins_nwstr returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::insstr(char const* str)
{
  [[maybe_unused]] int res = impl_->insstr(str);
  // winsstr returns ERR when the string cannot be inserted before the cursor; keep the inserted text within the window.
  ASSERT(res != ERR);
}

void BasicWindow::insstr(char const* str, int n)
{
  [[maybe_unused]] int res = impl_->insstr(str, n);
  // winsnstr returns ERR when n bytes cannot be inserted; clamp n to what fits in the window.
  ASSERT(res != ERR);
}

void BasicWindow::insstr(Position pos, char const* str)
{
  [[maybe_unused]] int res = impl_->insstr(pos, str);
  // mvwinsstr returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::insstr(Position pos, char const* str, int n)
{
  [[maybe_unused]] int res = impl_->insstr(pos, str, n);
  // mvwinsnstr returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::inwstr(wchar_t* str) const
{
  [[maybe_unused]] int res = impl_->inwstr(str);
  // winwstr returns ERR only when the window handle is invalid; call inwstr on a live Window with a buffer sized for a full line.
  ASSERT(res != ERR);
}

void BasicWindow::inwstr(wchar_t* str, int n) const
{
  [[maybe_unused]] int res = impl_->inwstr(str, n);
  // winnwstr returns ERR only when the window handle is invalid; call inwstr on a live Window with a buffer of at least n + 1 wide characters.
  ASSERT(res != ERR);
}

void BasicWindow::inwstr(Position pos, wchar_t* str) const
{
  [[maybe_unused]] int res = impl_->inwstr(pos, str);
  // mvwinwstr returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::inwstr(Position pos, wchar_t* str, int n) const
{
  [[maybe_unused]] int res = impl_->inwstr(pos, str, n);
  // mvwinnwstr returns ERR when pos is outside the window; pass a Position inside the window.
  ASSERT(res != ERR);
}

void BasicWindow::curs_set(int visibility)
{
  [[maybe_unused]] int res = Handle::curs_set(visibility);
  // curs_set returns ERR when the terminal does not support the requested cursor visibility; check the cursor-visibility capability before requesting a change.
  ASSERT(res != ERR);
}

void BasicWindow::printw(char const* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  [[maybe_unused]] int res = impl_->printw(fmt, args);
  va_end(args);
  // vw_printw returns ERR when the formatted output does not fit in the window; keep printed text within the window bounds.
  ASSERT(res != ERR);
}

void BasicWindow::vprintw(char const* fmt, va_list varglist)
{
  [[maybe_unused]] int res = impl_->printw(fmt, varglist);
  // vw_printw returns ERR when the formatted output does not fit in the window; keep printed text within the window bounds.
  ASSERT(res != ERR);
}

void BasicWindow::printw(Position pos, char const* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  [[maybe_unused]] int res = impl_->printw(pos, fmt, args);
  va_end(args);
  // printw first moves the cursor; the wmove inside it returns ERR when pos is outside the window, so pass a Position inside the window.
  ASSERT(res != ERR);
}

BasicWindow BasicWindow::subpad(Dimension size, Position pos)
{
  WINDOW* res = impl_->subpad(size, pos);
  // subpad returns null when the requested sub-rectangle does not fit inside the pad; pass a size and position contained in the pad.
  ASSERT(res);
  return BasicWindow(std::make_unique<Handle>(res));
}

void BasicWindow::prefresh(Position pad_pos, Position screen_pos, Dimension screen_size)
{
  [[maybe_unused]] int res = impl_->prefresh(pad_pos, screen_pos, screen_size);
  // prefresh returns ERR when the requested rectangles fall outside the pad or the screen; clamp pad_pos, screen_pos, and screen_size to valid ranges.
  ASSERT(res != ERR);
}

void BasicWindow::pnoutrefresh(Position pad_pos, Position screen_pos, Dimension screen_size)
{
  [[maybe_unused]] int res = impl_->pnoutrefresh(pad_pos, screen_pos, screen_size);
  // pnoutrefresh returns ERR when the requested rectangles fall outside the pad or the screen; clamp pad_pos, screen_pos, and screen_size to valid ranges.
  ASSERT(res != ERR);
}

void BasicWindow::pechochar(ComplexChar const& complex_char)
{
  [[maybe_unused]] int res = impl_->pechochar(complex_char);
  // pecho_wchar returns ERR when the character cannot be added at the pad cursor; position the cursor inside the pad before echoing.
  ASSERT(res != ERR);
}

void BasicWindow::scrl(int n)
{
  [[maybe_unused]] int res = impl_->scrl(n);
  // wscrl returns ERR when scrolling is disabled or the window handle is invalid; enable scrolling with scrollok on a live Window before calling scrl.
  ASSERT(res != ERR);
}

void BasicWindow::key_name(wint_t key, std::string& name)
{
  char const* res = Handle::key_name(key);
  // key_name returns null for an unrecognized key code; pass a key value previously returned by get_wch.
  ASSERT(res);
  name = res;
}

void BasicWindow::move(Position pos)
{
  [[maybe_unused]] int res = impl_->move(pos);
  // wmove returns ERR when pos is outside the window; pass a Position inside the window bounds.
  ASSERT(res != ERR);
}

void BasicWindow::resize(Dimension size)
{
  [[maybe_unused]] int res = impl_->resize(size);
  // wresize returns ERR when the new dimensions are not positive or exceed screen or memory limits; resize to a positive Dimension that fits the screen.
  ASSERT(res != ERR);
}

Position BasicWindow::getyx() const
{
  return impl_->getyx();
}

Position BasicWindow::getbegyx() const
{
  return impl_->getbegyx();
}

Dimension BasicWindow::getmaxyx() const
{
  return impl_->getmaxyx();
}

std::optional<Position> BasicWindow::getparyx() const
{
  return impl_->getparyx();
}

bool BasicWindow::enclose(Position pos) const
{
  return impl_->enclose(pos);
}

bool BasicWindow::mouse_trafo(Position& pos, bool to_screen) const
{
  return impl_->mouse_trafo(pos, to_screen);
}

bool BasicWindow::is_cleared() const
{
  return impl_->is_cleared();
}

bool BasicWindow::is_idcok() const
{
  return impl_->is_idcok();
}

bool BasicWindow::is_idlok() const
{
  return impl_->is_idlok();
}

bool BasicWindow::is_immedok() const
{
  return impl_->is_immedok();
}

bool BasicWindow::is_keypad() const
{
  return impl_->is_keypad();
}

bool BasicWindow::is_leaveok() const
{
  return impl_->is_leaveok();
}

bool BasicWindow::is_nodelay() const
{
  return impl_->is_nodelay();
}

bool BasicWindow::is_notimeout() const
{
  return impl_->is_notimeout();
}

bool BasicWindow::is_pad() const
{
  return impl_->is_pad();
}

bool BasicWindow::is_scrollok() const
{
  return impl_->is_scrollok();
}

bool BasicWindow::is_subwin() const
{
  return impl_->is_subwin();
}

bool BasicWindow::is_syncok() const
{
  return impl_->is_syncok();
}

int BasicWindow::getdelay() const
{
  return impl_->getdelay();
}

ScrollRegion BasicWindow::getscrreg() const
{
  ScrollRegion region{};
  [[maybe_unused]] int res = impl_->getscrreg(region);
  // wgetscrreg returns ERR only when the window handle is invalid; call getscrreg on a live Window.
  ASSERT(res != ERR);
  return region;
}

void BasicWindow::keypad(bool bf)
{
  [[maybe_unused]] int res = impl_->keypad(bf);
  // Paranoia check: keypad should always succeed unless ncurses wasn't initialized yet.
  ASSERT(res != ERR);
}

void BasicWindow::nodelay(bool bf)
{
  [[maybe_unused]] int res = impl_->nodelay(bf);
  // Paranoia check: nodelay should always succeed unless ncurses wasn't initialized yet.
  ASSERT(res != ERR);
}

void BasicWindow::notimeout(bool bf)
{
  [[maybe_unused]] int res = impl_->notimeout(bf);
  // Paranoia check: notimeout should always succeed unless ncurses wasn't initialized yet.
  ASSERT(res != ERR);
}

void BasicWindow::timeout(int delay_ms)
{
  impl_->timeout(delay_ms);
}

void BasicWindow::addspaces(columns_t n, Rendition const& rendition)
{
  DoutEntering(dc::terminal, "BasicWindow::addspaces(" << n << ", " << rendition << ")");

  // A static array with 32 spaces.
  constexpr static columns_t number_of_spaces = 32;
  constexpr static auto spaces = [] {
    std::array<wchar_t, number_of_spaces> result;
    result.fill(L' ');
    return result;
  }();

  while (n > 0)
  {
    addstr(spaces.data(), std::min(n, number_of_spaces), rendition);
    if (AI_UNLIKELY(n > number_of_spaces))
    {
      n -= number_of_spaces;
      continue;
    }
    break;
  }
}

#ifdef CWDEBUG
void BasicWindow::print_on(std::ostream& os) const
{
  os << '{';
  impl_->print_members(os, "impl_->");
  os << '}';
}
#endif

} // namespace ava::tui::terminal
