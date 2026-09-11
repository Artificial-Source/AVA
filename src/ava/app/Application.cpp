#include "sys.h"
#include "Application.h"
#ifdef CWDEBUG
#include "ava/debug/libcwd_output_sink.h"

#include <cstdlib>
#include <iostream>
#include <string>
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

// Install a process-lifetime private sink before libcwd initialization, then
// balance initialization when no requested test output file could be enabled.
// Ordinary, non-test processes follow debug_init(): libcwd stays off unless
// the operator opts in with AVA_DEBUG_OUTPUT=1.

//static
bool Application::prepare_debug()
{
  char const* test_name = std::getenv("AVA_TEST_NAME");
  if (test_name == nullptr || test_name[0] == '\0')
    return false;

  s_output_sink_ = std::make_unique<debug::LibcwdOutputSink>(test_name);
  if (!s_output_sink_->setup_succeeded())
    std::cerr << "failed to configure libcwd output: " << s_output_sink_->setup_error() << '\n';

  return s_output_sink_->enabled();
}
#endif

}  // namespace ava::app
