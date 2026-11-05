#pragma once

#include "debug.h"

#ifdef CWDEBUG
#include "ava/debug/print_members_on.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ava::app {

// The terminal emulator and debugger requested through AVA_DEBUG_TERMINAL and AVA_DEBUG_GDB.
struct DebugTerminalConfig
{
  std::string terminal;         // Terminal emulator program, invoked as `<terminal> -e <command> <args>...`.
  std::string gdb;              // Debugger program that attaches to AVA, or empty to only display debug output.

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Parse the raw values of the AVA_DEBUG_TERMINAL (terminal) and AVA_DEBUG_GDB (gdb) environment variables.
//
// Returns std::nullopt when terminal is null or empty: no debug terminal was requested.
// A terminal value of "1" selects konsole; any other value names the terminal program.
// A null, empty or "1" gdb value selects gdb, "0" disables attaching a debugger,
// and any other value names the debugger program.
[[nodiscard]] std::optional<DebugTerminalConfig> parse_debug_terminal_env(char const* terminal, char const* gdb);

// Return true when tty_path, the line printed by tty(1) without its newline, names a terminal device for debug output.
//
// Accepts only /dev/pts/<digits> and /dev/tty<letters-or-digits>; anything else, including
// "not a tty", is rejected.
[[nodiscard]] bool valid_debug_tty_path(std::string_view tty_path);

// Open a separate terminal window, route libcwd debug output to it and optionally attach gdb to this process from it.
//
// Construct this with config while AVA is still single-threaded, before libcwd is initialized
// and before core::Signals reserves any signals, so that the spawned terminal inherits a pristine
// signal state and core::Signals sees the attached debugger (leaving SIGINT to gdb).
// The constructor blocks until the window reported its tty (at most 10 seconds) and, when a
// debugger was requested, until that debugger attached (at most 60 seconds).
// Failures never throw: setup_error() describes them and enabled() reports whether libcwd output
// was nevertheless routed to the new window.
// The object must outlive all libcwd output, so keep it for the lifetime of the process.
class DebugTerminal final
{
 public:
  explicit DebugTerminal(DebugTerminalConfig config);
  ~DebugTerminal();

  DebugTerminal(DebugTerminal const&) = delete;
  DebugTerminal& operator=(DebugTerminal const&) = delete;
  DebugTerminal(DebugTerminal&&) = delete;
  DebugTerminal& operator=(DebugTerminal&&) = delete;

  // Return true when libcwd output was redirected to the tty of the spawned terminal window.
  [[nodiscard]] bool enabled() const noexcept;

  // Return an actionable description of the failed setup step, or an empty string after full success.
  //
  // May be nonempty while enabled() is true, for example when the debugger did not attach in time.
  [[nodiscard]] std::string const& setup_error() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ava::app
#endif // CWDEBUG
