// Cover the pure environment and tty-path parsing used by the AVA_DEBUG_TERMINAL developer aid.
#include "sys.h"
// clang-format off
#include "ava/debug/debug_ostream_operators.h" // Must precede the test harness's debug includes.
#include "tests/support/test_harness.h"
#include "ava/app/DebugTerminal.h"
// clang-format on

#include <string>
#include <string_view>

namespace {

using ava::app::parse_debug_terminal_env;
using ava::app::valid_debug_tty_path;

void test_debug_terminal_env_absent_or_empty_disables()
{
  expect(!parse_debug_terminal_env(nullptr, nullptr), "unset AVA_DEBUG_TERMINAL requests no debug terminal");
  expect(!parse_debug_terminal_env("", "gdb"), "empty AVA_DEBUG_TERMINAL requests no debug terminal");
}

void test_debug_terminal_env_selects_terminal()
{
  auto const by_one = parse_debug_terminal_env("1", nullptr);
  expect(by_one && by_one->terminal == "konsole", "AVA_DEBUG_TERMINAL=1 selects konsole");
  auto const explicit_terminal = parse_debug_terminal_env("xterm", nullptr);
  expect(explicit_terminal && explicit_terminal->terminal == "xterm", "AVA_DEBUG_TERMINAL names the terminal program");
}

void test_debug_terminal_env_selects_debugger()
{
  for (char const* gdb : {static_cast<char const*>(nullptr), "", "1"})
  {
    auto const config = parse_debug_terminal_env("konsole", gdb);
    expect(config && config->gdb == "gdb", "unset, empty or 1 AVA_DEBUG_GDB selects gdb");
  }
  auto const disabled = parse_debug_terminal_env("konsole", "0");
  expect(disabled && disabled->gdb.empty(), "AVA_DEBUG_GDB=0 disables the debugger");
  auto const custom = parse_debug_terminal_env("konsole", "/opt/gdb/bin/gdb");
  expect(custom && custom->gdb == "/opt/gdb/bin/gdb", "AVA_DEBUG_GDB names the debugger program");
}

void test_debug_tty_path_validation()
{
  expect(valid_debug_tty_path("/dev/pts/3"), "pseudo-terminal paths are accepted");
  expect(valid_debug_tty_path("/dev/pts/12345"), "multi-digit pseudo-terminal paths are accepted");
  expect(valid_debug_tty_path("/dev/tty2"), "virtual console paths are accepted");
  expect(!valid_debug_tty_path(""), "an empty tty path is rejected");
  expect(!valid_debug_tty_path("not a tty"), "tty(1)'s failure text is rejected");
  expect(!valid_debug_tty_path("/tmp/x"), "paths outside /dev are rejected");
  expect(!valid_debug_tty_path("/dev/pts/"), "a pts path without number is rejected");
  expect(!valid_debug_tty_path("/dev/tty"), "the controlling-terminal alias is rejected");
  expect(!valid_debug_tty_path("/dev/pts/3/../../sda"), "path traversal is rejected");
  expect(!valid_debug_tty_path("/dev/pts/3\n"), "an embedded newline is rejected");
  expect(!valid_debug_tty_path(std::string_view("/dev/pts/3\0", 11)), "an embedded NUL is rejected");
}

}  // namespace

void run_debug_terminal_tests()
{
  test_debug_terminal_env_absent_or_empty_disables();
  test_debug_terminal_env_selects_terminal();
  test_debug_terminal_env_selects_debugger();
  test_debug_tty_path_validation();
}
