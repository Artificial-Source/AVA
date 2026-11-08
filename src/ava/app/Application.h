#pragma once

#include "command_line.h"
#include "ava/core/Application.h"
#include "ava/config/xdg_paths.h"
#include "ava/process/scope.h"

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
#ifdef CWDEBUG
 private:
  static std::unique_ptr<debug::LibcwdOutputSink> s_output_sink_;
  static std::unique_ptr<DebugTerminal> s_debug_terminal_;
  static bool prepare_debug();
#endif

 public:
  Application();
  ~Application();

  // Run the application. Called from `main`.
  int run(int argc, char** argv, ava::process::ProcessScopeV1 const& application_process_scope);

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

  // Can't print ava::core::Application.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  int dispatch_immediate(ImmediateInvocation const& command);
  int dispatch_connect(ConnectInvocation const& command, ava::config::XdgPaths const& paths, ava::process::ProcessScopeV1 const& application_process_scope);
  int dispatch_runtime(RuntimeInvocation command, ava::config::XdgPaths paths, char const* program, ava::process::ProcessScopeV1 const& application_process_scope);
};

}  // namespace ava::app
