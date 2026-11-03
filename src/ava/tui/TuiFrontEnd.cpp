// Enters the terminal session and retires application work on every exit path.
#include "sys.h"
#include "TuiFrontEnd.h"
#include "TuiSession.h"
#include "composer_internal.h"
#include "runtime_input_internal.h"
#include "terminal.h"
#include "terminal/Context.h"
#include "theme.h"
#include "ava/core/Application.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include "debug.h"

namespace ava::tui {

namespace {

// Startup probe budget; per-read terminal decoding can overrun this outer deadline.
constexpr auto kTerminalBackgroundProbeDeadline = std::chrono::milliseconds(50);

// TUI startup only: borrow an environment value through the immediate probe
// decision; no caller may retain it across environment mutation.
std::optional<std::string_view> optional_environment_view(char const* name)
{
  auto const* value = std::getenv(name);
  if (value == nullptr)
    return std::nullopt;
  return std::string_view(value);
}

// TUI startup before first paint: probe only eligible terminals and replay
// unrelated input afterward. Late replies must not change an already shown theme.
void maybe_probe_terminal_background_appearance(terminal::Context& terminal_context)
{
  if (!tui_theme_needs_terminal_background_probe())
    return;

  auto const tmux = optional_environment_view("TMUX");
  auto const term = optional_environment_view("TERM");
  if (!terminal_background_probe_environment_allows_query(tmux, term))
    return;

  arm_terminal_background_response_handler();
  if (terminal_context.query_background_color())
    runtime_input::drain_startup_probe_input(kTerminalBackgroundProbeDeadline);
  disarm_terminal_background_response_handler();
}

// Stack-owned by TuiFrontEnd::run on the TUI thread. Both normal session exit
// and early unwinding use this guard, but application shutdown runs at most once.
// The frontend outlives the guard and all work joined by its hook.
class RuntimeBeforeShutdownGuard
{
 public:
  // TUI thread: borrow the frontend before any startup operation can fail.
  explicit RuntimeBeforeShutdownGuard(TuiFrontEnd& frontend) : frontend_(frontend) { }
  // One guard owns the single shutdown attempt for this run.
  RuntimeBeforeShutdownGuard(RuntimeBeforeShutdownGuard const&) = delete;
  // Reassignment could lose the pending cleanup obligation.
  RuntimeBeforeShutdownGuard& operator=(RuntimeBeforeShutdownGuard const&) = delete;
  // TUI thread: cover every early exit; invoke suppresses repeated attempts.
  ~RuntimeBeforeShutdownGuard() { static_cast<void>(invoke()); }

  // TUI thread: mark attempted before entering application code so exceptions
  // or reentry cannot trigger a second shutdown. False reports a thrown hook.
  [[nodiscard]] bool invoke() noexcept
  {
    // Set the latch before calling out: even a throwing hook has been attempted.
    if (invoked_)
      return true;
    invoked_ = true;
    try
    {
      frontend_.on_before_tui_shutdown();
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  // This is an anonymous namespace - we can't generate print_members.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  TuiFrontEnd& frontend_; // Borrowed application boundary; the run scope keeps it alive.
  bool invoked_ = false; // TUI-thread attempt latch, set before calling potentially throwing application code.
};

// Stack-owned TUI cleanup for terminal image overlays. The initialized Context
// outlives this guard; session destruction precedes graphics cleanup.
class ComposerTerminalGraphicsGuard
{
 private:
  terminal::Context& terminal_context_; // TUI-thread borrowed terminal used only during final overlay cleanup.

 public:
  // TUI thread: acquire cleanup responsibility after Context initialization.
  ComposerTerminalGraphicsGuard(terminal::Context& terminal_context) : terminal_context_(terminal_context) { }
  // Exactly one guard clears the overlays for this session scope.
  ComposerTerminalGraphicsGuard(ComposerTerminalGraphicsGuard const&) = delete;
  // Preserve the original terminal and cleanup responsibility until scope exit.
  ComposerTerminalGraphicsGuard& operator=(ComposerTerminalGraphicsGuard const&) = delete;
  // TUI thread after session retirement: erase overlays while Context still lives.
  ~ComposerTerminalGraphicsGuard() { detail::clear_composer_terminal_graphics(terminal_context_); }

  // This is an anonymous namespace - we can't generate print_members.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Stack-owned TUI-thread OSC 11 detection and startup-input FIFO cleanup. Reset before
// each interactive TUI session and again on every exit so sequential sessions and
// tests never inherit a prior probe result or queued startup events.
class TerminalBackgroundDetectionGuard
{
 public:
  // TUI thread: discard prior-session probe state before accepting startup replies.
  TerminalBackgroundDetectionGuard()
  {
    disarm_terminal_background_response_handler();
    reset_detected_terminal_background_appearance();
    runtime_input::clear_startup_input_queue();
  }

  // Probe admission and startup input have one scope owner, not copyable snapshots.
  TerminalBackgroundDetectionGuard(TerminalBackgroundDetectionGuard const&) = delete;
  // Reassignment would confuse which scope retires the global probe handler.
  TerminalBackgroundDetectionGuard& operator=(TerminalBackgroundDetectionGuard const&) = delete;

  // TUI thread: close probe admission before clearing queued startup input.
  ~TerminalBackgroundDetectionGuard()
  {
    disarm_terminal_background_response_handler();
    reset_detected_terminal_background_appearance();
    runtime_input::clear_startup_input_queue();
  }

  // This is an anonymous namespace - we can't generate print_members.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace

// Enter the private composer implementation after the application has registered frontend data.
int TuiFrontEnd::run()
{
  DoutEntering(dc::notice, "TuiFrontEnd::run()");

  // Install the application cleanup obligation before terminal validation or
  // initialization can fail. Inner session/graphics scopes retire first.
  RuntimeBeforeShutdownGuard before_shutdown(*this);

  if (!terminal_is_tty())
  {
    std::cerr << "interactive TUI requires stdin and stdout to be terminals\n";
    return 1;
  }

  core::Application& application = core::Application::instance();
  core::Signals& signals_manager = application.signals_manager();

#ifdef CWDEBUG
  // Uncomment this to open a new terminal with gdb when the TUI thread is started.
  // application.attach_gdb();
#endif

  // Restore the default disposition so ncurses can install its SIGTSTP handler.
  // This also unblocks SIGTSTP on this thread; other threads keep it blocked.
  signals_manager.default_handlers({SIGTSTP});
  application.initialize_terminal_context();
  terminal::Context& terminal_context = application.terminal_context();

  // Activate signal handlers for the TUI.
#ifdef CWDEBUG
  // If we're running inside gdb, do not install a handler for SIGINT.
  if (NAMESPACE_DEBUG::being_traced())
    signals_manager.activate_handlers({SIGTERM});
  else
#endif
    signals_manager.activate_handlers({SIGINT, SIGTERM});         // Signals for which we want to call Signals::signal_handler.
  signals_manager.default_handlers({SIGHUP});

  ComposerTerminalGraphicsGuard graphics_cleanup(terminal_context);
  // Load effective presentation before applying the cursor or probing the theme.
  // Otherwise startup uses defaults until a later settings change or repaint.
  std::string initial_status_line = load_key_bindings();
  load_display_settings(initial_status_line);
  terminal_context.apply_cursor_settings(cursor_settings());
  // Probe the direct terminal background once after enter and before first paint.
  // No re-probe on suspend/resume and no late theme flip after presentation starts.
  TerminalBackgroundDetectionGuard terminal_background_detection;
  maybe_probe_terminal_background_appearance(terminal_context);
  TuiSession session(*this, terminal_context, initial_status_line);
  return session.run(std::bind_front(&RuntimeBeforeShutdownGuard::invoke, &before_shutdown));
}

// Load startup bindings into TUI-owned state and return only formatted failure status.
std::string TuiFrontEnd::load_key_bindings()
{
  std::string keybind_status;
  if (auto loaded = ava::tui::load_key_bindings(keybinds_json_path()); loaded)
    key_bindings_ = std::move(*loaded);
  else
    keybind_status = loaded.error().format();
  return keybind_status;
}

} // namespace ava::tui
