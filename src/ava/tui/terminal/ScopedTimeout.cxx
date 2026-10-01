#include "sys.h"
#include "ScopedTimeout.h"
#include "ava/core/Application.h"

namespace ava::tui::terminal {

ScopedTimeout::ScopedTimeout(std::chrono::milliseconds delay_ms)
{
  Context& terminal_context = core::Application::instance().terminal_context();
  terminal_context.stdscr().timeout(delay_ms.count());
}

ScopedTimeout::~ScopedTimeout()
{
  Context& terminal_context = core::Application::instance().terminal_context();
  terminal_context.stdscr().timeout(-1);
}

} // namespace ava::tui::terminal
