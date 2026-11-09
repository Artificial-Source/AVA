#pragma once

#include "ExitCode.h"
#include "command_line.h"
#include "ava/core/Application.h"
#include "ava/config/xdg_paths.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"

#include <optional>
#include <memory>
#include <string_view>
#include "debug.h"
#ifdef CWDEBUG
#include <memory>
#endif

namespace ava::debug {
// Forward declaration.
class LibcwdOutputSink;
} // namespace ava::debug

namespace ava::frontend {
// Forward declaration.
class FrontEnd;
} // namespace ava::frontend

namespace ava::app {
#ifdef CWDEBUG
// Forward declaration.
class DebugTerminal;
#endif

class Application final : public core::Application
{
 private:
  ExitCode exit_code_;
  std::shared_ptr<ava::process::Supervisor> supervisor_;
  std::optional<ava::process::ProcessScopeV1> application_process_scope_;
#ifdef CWDEBUG
  static std::unique_ptr<debug::LibcwdOutputSink> s_output_sink_;
  static std::unique_ptr<DebugTerminal> s_debug_terminal_;
  static bool prepare_debug();
#endif

 public:
  Application();
  ~Application();

  // Run the application. Called from `main`, which returns the result.
  void run(int argc, char** argv);

  // Return the one AVA process Application.
  //
  // Hides core::Application::instance() so that code in ava::app reaches the members below.
  static Application& instance() { return static_cast<Application&>(core::Application::instance()); }

  [[nodiscard]] std::string_view application_name() const noexcept override { return "AVA"; }

  // Main thread: sleep until request_shutdown() is called, then terminate frontend.
  //
  // Returns once frontend.terminate() returned, which means that the TUI thread left its event loop
  // and no longer dispatches events; the caller must still join that thread.
  void wait_for_shutdown(frontend::FrontEnd& frontend);

  // Get the final exit code for main.
  ExitCode exit_code() const { return exit_code_; }

  // Can't print ava::core::Application.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void dispatch_immediate(ImmediateInvocation const& command);
  // Run the `ava connect` invocation command, storing the credential below paths.
  //
  // Throws an ava::app::AIAlert::ExitCode when no credential could be stored.
  void dispatch_connect(ConnectInvocation const& command, ava::config::XdgPaths const& paths);
  void dispatch_runtime(RuntimeInvocation command, ava::config::XdgPaths paths);
};

}  // namespace ava::app
