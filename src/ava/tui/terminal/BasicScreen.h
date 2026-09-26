#pragma once

#include "ava/debug/print_members_on.h"
#include <memory>

namespace ava::tui::terminal {

// class BasicScreen
//
// Uses the PImpl idiom with `Handle`, a thin wrapper around an ncurses `SCREEN*` handle.
//
class BasicScreen
{
 private:
  struct Handle;
  std::unique_ptr<Handle> impl_;

 public:
  BasicScreen();
  BasicScreen(char const* type, FILE* outfd, FILE* infd);

  // Disallow copying; allow moving a BasicScreen.
  BasicScreen(BasicScreen const&) = delete;
  BasicScreen& operator=(BasicScreen const&) = delete;
  BasicScreen(BasicScreen&&) noexcept;
  BasicScreen& operator=(BasicScreen&&) noexcept;

  // The destructor must be defined in the .cxx file because of the std::unique_ptr<Handle> with incomplete `Handle`.
  ~BasicScreen();

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Sets the current BasicScreen as terminal.
  void use_as_term();
};

} // namespace ava::tui::terminal
