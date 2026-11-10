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
#include <cstdio>
#include <cstdlib>
#include <functional>
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
  // Not copyable: one guard owns the single shutdown attempt for this run.
  RuntimeBeforeShutdownGuard(RuntimeBeforeShutdownGuard const&) = delete;
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
  // Not copyable: exactly one guard clears the overlays for this session scope.
  ComposerTerminalGraphicsGuard(ComposerTerminalGraphicsGuard const&) = delete;
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
    runtime_input::reset_input_decoder();
  }

  // Not copyable: one scope owns probe cleanup and queued startup input.
  TerminalBackgroundDetectionGuard(TerminalBackgroundDetectionGuard const&) = delete;
  TerminalBackgroundDetectionGuard& operator=(TerminalBackgroundDetectionGuard const&) = delete;

  // TUI thread: stop accepting probe replies before clearing queued startup input.
  ~TerminalBackgroundDetectionGuard()
  {
    disarm_terminal_background_response_handler();
    reset_detected_terminal_background_appearance();
    runtime_input::reset_input_decoder();
  }

  // This is an anonymous namespace - we can't generate print_members.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace

void TuiFrontEnd::set_tui_session(TuiSession* session)
{
  // For now all three handlers forward to the session.
  terminal_input_handler_.set_tui_session(session);
  notification_handler_.set_tui_session(session);
  timeout_handler_.set_tui_session(session);
}

// Enter the private composer implementation after the application has registered frontend data.
app::ExitCode TuiFrontEnd::run()
{
  DoutEntering(dc::notice, "TuiFrontEnd::run()");
  using EventLoop = utils::threading::EventLoop;
  EventLoop& event_loop = *event_loop_;

  try
  {
    // Install the application cleanup obligation before initialization can fail.
    // Inner session/graphics scopes retire first.
    RuntimeBeforeShutdownGuard before_shutdown(*this);

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

    // Activate signal handlers for the TUI (SIGINT is skipped when running inside gdb).
    signals_manager.activate_handlers({SIGINT, SIGTERM});           // Signals for which we want to call Signals::signal_handler.
    signals_manager.default_handlers({SIGHUP});

    // Kitty images that were placed by the composer stay visible in the terminal until they are explicitly deleted.
    // Delete them when leaving this scope, whether normally or by exception.
    // Construct this guard before the TuiSession below, so that the images are only deleted after the session that draws them is gone.
    ComposerTerminalGraphicsGuard graphics_cleanup(terminal_context);

    // Read the user's key bindings and display settings from their configuration files.
    // Failing to load either is not fatal: the defaults remain in effect and the error
    // message is collected in initial_status_line, which is passed to the TuiSession below.
    //
    // Do this first because applying the cursor settings and deciding whether to
    // probe the terminal background, below, both depend on the loaded display settings.
    std::string initial_status_line = load_key_bindings();
    load_display_settings(initial_status_line);
    terminal_context.apply_cursor_settings(cursor_settings());

    // If the theme is set to automatic, ask the terminal for its background color so that the theme can match it.
    // The query is not sent when running inside tmux.
    //
    // This happens only once, after the terminal was initialized and before anything is drawn.
    // There is no new query after a suspend/resume and a reply that arrives too late is ignored,
    // so that the automatically selected theme cannot change once it is on screen.
    TerminalBackgroundDetectionGuard terminal_background_detection;
    maybe_probe_terminal_background_appearance(terminal_context);

    // Register every event before the first wait: the terminal input fd, and one notification Event that all producers share for now.
    [[maybe_unused]] EventLoop::Event const input_event = event_loop.watch_fd(::fileno(terminal_context.input_stream()), EventLoop::PollEvents::read);
    EventLoop::Event const notify_event = event_loop.watch_event();

    Dout(dc::tuievents, "Terminal input event has index " << EventLoop::index(input_event));
    Dout(dc::tuievents, "Notify event has index " << EventLoop::index(notify_event));

    //FIXME: TuiWakeup and TuiSession is *old* code that needs to be replaced.
    TuiWakeup const wakeup(event_loop_, notify_event);
    TuiSession session(*this, terminal_context, initial_status_line, wakeup);
    set_tui_session(&session);
    session.start();

    // The only exit of this loop is a termination requested by the application (see terminate()).
    for (;;)
    {
      // Wait for the next Event.
      if (auto const deadline = timeout_handler_.next_deadline())
        event_loop.wait_until(*deadline);
      else
        event_loop.wait();

      // Get the WaitResult.
      EventLoop::WaitResult const& wait_result = event_loop.result();
      EventLoop::EventMask events = wait_result.events();
      if (events.any())
      {
        for (auto event : events)
        {
          EventLoop::Index index = EventLoop::index(event);
          //FooBar& foobar = my_array[index];
          if (event_loop.is_fd_event(event))
          {
            int const fd = wait_result.fd(index);
            EventLoop::ReadyEvents const ready_events = wait_result.ready_events(index);
            terminal_input_handler_.input_ready(fd, ready_events);
          }
          else
            notification_handler_.notified();
        }
      }
      else if (wait_result.status() == EventLoop::WaitStatus::terminated)
        break;

      // Not only after a timeout: a deadline can pass while other events are being handled.
      timeout_handler_.expire(TuiTimerRegistry::Clock::now());

      // FIXME: This is a hack to keep the testsuite working while functionality of the old TuiSession is still being used.
      session.flush();
    }

    return session.shutdown(std::bind_front(&RuntimeBeforeShutdownGuard::invoke, &before_shutdown));
  }
  catch (...)
  {
    // The main thread blocks in terminate() until this thread acknowledges the termination.
    // Hence a TUI thread that leaves early must ask for the shutdown and keep waiting until then.
    core::Application::instance().request_shutdown();
    do
      event_loop.wait();
    while (event_loop.result().status() != EventLoop::WaitStatus::terminated);
    throw;
  }
}

void TuiFrontEnd::terminate()
{
  DoutEntering(dc::notice, "TuiFrontEnd::terminate()");
  event_loop_->terminate();
}

// TUI thread between dispatch steps: replace the map in place so existing const
// borrows stay valid. Presentation refreshes the associated hints after this call.
void TuiFrontEnd::apply_key_bindings(TuiKeyBindings bindings)
{
  key_bindings_ = std::move(bindings);
}

std::array<std::string, 3> TuiFrontEnd::read_settings_model_labels()
{
  // These refreshes consume change flags. Read them together only when the Model
  // section is rebuilt, and copy the labels before returning to its row builder.
  auto const provider = model_selection().provider_refresh();
  auto const model = model_selection().model_refresh();
  auto const reasoning = model_selection().reasoning_refresh();
  return {provider.value, model.value, reasoning.value};
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
