#pragma once

#include "ava/core/Application.h"

#include <string_view>
#include "debug.h"
#ifdef CWDEBUG
#include <memory>
#endif

namespace ava::debug {
// Forward declaration.
class LibcwdOutputSink;
} // namespace ava::debug

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

  [[nodiscard]] std::string_view application_name() const noexcept override { return "AVA"; }

  // Can't print ava::core::Application.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::app
