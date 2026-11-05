#include "sys.h"
#include "Application.h"
#ifdef CWDEBUG
#include "DebugTerminal.h"
#include "ava/debug/libcwd_output_sink.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include "debug.h"
#endif

namespace ava::app {

// Construct the AVA process Application after preparing its optional private
// test sink. The core base initializes libcwd before constructing allocators;
// the constructor body then records the routing marker through libcwd itself.
Application::Application() : core::Application(CWDEBUG_ONLY(prepare_debug()))
{
#ifdef CWDEBUG
  char const* test_name = std::getenv("AVA_TEST_NAME");
  if (test_name != nullptr && test_name[0] != '\0')
    Dout(dc::notice, "AVA libcwd routing marker: test=" << test_name);
#endif
}

Application::~Application() = default;

#ifdef CWDEBUG
//static
std::unique_ptr<debug::LibcwdOutputSink> Application::s_output_sink_;
//static
std::unique_ptr<DebugTerminal> Application::s_debug_terminal_;

// Choose where libcwd output goes before libcwd is initialized; returns true to force initialization.
//
// Test processes (AVA_TEST_NAME) install a process-lifetime private sink and
// force initialization only when the requested test output file could be enabled.
// Otherwise a nonempty AVA_DEBUG_TERMINAL opens a separate terminal window that
// receives libcwd output and, unless AVA_DEBUG_GDB=0, runs a debugger attached
// to this process; AVA blocks here until that debugger attached. This runs while
// the process is still single-threaded and before core::Signals reserves any
// signals, which DebugTerminal relies on. Every other process follows
// debug_init(): libcwd stays off unless the operator opts in with AVA_DEBUG_OUTPUT=1.

//static
bool Application::prepare_debug()
{
  char const* test_name = std::getenv("AVA_TEST_NAME");
  if (test_name != nullptr && test_name[0] != '\0')
  {
    s_output_sink_ = std::make_unique<debug::LibcwdOutputSink>(test_name);
    if (!s_output_sink_->setup_succeeded())
      std::cerr << "failed to configure libcwd output: " << s_output_sink_->setup_error() << '\n';

    return s_output_sink_->enabled();
  }

  std::optional<DebugTerminalConfig> debug_terminal_config = parse_debug_terminal_env(std::getenv("AVA_DEBUG_TERMINAL"), std::getenv("AVA_DEBUG_GDB"));
  if (!debug_terminal_config)
    return false;

  s_debug_terminal_ = std::make_unique<DebugTerminal>(std::move(*debug_terminal_config));
  if (!s_debug_terminal_->setup_error().empty())
    std::cerr << "AVA_DEBUG_TERMINAL: " << s_debug_terminal_->setup_error() << '\n';

  return s_debug_terminal_->enabled();
}
#endif

}  // namespace ava::app
