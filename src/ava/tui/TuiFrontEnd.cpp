#include "sys.h"
#include "TuiFrontEnd.h"
#include "TuiSession.h"
#include "command_output.h"
#include "composer_internal.h"
#include "prompt_stash_internal.h"
#include "runtime_actions_internal.h"
#include "runtime_active_run_internal.h"
#include "runtime_draft_internal.h"
#include "runtime_input_internal.h"
#include "runtime_internal.h"
#include "runtime_mermaid_internal.h"
#include "runtime_navigation_internal.h"
#include "runtime_plugin_ui_internal.h"
#include "runtime_prompts_internal.h"
#include "runtime_render_internal.h"
#include "runtime_state_internal.h"
#include "runtime_subagent_workspace_internal.h"
#include "runtime_submit_internal.h"
#include "runtime_transcript_search_internal.h"
#include "runtime_user_turn_selection_internal.h"
#include "runtime_views_internal.h"

#include <utility>

namespace ava::tui {

namespace {

constexpr auto kTerminalBackgroundProbeDeadline = std::chrono::milliseconds(50);

std::optional<std::string_view> optional_environment_view(char const* name)
{
  auto const* value = std::getenv(name);
  if (value == nullptr)
    return std::nullopt;
  return std::string_view(value);
}

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

class RuntimeBeforeShutdownGuard
{
 public:
  explicit RuntimeBeforeShutdownGuard(TuiFrontEnd& frontend) : frontend_(frontend) { }
  RuntimeBeforeShutdownGuard(RuntimeBeforeShutdownGuard const&) = delete;
  RuntimeBeforeShutdownGuard& operator=(RuntimeBeforeShutdownGuard const&) = delete;
  ~RuntimeBeforeShutdownGuard() { static_cast<void>(invoke()); }

  [[nodiscard]] bool invoke() noexcept
  {
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
  TuiFrontEnd& frontend_;
  bool invoked_ = false;
};

class ComposerTerminalGraphicsGuard
{
 private:
  terminal::Context& terminal_context_;

 public:
  ComposerTerminalGraphicsGuard(terminal::Context& terminal_context) : terminal_context_(terminal_context) { }
  ComposerTerminalGraphicsGuard(ComposerTerminalGraphicsGuard const&) = delete;
  ComposerTerminalGraphicsGuard& operator=(ComposerTerminalGraphicsGuard const&) = delete;
  ~ComposerTerminalGraphicsGuard() { detail::clear_composer_terminal_graphics(terminal_context_); }

  // This is an anonymous namespace - we can't generate print_members.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Session-scoped OSC 11 detection and startup-input FIFO ownership. Reset before
// each interactive TUI session and again on every exit so sequential sessions and
// tests never inherit a prior probe result or queued startup events.
class TerminalBackgroundDetectionGuard
{
 public:
  TerminalBackgroundDetectionGuard()
  {
    disarm_terminal_background_response_handler();
    reset_detected_terminal_background_appearance();
    runtime_input::clear_startup_input_queue();
  }

  TerminalBackgroundDetectionGuard(TerminalBackgroundDetectionGuard const&) = delete;
  TerminalBackgroundDetectionGuard& operator=(TerminalBackgroundDetectionGuard const&) = delete;

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
