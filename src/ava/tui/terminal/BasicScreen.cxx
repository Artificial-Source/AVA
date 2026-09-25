#include "sys.h"
#include "BasicScreen.h"

#include <sys/ioctl.h>
#include <unistd.h>
#include "debug.h"              // ASSERT

// This header must be included last.
#include "private_convert.h"

namespace ava::tui::terminal {

struct BasicScreen::Handle
{
 private:
  SCREEN* handle_{};

 public:
  Handle(char const* type, FILE* outfd, FILE* infd) : handle_(newterm(const_cast<char*>(type), outfd, infd))
  {
    // This should never fail.
    ASSERT(handle_);
  }

  ~Handle()
  {
    if (!handle_)
      return;
    // It is normal that this fails if `outfd` and `infd` are not a TTY, as is always the case
    // because this object is only constructed from the testsuite.
    static_cast<void>(endwin());
    delscreen(handle_);
  }

  void use_as_term() { [[maybe_unused]] SCREEN* old_screen = set_term(handle_); }
};

BasicScreen::BasicScreen() = default;
BasicScreen::BasicScreen(char const* type, FILE* outfd, FILE* infd) : impl_(std::make_unique<Handle>(type, outfd, infd))
{
  use_as_term();
}

BasicScreen::~BasicScreen() = default;
BasicScreen::BasicScreen(BasicScreen&&) noexcept = default;
BasicScreen& BasicScreen::operator=(BasicScreen&&) noexcept = default;

void BasicScreen::save_program_mode()
{
  static_cast<void>(def_prog_mode());
}

void BasicScreen::leave_program_mode()
{
  static_cast<void>(endwin());
}

void BasicScreen::restore_program_mode()
{
  static_cast<void>(reset_prog_mode());
}

// Synchronize ncurses' cached dimensions with the kernel without injecting redundant resize events.
void BasicScreen::refresh_geometry_from_kernel() noexcept
{
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_row == 0 || size.ws_col == 0 || stdscr == nullptr)
    return;
  auto const rows = static_cast<int>(size.ws_row);
  auto const cols = static_cast<int>(size.ws_col);
  if (is_term_resized(rows, cols) == FALSE)
    return;
  static_cast<void>(resizeterm(rows, cols));
}

void BasicScreen::use_as_term()
{
  impl_->use_as_term();
}

} // namespace ava::tui::terminal
