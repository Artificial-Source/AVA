#include "sys.h"
#include "Application.h"
#include "acp_mode.h"
#include "app.h"
#include "command_line.h"
#include "connect_openai.h"
#include "doctor_support.h"
#include "interactive.h"
#include "print_mode.h"
#include "rpc_mode.h"
#include "runtime.h"
#include "runtime/Session.h"
#include "signal_policy.h"
#include "ExitCode.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/frontend/FrontEnd.h"
#include "ava/config/xdg_paths.h"
#include "ava/provider/catalog.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/version.h"
#include "utils/AIAlert.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <unistd.h>

#ifdef CWDEBUG
#include "DebugTerminal.h"
#include "ava/debug/libcwd_output_sink.h"

#include <string>
#include "debug.h"
#endif

namespace ava::app {
namespace {

namespace version = ava::core::version;

void print_help()
{
  std::cout << "AVA " << version::kDisplayVersion << "\n\n";
  std::cout << "Usage:\n";
  std::cout << "  ava [--help]\n";
  std::cout << "  ava [prompt]\n";
  std::cout << "  ava @file [prompt]\n";
  std::cout << "  ava login [provider] [--api-key|--browser-oauth|--headless-oauth]\n";
  std::cout << "  ava auth login [provider] [--api-key|--browser-oauth|--headless-oauth]\n";
  std::cout << "  ava connect [provider] [--api-key|--browser-oauth|--headless-oauth]\n";
  std::cout << "  ava connect <provider> --api-key-stdin|--api-key-env <env>\n";
  std::cout << "  ava doctor [--json]\n";
  std::cout << "  ava support export\n";
  std::cout << "  ava packages <list|install|remove|update|config>  # deferred\n";
  std::cout << "  ava --version\n";
  std::cout << "  ava --mode build|plan|text|json|rpc\n";
  std::cout << "  ava --thinking off|<reasoning-level>\n";
  std::cout << "  ava --session <id> | --session-id <id>\n";
  std::cout << "  ava --continue | --resume | -r\n";
  std::cout << "  ava --fork <id>\n";
  std::cout << "  ava --name <name>\n";
  std::cout << "  ava --session-dir <dir>\n";
  std::cout << "  ava --cwd <path>\n";
  std::cout << "  ava --no-session\n";
  std::cout << "  ava --offline\n";
  std::cout << "  ava --trace  # write a private bounded production trace\n";
  std::cout << "  ava [--agent name] [--system-prompt text] [--append-system-prompt text]\n";
  std::cout << "  ava [--tools list] [--exclude-tools list] [--no-builtin-tools|--no-tools]\n";
  std::cout << "  ava --print [@file ...] [prompt] [--json|--output json] [--allow read-only] [--allow-tool list]\n";
  std::cout << "  ava -p [@file ...] [prompt] [--json|--output json] [--allow read-only] [--allow-tool list]\n";
  std::cout << "  ava --rpc [--allow read-only] [--allow-tool list]\n";
  std::cout << "  ava --output rpc [--allow read-only] [--allow-tool list]\n";
  std::cout << "  ava --print|--rpc [--parallel-read-search-tools] [--parallel-read-search-max-workers 1..4]\n";
  std::cout << "  ava [--trace] --acp  # ACP v1 transport with the implemented AVA session/tool profile\n\n";
  std::cout << version::kDisplayVersion << " status: backend MVP runtime with terminal, print, RPC, and ACP v1 workflows.\n";
}

bool logical_path_is_within(std::filesystem::path const& path, std::filesystem::path const& root)
{
  auto const path_text = path.lexically_normal().string();
  auto const root_text = root.lexically_normal().string();
  if (path_text == root_text)
    return true;
  if (root_text == "/")
    return path_text.starts_with('/');
  return path_text.size() > root_text.size() && path_text.starts_with(root_text) && path_text[root_text.size()] == '/';
}

std::string_view exit_status_text(ExitCode status, bool sessionless)
{
  if (status == ExitCode::success)
    return sessionless ? "session discarded" : "session saved";
  if (status == ExitCode::interrupted)
    return sessionless ? "interrupted, session discarded" : "interrupted, session saved";
  return sessionless ? "session discarded with warnings" : "session saved with warnings";
}

void print_exit_card(ava::app::runtime::session_ts const& unlocked_session, ExitCode status)
{
  bool const use_color = stdout_is_tty() && std::getenv("NO_COLOR") == nullptr;
  auto const blue = use_color ? std::string_view("\x1b[38;2;77;158;246m") : std::string_view("");
  auto const muted = use_color ? std::string_view("\x1b[38;2;148;163;184m") : std::string_view("");
  auto const bold = use_color ? std::string_view("\x1b[1m") : std::string_view("");
  auto const reset = use_color ? std::string_view("\x1b[0m") : std::string_view("");
  auto art = [&](std::string_view text) { std::cout << blue << text << reset << '\n'; };
  if (stdout_is_tty())
    std::cout << "\x1b(B\x1b[0m";
  std::cout << '\n';
  art("  █████████   █████   █████   █████████");
  art("  ███░░░░░███ ░░███   ░░███   ███░░░░░███");
  art(" ░███    ░███  ░███    ░███  ░███    ░███");
  art(" ░███████████  ░███    ░███  ░███████████");
  art(" ░███░░░░░███  ░░███   ███   ░███░░░░░███");
  art(" ░███    ░███   ░░░█████░    ░███    ░███");
  art(" █████   █████    ░░███      █████   █████");
  art("░░░░░   ░░░░░      ░░░      ░░░░░   ░░░░░");
  std::cout << '\n';
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
  std::cout << bold << "AVA " << reset << exit_status_text(status, session_r->sessionless()) << ". " << muted << "Ready when you are." << reset << '\n';
  if (session_r->sessionless())
  {
    std::cout << muted << "History: " << reset << "not saved (--no-session)\n";
    return;
  }
  std::cout << muted << "Resume: " << reset << "ava --session " << session_r->store.options_r()->session_id << '\n';
  std::cout << muted << "Saved:  " << reset << session_r->store.session_path().string() << '\n';
}

} // namespace

void Application::dispatch_immediate(ImmediateInvocation const& command)
{
  switch (command.kind)
  {
    case ImmediateOutputKind::Help:
      print_help();
      break;
    case ImmediateOutputKind::Version:
      std::cout << "ava " << version::kFullVersion << '\n';
      break;
    case ImmediateOutputKind::PackageDeferred:
      std::cout << "AVA package manager is deferred pending local-source, provenance, trust, rollback, and compatibility policy.\n"
                   "Install resources manually under $XDG_CONFIG_HOME/ava or trusted project .ava directories; see docs/core/configuration.md and "
                   "docs/extensions/plugin-system.md.\n";
      break;
    case ImmediateOutputKind::Text:
      if (command.status != ExitCode::success)
        THROW_EXIT_CODE_V(command.status, "[ERRMSG]", AIArgs("[ERRMSG]", command.text));
      std::cout << command.text;
      break;
  }
}

void Application::dispatch_connect(ConnectInvocation const& command, ava::config::XdgPaths const& paths)
{
  if (command.source == ConnectCredentialSource::BrowserOAuth)
    run_connect_openai_browser(paths, application_process_scope_);
  else if (command.source == ConnectCredentialSource::HeadlessOAuth)
    run_connect_openai_headless(paths, application_process_scope_);
  else if (command.source == ConnectCredentialSource::Stdin || command.source == ConnectCredentialSource::Env)
    run_connect_provider_credential(
        paths, ConnectProviderCredentialOptions{.provider_id = *command.provider, .credential_type = *command.credential_type, .env_var = command.env_var});
  else if (command.source == ConnectCredentialSource::Prompt)
    run_connect_provider_wizard(
        paths, ConnectProviderWizardOptions{.provider_id = command.provider, .credential_type = command.credential_type, .stdin_is_tty = stdin_is_tty()},
        application_process_scope_);
  else if (command.provider && *command.provider == "openai")
    run_connect_openai_wizard(paths, ConnectProviderWizardOptions{.provider_id = command.provider, .stdin_is_tty = stdin_is_tty()}, application_process_scope_);
  else
    run_connect_provider_wizard(paths, ConnectProviderWizardOptions{.provider_id = command.provider, .stdin_is_tty = stdin_is_tty()},
                                application_process_scope_);
}

void Application::dispatch_runtime(RuntimeInvocation command, ava::config::XdgPaths paths)
{
  if (command.session_dir)
  {
    std::error_code error;
    auto resolved = std::filesystem::absolute(*command.session_dir, error);
    if (error)
      THROW_EXIT_CODE(usage, "failed to resolve --session-dir: [ERRMSG]", AIArgs("[ERRMSG]", error.message()));
    paths.sessions_dir = resolved.lexically_normal();
  }

  runtime::OpenContext open_context;
  open_context.application_process_scope = application_process_scope_;
  auto cwd = ava::core::launch_workspace_root();
  if (!cwd)
    THROW_EXIT_CODE(environment_unavailable, "[ERRMSG]", AIArgs("[ERRMSG]", cwd.error().format()));
  open_context.workspace_dir = std::move(*cwd);
  open_context.current_dir = open_context.workspace_dir;
  if (command.requested_current_dir)
  {
    auto const candidate = command.requested_current_dir->is_absolute() ? command.requested_current_dir->lexically_normal()
                                                                        : (open_context.workspace_dir / *command.requested_current_dir).lexically_normal();
    if (!logical_path_is_within(candidate, open_context.workspace_dir))
      THROW_EXIT_CODE(usage, "--cwd must be equal to or inside the workspace directory");
    open_context.current_dir = candidate;
  }
  auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(paths, command.trace_requested);
  if (!diagnostics)
    THROW_EXIT_CODE(environment_unavailable,
                 command.trace_requested ? "trace startup failed: private diagnostics storage is unavailable." : "runtime diagnostics startup failed.");

  open_context.mode = command.mode;
  open_context.tool_visibility = std::move(command.tool_visibility);
  open_context.requested_primary_agent = std::move(command.requested_primary_agent);
  open_context.paths = paths;
  open_context.prompt_overrides = std::move(command.prompt_overrides);
  open_context.offline = command.offline;
  open_context.diagnostics = *diagnostics;
  auto provider_catalog = ava::provider::ProviderCatalog::build(paths);
  if (!provider_catalog)
    THROW_EXIT_CODE(configuration_invalid, "[ERRMSG]", AIArgs("[ERRMSG]", provider_catalog.error().format()));
  open_context.provider_catalog = std::move(*provider_catalog);
  runtime::SessionLifecycleRequest lifecycle;
  lifecycle.sessionless = command.sessionless;
  lifecycle.requested_session_id = std::move(command.requested_session_id);
  lifecycle.fork_session_id = std::move(command.fork_session_id);
  lifecycle.initial_session_name = std::move(command.initial_session_name);
  lifecycle.continue_last_session = command.continue_last_session;
  lifecycle.initial_reasoning_level = std::move(command.initial_reasoning_level);

  if (command.frontend == RuntimeFrontend::Print)
  {
    run_print_mode(PrintModeOptions{.open_context = open_context,
                                    .lifecycle_request = lifecycle,
                                    .explicit_prompt = command.print_prompt,
                                    .read_stdin = !stdin_is_tty(),
                                    .output_format = command.print_output_format,
                                    .permission_policy = std::move(command.headless_permission_policy),
                                    .parallel_read_search_tools = command.parallel_read_search_tools,
                                    .parallel_read_search_max_workers = command.parallel_read_search_max_workers,
                                    .provider_override = std::nullopt,
                                    .transport_override = std::nullopt},
                   std::cin, std::cout, std::cerr);
    return;
  }
  if (command.frontend == RuntimeFrontend::Rpc)
  {
    run_rpc_mode(RpcModeOptions{.open_context = open_context,
                                .lifecycle_request = lifecycle,
                                .permission_policy = std::move(command.headless_permission_policy),
                                .parallel_read_search_tools = command.parallel_read_search_tools,
                                .parallel_read_search_max_workers = command.parallel_read_search_max_workers},
                 std::cin, std::cout, rpc::RpcInputWake{});
    return;
  }

  auto session = runtime::Session::open(open_context, lifecycle);
  if (!session)
    THROW_EXIT_CODE(session_unavailable, "[ERRMSG]", AIArgs("[ERRMSG]", session.error().format()));
  bool const farewell = stdin_is_tty() && stdout_is_tty();
  exit_code_ = run_interactive(*session);
  if (farewell)
    print_exit_card(*session, exit_code_);
}

// Construct the AVA process Application after preparing its optional private
// test sink. The core base initializes libcwd before constructing allocators;
// the constructor body then records the routing marker through libcwd itself.
Application::Application() : core::Application(CWDEBUG_ONLY(prepare_debug())), exit_code_(ExitCode::success)
{
#ifdef CWDEBUG
  char const* test_name = std::getenv("AVA_TEST_NAME");
  if (test_name != nullptr && test_name[0] != '\0')
    Dout(dc::notice, "AVA libcwd routing marker: test=" << test_name);
#endif
  supervisor_ = std::make_shared<ava::process::Supervisor>();
  auto generated_scope = ava::process::ProcessScopeV1::application(supervisor_);
  if (!generated_scope)
  {
    THROW_EXIT_CODE(environment_unavailable, "AVA process supervision startup failed: [ERRMSG]",
        AIArgs("[ERRMSG]", generated_scope.error().format()));
  }
  application_process_scope_ = std::move(*generated_scope);
}

Application::~Application()
{
  supervisor_->stop_accepting();
  auto const cleanup = supervisor_->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(2));
  if (exit_code_ == ExitCode::success && !cleanup.complete)
    exit_code_ = ExitCode::cleanup_incomplete;
}

void Application::run(int argc, char** argv)
{
  CommandLineInvocation invocation = parse_command_line(argc, argv);
  apply_invocation_signal_policy(invocation_mode(invocation));

  if (auto const* immediate = std::get_if<ImmediateInvocation>(&invocation))
  {
    dispatch_immediate(*immediate);
    return;
  }

  if (auto const* acp = std::get_if<AcpInvocation>(&invocation))
  {
    auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(ava::config::xdg_paths(), acp->trace);
    if (!diagnostics)
      THROW_EXIT_CODE(environment_unavailable, "trace startup failed: private diagnostics storage is unavailable");
    exit_code_ = run_acp_mode(std::cerr, *application_process_scope_, std::move(*diagnostics));
    return;
  }

  if (auto const* doctor = std::get_if<DoctorInvocation>(&invocation))
  {
    std::error_code error;
    auto workspace = std::filesystem::current_path(error);
    if (error)
      workspace.clear();
    exit_code_ = run_doctor(ava::config::xdg_paths(), workspace, doctor->json, std::cout, std::cerr);
    return;
  }

  if (std::holds_alternative<SupportExportInvocation>(invocation))
  {
    std::error_code error;
    auto workspace = std::filesystem::current_path(error);
    if (error)
      workspace.clear();
    exit_code_ = run_support_export(ava::config::xdg_paths(), workspace, std::cout, std::cerr);
    return;
  }

  if (auto const* connect = std::get_if<ConnectInvocation>(&invocation))
  {
    dispatch_connect(*connect, ava::config::xdg_paths());
    return;
  }

  dispatch_runtime(std::get<RuntimeInvocation>(std::move(invocation)), ava::config::xdg_paths());
}

void Application::wait_for_shutdown(frontend::FrontEnd& frontend)
{
  wait_for_shutdown_request();
  // Stop the frontend thread first: upon return from `terminate` the frontend thread is guaranteed
  // to not be in the middle of a callback: it is inside EventLoop::wait_impl that will return, or
  // already returned WaitStatus::terminated.
  frontend.terminate();
}

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

//============================================================================
// Error code handling.
//

namespace {

struct ApplicationErrorCategory : std::error_category
{
  char const* name() const noexcept override;
  std::string message(int ev) const override;
};

char const* ApplicationErrorCategory::name() const noexcept
{
  // This error category means that its value is in fact an exit code for main.
  return "ava::app::Application";
}

std::string ApplicationErrorCategory::message(int) const
{
  return "ApplicationErrorCategory is not a real error; it is the exit code for main.";
}

ApplicationErrorCategory const theErrorCategory { };

} // namespace

std::error_code make_error_code(ExitCode status)
{
  return std::error_code(std::to_underlying(status), theErrorCategory);
}

}  // namespace ava::app
