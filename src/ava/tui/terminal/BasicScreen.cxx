#include "sys.h"
#include "BasicScreen.h"

#include "debug.h"              // ASSERT

// This header must be included last.
#include "private_convert.h"

namespace ava::tui::terminal {

#ifdef CWDEBUG
bool is_tui_thread();
#endif

struct BasicScreen::Handle
{
 private:
  SCREEN* handle_{};

 public:
  // Create a native screen of type using the borrowed outfd and infd streams on the registered TUI thread.
  Handle(char const* type, FILE* outfd, FILE* infd)
  {
    // Create screens only after Context::initialize has registered the TUI thread.
    ASSERT(is_tui_thread());
    handle_ = newterm(const_cast<char*>(type), outfd, infd);
    // This should never fail.
    ASSERT(handle_);
  }

  ~Handle()
  {
    if (!handle_)
      return;
    // Destroy the screen on the TUI thread before that thread exits.
    ASSERT(is_tui_thread());
    // It is normal that this fails if `outfd` and `infd` are not a TTY, as is always the case
    // because this object is only constructed from the testsuite.
    static_cast<void>(endwin());
    delscreen(handle_);
  }

  void use_as_term()
  {
    // Switch native screens only on the TUI thread, even when this wrapper was moved or retained.
    ASSERT(is_tui_thread());
    [[maybe_unused]] SCREEN* old_screen = set_term(handle_);
  }
};

BasicScreen::BasicScreen() = default;
BasicScreen::BasicScreen(char const* type, FILE* outfd, FILE* infd) : impl_(std::make_unique<Handle>(type, outfd, infd))
{
  use_as_term();
}

BasicScreen::~BasicScreen() = default;
BasicScreen::BasicScreen(BasicScreen&&) noexcept = default;
BasicScreen& BasicScreen::operator=(BasicScreen&&) noexcept = default;

void BasicScreen::use_as_term()
{
  impl_->use_as_term();
}

} // namespace ava::tui::terminal
