#include "sys.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/app/acp_mode.h"
#include "ava/app/app.h"
#include "ava/app/command_line.h"
#include "ava/app/connect_openai.h"
#include "ava/app/doctor_support.h"
#include "ava/app/interactive.h"
#include "ava/app/print_mode.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/signal_policy.h"
#include "ava/config/xdg_paths.h"
#include "ava/provider/catalog.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/version.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <unistd.h>

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

std::string_view exit_status_text(int status, bool sessionless)
{
  if (status == 0)
    return sessionless ? "session discarded" : "session saved";
  if (status == 130)
    return sessionless ? "interrupted, session discarded" : "interrupted, session saved";
  return sessionless ? "session discarded with warnings" : "session saved with warnings";
}

void print_exit_card(ava::app::runtime::session_ts const& unlocked_session, int status)
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
  std::cout << muted << "Resume: " << reset << "ava --session " << session_r->store.session_id() << '\n';
  std::cout << muted << "Saved:  " << reset << session_r->store.session_path().string() << '\n';
}

int dispatch_immediate(ImmediateInvocation const& command)
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
      (command.stderr_output ? std::cerr : std::cout) << command.text;
      break;
  }
  return command.status;
}

int dispatch_connect(ConnectInvocation const& command, ava::config::XdgPaths const& paths, ava::process::ProcessScopeV1 const& application_process_scope)
{
  if (command.source == ConnectCredentialSource::BrowserOAuth)
    return run_connect_openai_browser(paths, std::cout, std::cerr, application_process_scope);
  if (command.source == ConnectCredentialSource::HeadlessOAuth)
    return run_connect_openai_headless(paths, std::cout, std::cerr, application_process_scope);
  if (command.source == ConnectCredentialSource::Stdin || command.source == ConnectCredentialSource::Env)
    return run_connect_provider_credential(
        paths, ConnectProviderCredentialOptions{.provider_id = *command.provider, .credential_type = *command.credential_type, .env_var = command.env_var},
        std::cin, std::cout, std::cerr);
  if (command.source == ConnectCredentialSource::Prompt)
    return run_connect_provider_wizard(
        paths, ConnectProviderWizardOptions{.provider_id = command.provider, .credential_type = command.credential_type, .stdin_is_tty = stdin_is_tty()},
        std::cin, std::cout, std::cerr, application_process_scope);
  if (command.provider && *command.provider == "openai")
    return run_connect_openai_wizard(paths, ConnectProviderWizardOptions{.provider_id = command.provider, .stdin_is_tty = stdin_is_tty()}, std::cin, std::cout,
                                     std::cerr, application_process_scope);
  return run_connect_provider_wizard(paths, ConnectProviderWizardOptions{.provider_id = command.provider, .stdin_is_tty = stdin_is_tty()}, std::cin, std::cout,
                                     std::cerr, application_process_scope);
}

int dispatch_runtime(RuntimeInvocation command, ava::config::XdgPaths paths, char const* program, ava::process::ProcessScopeV1 const& application_process_scope)
{
  auto fatal_error = [&](int status, std::string_view text) {
    std::cerr << program << ": " << text << '\n';
    return status;
  };
  if (command.session_dir)
  {
    std::error_code error;
    auto resolved = std::filesystem::absolute(*command.session_dir, error);
    if (error)
    {
      std::cerr << program << ": failed to resolve --session-dir: " << error.message() << '\n';
      return 2;
    }
    paths.sessions_dir = resolved.lexically_normal();
  }

  runtime::OpenContext open_context;
  open_context.application_process_scope = application_process_scope;
  auto cwd = ava::core::launch_workspace_root();
  if (!cwd)
  {
    std::cerr << program << ": " << cwd.error().format() << '\n';
    return 1;
  }
  open_context.workspace_dir = std::move(*cwd);
  open_context.current_dir = open_context.workspace_dir;
  if (command.requested_current_dir)
  {
    auto const candidate = command.requested_current_dir->is_absolute() ? command.requested_current_dir->lexically_normal()
                                                                        : (open_context.workspace_dir / *command.requested_current_dir).lexically_normal();
    if (!logical_path_is_within(candidate, open_context.workspace_dir))
      return fatal_error(2, "--cwd must be equal to or inside the workspace directory");
    open_context.current_dir = candidate;
  }
  auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(paths, command.trace_requested);
  if (!diagnostics)
    return fatal_error(1,
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
  {
    std::cerr << program << ": " << provider_catalog.error().format() << '\n';
    return 1;
  }
  open_context.provider_catalog = std::move(*provider_catalog);
  runtime::SessionLifecycleRequest lifecycle;
  lifecycle.sessionless = command.sessionless;
  lifecycle.requested_session_id = std::move(command.requested_session_id);
  lifecycle.fork_session_id = std::move(command.fork_session_id);
  lifecycle.initial_session_name = std::move(command.initial_session_name);
  lifecycle.continue_last_session = command.continue_last_session;
  lifecycle.initial_reasoning_level = std::move(command.initial_reasoning_level);

  if (command.frontend == RuntimeFrontend::Print)
    return run_print_mode(PrintModeOptions{.open_context = open_context,
                                           .lifecycle_request = lifecycle,
                                           .explicit_prompt = command.print_prompt,
                                           .read_stdin = !stdin_is_tty(),
                                           .output_format = command.print_output_format,
                                           .permission_policy = std::move(command.headless_permission_policy),
                                           .provider_override = std::nullopt,
                                           .transport_override = std::nullopt},
                          std::cin, std::cout, std::cerr);
  if (command.frontend == RuntimeFrontend::Rpc)
    return run_rpc_mode(
        RpcModeOptions{.open_context = open_context, .lifecycle_request = lifecycle, .permission_policy = std::move(command.headless_permission_policy)},
        std::cin, std::cout, std::cerr, rpc::RpcInputWake{});

  auto session = runtime::Session::open(open_context, lifecycle);
  if (!session)
  {
    std::cerr << program << ": " << session.error().format() << '\n';
    return 1;
  }
  bool const farewell = stdin_is_tty() && stdout_is_tty();
  int const status = run_interactive(*session);
  if (farewell)
    print_exit_card(*session, status);
  return status;
}

}  // namespace

int run(int argc, char** argv, ava::process::ProcessScopeV1 const& application_process_scope)
{
  CommandLineInvocation invocation = parse_command_line(argc, argv);
  apply_invocation_signal_policy(invocation_mode(invocation));

  if (auto const* immediate = std::get_if<ImmediateInvocation>(&invocation))
    return dispatch_immediate(*immediate);
  if (auto const* acp = std::get_if<AcpInvocation>(&invocation))
  {
    auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(ava::config::xdg_paths(), acp->trace);
    if (!diagnostics)
    {
      std::cerr << argv[0] << ": trace startup failed: private diagnostics storage is unavailable.\n";
      return 1;
    }
    return run_acp_mode(std::cerr, application_process_scope, std::move(*diagnostics));
  }
  if (auto const* doctor = std::get_if<DoctorInvocation>(&invocation))
  {
    std::error_code error;
    auto workspace = std::filesystem::current_path(error);
    if (error)
      workspace.clear();
    return run_doctor(ava::config::xdg_paths(), workspace, doctor->json, std::cout, std::cerr);
  }
  if (std::holds_alternative<SupportExportInvocation>(invocation))
  {
    std::error_code error;
    auto workspace = std::filesystem::current_path(error);
    if (error)
      workspace.clear();
    return run_support_export(ava::config::xdg_paths(), workspace, std::cout, std::cerr);
  }
  if (auto const* connect = std::get_if<ConnectInvocation>(&invocation))
    return dispatch_connect(*connect, ava::config::xdg_paths(), application_process_scope);
  return dispatch_runtime(std::get<RuntimeInvocation>(std::move(invocation)), ava::config::xdg_paths(), argv[0], application_process_scope);
}

}  // namespace ava::app
