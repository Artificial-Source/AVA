// Exercise Mermaid bridge attachment contracts through real TUI sessions.
// The supervised child first accepts an empty bridge, then intentionally aborts.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/TuiSession.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/core/Application.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>

namespace {

// Flush fixed progress markers before the expected assertion terminates the child.
void marker(char const* value)
{
  if (std::fputs(value, stderr) == EOF || std::fflush(stderr) != 0)
    throw std::system_error(errno, std::generic_category(), "write Mermaid contract marker");
}

// Prevent this intentional abort from creating a core or invoking a core handler.
void disable_core_dumps()
{
  rlimit const no_core{.rlim_cur = 0, .rlim_max = 0};
  if (::setrlimit(RLIMIT_CORE, &no_core) != 0)
    throw std::system_error(errno, std::generic_category(), "disable Mermaid contract core dump");
#ifdef __linux__
  if (::prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0)
    throw std::system_error(errno, std::generic_category(), "disable Mermaid contract core handler");
  int const dumpable = ::prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L);
  if (dumpable < 0)
    throw std::system_error(errno, std::generic_category(), "check Mermaid contract core handling");
  if (dumpable != 0)
    throw std::runtime_error("Mermaid contract child still permits core handling");
#endif
}

// Test-thread guard owns a pipe and borrows the Application stream. Both Sessions
// die first; the guard restores the original descriptor before the Context retires.
class InputPipe final
{
 public:
  // Save the Context descriptor and install an empty pipe with its writer held open.
  InputPipe() : stream_(ava::core::Application::instance().terminal_context().input_stream()), saved_(::dup(::fileno(stream_)))
  {
    if (!saved_)
      throw std::system_error(errno, std::generic_category(), "save Mermaid contract input");
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) != 0)
      throw std::system_error(errno, std::generic_category(), "create Mermaid contract input pipe");
    ava::core::UniqueFd reader(descriptors[0]);
    peer_.reset(descriptors[1]);
    if (::dup2(reader.get(), ::fileno(stream_)) < 0)
      throw std::system_error(errno, std::generic_category(), "replace Mermaid contract input");
    std::clearerr(stream_);
    ava::tui::runtime_input::reset_input_decoder();
  }

  // Restore the borrowed stream after both sessions retire on an unexpected return.
  ~InputPipe()
  {
    ava::tui::runtime_input::clear_startup_input_queue();
    ava::tui::runtime_input::reset_input_decoder();
    expect(::dup2(saved_.get(), ::fileno(stream_)) >= 0, "restore Mermaid contract input");
    std::clearerr(stream_);
  }

  // Descriptor restoration has exactly one owner.
  InputPipe(InputPipe const&) = delete;
  InputPipe& operator=(InputPipe const&) = delete;

  // This internal guard has no generated printer and borrows its Context stream.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Application owns this stream and outlives both sessions and this guard.
  FILE* stream_;
  // The original endpoint remains owned until restoration completes.
  ava::core::UniqueFd saved_;
  // Holding the write endpoint prevents an unrelated input hangup.
  ava::core::UniqueFd peer_;
};

} // namespace

// Run only through the subprocess supervisor after the common terminal setup.
void run_tui_mermaid_missing_notifier_test()
{
  disable_core_dumps();
  InputPipe input;
  tui_test_support::TuiFrontEndFixture frontend;
  frontend.hooks().mermaid_bridge = [] { return ava::tui::TuiMermaidRenderBridge{}; };
  {
    ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
  }
  marker("R11_EMPTY_BRIDGE_OK\n");

  // Even a disabled bridge with one callback must supply completion notification.
  frontend.hooks().mermaid_bridge = [] {
    ava::tui::TuiMermaidRenderBridge bridge;
    bridge.drain = []() -> std::optional<std::vector<ava::tui::TuiMermaidRenderCompletion>> { return std::vector<ava::tui::TuiMermaidRenderCompletion>{}; };
    return bridge;
  };
  marker("R11_MISSING_NOTIFIER_ATTACH\n");
  ava::tui::TuiSession session(*frontend.frontend(), ava::core::Application::instance().terminal_context(), "");
  marker("R11_MISSING_NOTIFIER_RETURNED\n");
  expect(false, "attaching a nonempty Mermaid bridge without a completion notifier must assert");
}
