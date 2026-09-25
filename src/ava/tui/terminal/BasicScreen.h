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

  // Save ncurses' current program-terminal mode before temporarily returning control to another process.
  void save_program_mode();

  // Leave ncurses' program-terminal mode after save_program_mode() has captured it.
  void leave_program_mode();

  // Restore the program-terminal mode previously captured by save_program_mode().
  void restore_program_mode();

  // Refresh ncurses' dimensions from the kernel terminal size when they differ.
  //
  // Missing terminal geometry and uninitialized screens are ignored so resize checks remain safe during partial startup and tests.
  static void refresh_geometry_from_kernel() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Sets the current BasicScreen as terminal.
  void use_as_term();
};

} // namespace ava::tui::terminal
