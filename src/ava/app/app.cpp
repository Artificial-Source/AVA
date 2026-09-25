#include "sys.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/app/acp_mode.h"
#include "ava/app/app.h"
#include "ava/app/connect_openai.h"
#include "ava/app/doctor_support.h"
#include "ava/app/headless_policy.h"
#include "ava/app/interactive.h"
#include "ava/app/print_mode.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/agent/mode.h"
#include "ava/tui/composer.h"
#include "ava/config/xdg_paths.h"
#include "ava/provider/catalog.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/version.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
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

bool stdin_is_tty()
{
  return isatty(STDIN_FILENO) == 1;
}

bool stdout_is_tty()
{
  return isatty(STDOUT_FILENO) == 1;
}

bool is_cli_option(std::string_view arg)
{
  return arg == "--help" || arg == "-h" || arg == "--version" || arg == "--mode" || arg == "--session" || arg == "--session-id" || arg == "--continue" ||
         arg == "--resume" || arg == "-c" || arg == "-r" || arg == "--fork" || arg == "--name" || arg == "-n" || arg == "--session-dir" || arg == "--cwd" ||
         arg == "--no-session" || arg == "--offline" || arg == "--trace" || arg == "--thinking" || arg == "--agent" || arg == "--system-prompt" ||
         arg == "--append-system-prompt" || arg == "--print" || arg == "-p" || arg == "--rpc" || arg == "--acp" || arg == "--json" || arg == "--output" ||
         arg == "--allow" || arg == "--allow-tool" || arg == "--tools" || arg == "-t" || arg == "--exclude-tools" || arg == "-xt" ||
         arg == "--no-builtin-tools" || arg == "-nbt" || arg == "--no-tools" || arg == "-nt";
}

bool is_cli_file_argument(std::string_view arg)
{
  return arg.size() > 1 && arg.front() == '@';
}

std::string prompt_reference_for_cli_file_argument(std::string_view path)
{
  bool needs_quotes = false;
  for (char const ch : path)
  {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0)
    {
      needs_quotes = true;
      break;
    }
  }

  if (!needs_quotes)
    return "@" + std::string(path);

  std::string reference = "@\"";
  reference.append(path);
  reference += '"';
  return reference;
}

void append_prompt_argument(std::optional<std::string>& prompt, std::string_view argument)
{
  if (!prompt)
  {
    prompt = std::string(argument);
    return;
  }
  if (!prompt->empty())
    *prompt += ' ';
  prompt->append(argument);
}

std::string_view trim_cli_token(std::string_view token)
{
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())) != 0)
    token.remove_prefix(1);
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0)
    token.remove_suffix(1);
  return token;
}

void append_tool_visibility_names(std::vector<std::string>& target, std::string_view csv)
{
  std::size_t start = 0;
  while (start <= csv.size())
  {
    auto const end = csv.find(',', start);
    auto const token = trim_cli_token(csv.substr(start, end == std::string_view::npos ? csv.size() - start : end - start));
    if (!token.empty())
      target.emplace_back(token);
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
}

void prepend_file_arguments_to_prompt(std::optional<std::string>& prompt, std::vector<std::string> const& file_arguments)
{
  if (file_arguments.empty())
    return;

  std::string combined;
  for (auto const& file_argument : file_arguments)
  {
    if (!combined.empty())
      combined += ' ';
    combined += prompt_reference_for_cli_file_argument(file_argument);
  }
  if (prompt && !prompt->empty())
  {
    combined += "\n\n";
    combined += *prompt;
  }
  prompt = std::move(combined);
}

// Compare the normalized logical string forms of `path` and `root` without resolving symlinks.
//
// Returns true when `path` equals `root` or is below it at a path-component boundary.
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

}  // namespace

int run(int argc, char** argv, ava::process::ProcessScopeV1 const& application_process_scope)
{
  // Print a command-line error with the invoked program name and return its requested exit status.
  //
  // The error text must not include a trailing newline.
  auto fatal_error = [&](int exit_status, std::string_view error) {
    std::cerr << argv[0] << ": " << error << '\n';
    return exit_status;
  };

  if (argc >= 2 && std::string_view(argv[1]) == "doctor")
  {
    bool json = false;
    if (argc == 3 && std::string_view(argv[2]) == "--json")
      json = true;
    else if (argc != 2)
    {
      std::cerr << "Usage: " << argv[0] << " doctor [--json]\n";
      return 2;
    }
    std::error_code workspace_error;
    auto workspace = std::filesystem::current_path(workspace_error);
    if (workspace_error)
      workspace.clear();
    return run_doctor(ava::config::xdg_paths(), workspace, json, std::cout, std::cerr);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "support")
  {
    if (argc != 3 || std::string_view(argv[2]) != "export")
    {
      std::cerr << "Usage: " << argv[0] << " support export\n";
      return 2;
    }
    std::error_code workspace_error;
    auto workspace = std::filesystem::current_path(workspace_error);
    if (workspace_error)
      workspace.clear();
    return run_support_export(ava::config::xdg_paths(), workspace, std::cout, std::cerr);
  }

  int acp_flags = 0;
  int acp_trace_flags = 0;
  for (int index = 1; index < argc; ++index)
  {
    acp_flags += std::string_view(argv[index]) == "--acp" ? 1 : 0;
    acp_trace_flags += std::string_view(argv[index]) == "--trace" ? 1 : 0;
  }
  if (acp_flags > 0)
  {
    bool const valid = acp_flags == 1 && acp_trace_flags <= 1 && argc == 2 + acp_trace_flags;
    if (!valid)
    {
      return fatal_error(2, "--acp is a standalone mode and accepts only one optional --trace flag");
    }
    auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(ava::config::xdg_paths(), acp_trace_flags == 1);
    if (!diagnostics)
    {
      return fatal_error(1, "trace startup failed: private diagnostics storage is unavailable.");
    }
    return run_acp_mode(std::cerr, application_process_scope, std::move(*diagnostics));
  }
  if (acp_trace_flags > 1)
  {
    return fatal_error(2, "--trace may be specified only once");
  }

  auto mode = ava::agent::Mode::Build;
  std::optional<std::string> requested_session_id;
  std::optional<std::string> fork_session_id;
  std::optional<std::string> initial_session_name;
  std::optional<std::filesystem::path> session_dir;
  std::optional<std::filesystem::path> requested_current_dir;
  bool continue_last_session = false;
  bool sessionless = false;
  bool offline = false;
  bool trace_requested = false;
  bool print_mode = false;
  bool rpc_mode = false;
  std::optional<std::string> print_prompt;
  std::vector<std::string> cli_file_arguments;
  auto print_output_format = ava::app::PrintOutputFormat::Text;
  bool print_output_flag_seen = false;
  bool print_permission_flag_seen = false;
  ava::app::runtime::PromptOverrides prompt_overrides;
  ava::app::HeadlessPermissionPolicyOptions headless_permission_policy;
  ava::agent::ToolVisibilityOptions tool_visibility;
  std::optional<std::string> requested_primary_agent;
  std::optional<std::string> initial_reasoning_level;

  auto const paths = ava::config::xdg_paths();

  auto parse_connect_like_command = [&](int& index, std::optional<std::string> provider) -> int {
    enum class CredentialSource
    {
      None,
      Stdin,
      Env,
      Prompt,
      BrowserOAuth,
      HeadlessOAuth,
    };
    CredentialSource source = CredentialSource::None;
    std::optional<ava::app::ConnectCredentialType> credential_type;
    std::optional<std::string> env_var;

    auto set_source = [&](CredentialSource next_source, ava::app::ConnectCredentialType next_type) -> bool {
      if (source != CredentialSource::None)
      {
        std::cerr << argv[0] << ": connect accepts only one credential source\n";
        return false;
      }
      source = next_source;
      credential_type = next_type;
      return true;
    };

    while (index + 1 < argc)
    {
      std::string_view const option(argv[++index]);
      if (option == "--api-key")
      {
        if (!set_source(CredentialSource::Prompt, ava::app::ConnectCredentialType::ApiKey))
          return 2;
        continue;
      }
      if (option == "--browser-oauth")
      {
        if (!set_source(CredentialSource::BrowserOAuth, ava::app::ConnectCredentialType::ApiKey))
          return 2;
        continue;
      }
      if (option == "--headless-oauth")
      {
        if (!set_source(CredentialSource::HeadlessOAuth, ava::app::ConnectCredentialType::ApiKey))
          return 2;
        continue;
      }
      if (option == "--api-key-stdin")
      {
        if (!set_source(CredentialSource::Stdin, ava::app::ConnectCredentialType::ApiKey))
          return 2;
        continue;
      }
      if (option == "--api-key-env")
      {
        if (!set_source(CredentialSource::Env, ava::app::ConnectCredentialType::ApiKey))
          return 2;
        if (index + 1 >= argc)
        {
          std::cerr << argv[0] << ": " << ava::tui::sanitize_terminal_text(std::string(option)) << " requires an environment variable name\n";
          return 2;
        }
        env_var = std::string(argv[++index]);
        continue;
      }
      return fatal_error(2, "unknown connect option");
    }

    if (source == CredentialSource::BrowserOAuth || source == CredentialSource::HeadlessOAuth)
    {
      if (!provider || *provider != "openai")
      {
        return fatal_error(2, "OpenAI OAuth flags require provider `openai`");
      }
      if (source == CredentialSource::BrowserOAuth)
        return run_connect_openai_browser(paths, std::cout, std::cerr, application_process_scope);
      return run_connect_openai_headless(paths, std::cout, std::cerr, application_process_scope);
    }

    if (source == CredentialSource::Stdin || source == CredentialSource::Env)
    {
      if (!provider)
      {
        return fatal_error(2, "connect requires a provider with headless credential sources");
      }
      return run_connect_provider_credential(
          paths, ava::app::ConnectProviderCredentialOptions{.provider_id = *provider, .credential_type = credential_type.value(), .env_var = env_var}, std::cin,
          std::cout, std::cerr);
    }

    if (source == CredentialSource::Prompt)
    {
      return run_connect_provider_wizard(
          paths, ava::app::ConnectProviderWizardOptions{.provider_id = provider, .credential_type = credential_type, .stdin_is_tty = stdin_is_tty()}, std::cin,
          std::cout, std::cerr, application_process_scope);
    }

    if (provider && *provider == "openai")
    {
      return run_connect_openai_wizard(paths, ava::app::ConnectProviderWizardOptions{.provider_id = provider, .stdin_is_tty = stdin_is_tty()}, std::cin,
                                       std::cout, std::cerr, application_process_scope);
    }
    return run_connect_provider_wizard(paths, ava::app::ConnectProviderWizardOptions{.provider_id = provider, .stdin_is_tty = stdin_is_tty()}, std::cin,
                                       std::cout, std::cerr, application_process_scope);
  };

  for (int index = 1; index < argc; ++index)
  {
    std::string_view const arg(argv[index]);
    if (arg == "connect")
    {
      std::optional<std::string> provider;
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--"))
        provider = argv[++index];
      return parse_connect_like_command(index, provider);
    }
    if (arg == "login")
    {
      std::optional<std::string> provider;
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--"))
        provider = argv[++index];
      return parse_connect_like_command(index, provider);
    }
    if (arg == "auth")
    {
      if (index + 1 >= argc || std::string_view(argv[++index]) != "login")
      {
        return fatal_error(2, "auth requires login");
      }
      std::optional<std::string> provider;
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--"))
        provider = argv[++index];
      return parse_connect_like_command(index, provider);
    }
    if (arg == "packages" || arg == "package")
    {
      std::cout << "AVA package manager is deferred pending local-source, provenance, trust, rollback, and compatibility policy.\n"
                   "Install resources manually under $XDG_CONFIG_HOME/ava or trusted project .ava directories; see docs/core/configuration.md and "
                   "docs/extensions/plugin-system.md.\n";
      return 0;
    }
    if (arg == "--help" || arg == "-h")
    {
      print_help();
      return 0;
    }
    if (arg == "--version")
    {
      std::cout << "ava " << version::kFullVersion << '\n';
      return 0;
    }
    if (arg == "--mode")
    {
      if (index + 1 >= argc)
      {
        return fatal_error(2, "--mode requires build, plan, text, json, or rpc");
      }
      std::string_view const requested_mode(argv[++index]);
      if (requested_mode == "text")
      {
        print_mode = true;
        print_output_format = ava::app::PrintOutputFormat::Text;
        print_output_flag_seen = true;
        continue;
      }
      if (requested_mode == "json")
      {
        print_mode = true;
        print_output_format = ava::app::PrintOutputFormat::Json;
        print_output_flag_seen = true;
        continue;
      }
      if (requested_mode == "rpc")
      {
        rpc_mode = true;
        continue;
      }
      auto parsed = ava::agent::parse_mode(requested_mode);
      if (!parsed)
      {
        return fatal_error(2, "--mode requires build, plan, text, json, or rpc");
      }
      mode = *parsed;
      continue;
    }
    if (arg == "--agent")
    {
      if (requested_primary_agent)
      {
        std::cerr << "--agent may be specified only once\n";
        return 2;
      }
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
      {
        std::cerr << "--agent requires one agent name\n";
        return 2;
      }
      requested_primary_agent = argv[++index];
      continue;
    }
    if (arg == "--print" || arg == "-p")
    {
      print_mode = true;
      if (index + 1 < argc && !is_cli_option(argv[index + 1]) && !is_cli_file_argument(argv[index + 1]))
      {
        append_prompt_argument(print_prompt, argv[++index]);
      }
      continue;
    }
    if (arg == "--rpc")
    {
      rpc_mode = true;
      continue;
    }
    if (arg == "--json")
    {
      print_output_format = ava::app::PrintOutputFormat::Json;
      print_output_flag_seen = true;
      continue;
    }
    if (arg == "--output")
    {
      if (index + 1 >= argc)
      {
        return fatal_error(2, "--output requires json, text, or rpc");
      }
      std::string_view const output(argv[++index]);
      if (output == "json")
      {
        print_output_format = ava::app::PrintOutputFormat::Json;
      }
      else if (output == "text")
      {
        print_output_format = ava::app::PrintOutputFormat::Text;
      }
      else if (output == "rpc")
      {
        rpc_mode = true;
      }
      else
      {
        return fatal_error(2, "--output requires json, text, or rpc");
      }
      print_output_flag_seen = true;
      continue;
    }
    if (arg == "--allow")
    {
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
      {
        return fatal_error(2, "--allow requires read-only");
      }
      auto added = ava::app::add_headless_allow_policy(headless_permission_policy, argv[++index]);
      if (!added)
      {
        std::cerr << argv[0] << ": " << added.error().format() << '\n';
        return 2;
      }
      print_permission_flag_seen = true;
      continue;
    }
    if (arg == "--allow-tool")
    {
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
      {
        return fatal_error(2, "--allow-tool requires a comma-separated tool list");
      }
      auto added = ava::app::add_headless_allowed_tools(headless_permission_policy, argv[++index]);
      if (!added)
      {
        std::cerr << argv[0] << ": " << added.error().format() << '\n';
        return 2;
      }
      print_permission_flag_seen = true;
      continue;
    }
    if (arg == "--tools" || arg == "-t")
    {
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
      {
        std::cerr << argv[0] << ": " << std::string(arg) << " requires a comma-separated tool list\n";
        return 2;
      }
      append_tool_visibility_names(tool_visibility.included_tools, argv[++index]);
      continue;
    }
    if (arg == "--exclude-tools" || arg == "-xt")
    {
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
      {
        std::cerr << argv[0] << ": " << std::string(arg) << " requires a comma-separated tool list\n";
        return 2;
      }
      append_tool_visibility_names(tool_visibility.excluded_tools, argv[++index]);
      continue;
    }
    if (arg == "--no-builtin-tools" || arg == "-nbt")
    {
      if (tool_visibility.mode != ava::agent::ToolVisibilityMode::NoTools)
        tool_visibility.mode = ava::agent::ToolVisibilityMode::NoBuiltinTools;
      continue;
    }
    if (arg == "--no-tools" || arg == "-nt")
    {
      tool_visibility.mode = ava::agent::ToolVisibilityMode::NoTools;
      continue;
    }
    if (arg == "--session" || arg == "--session-id")
    {
      if (index + 1 >= argc)
      {
        std::cerr << argv[0] << ": " << std::string(arg) << " requires a session id\n";
        return 2;
      }
      requested_session_id = std::string(argv[++index]);
      continue;
    }
    if (arg == "--fork")
    {
      if (index + 1 >= argc)
      {
        return fatal_error(2, "--fork requires a session id");
      }
      fork_session_id = std::string(argv[++index]);
      continue;
    }
    if (arg == "--name" || arg == "-n")
    {
      if (index + 1 >= argc)
      {
        std::cerr << argv[0] << ": " << std::string(arg) << " requires a session name\n";
        return 2;
      }
      initial_session_name = std::string(argv[++index]);
      continue;
    }
    if (arg == "--session-dir")
    {
      if (index + 1 >= argc)
      {
        return fatal_error(2, "--session-dir requires a directory");
      }
      session_dir = std::filesystem::path(argv[++index]);
      continue;
    }
    if (arg == "--cwd")
    {
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
      {
        return fatal_error(2, "--cwd requires a path");
      }
      requested_current_dir = std::filesystem::path(argv[++index]);
      continue;
    }
    if (arg == "--continue" || arg == "--resume" || arg == "-c" || arg == "-r")
    {
      continue_last_session = true;
      continue;
    }
    if (arg == "--no-session")
    {
      sessionless = true;
      continue;
    }
    if (arg == "--offline")
    {
      offline = true;
      continue;
    }
    if (arg == "--trace")
    {
      if (trace_requested)
      {
        return fatal_error(2, "--trace may be specified only once");
      }
      trace_requested = true;
      continue;
    }
    if (arg == "--thinking")
    {
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
      {
        return fatal_error(2, "--thinking requires a reasoning level (off or a level supported by the active model)");
      }
      initial_reasoning_level = std::string(argv[++index]);
      continue;
    }
    if (arg == "--system-prompt")
    {
      if (index + 1 >= argc)
      {
        return fatal_error(2, "--system-prompt requires text");
      }
      prompt_overrides.system_prompt = std::string(argv[++index]);
      continue;
    }
    if (arg == "--append-system-prompt")
    {
      if (index + 1 >= argc)
      {
        return fatal_error(2, "--append-system-prompt requires text");
      }
      prompt_overrides.append_system_prompts.push_back(std::string(argv[++index]));
      continue;
    }

    if (is_cli_file_argument(arg))
    {
      print_mode = true;
      cli_file_arguments.push_back(std::string(arg.substr(1)));
      continue;
    }

    if (!arg.starts_with("-"))
    {
      print_mode = true;
      append_prompt_argument(print_prompt, arg);
      continue;
    }

    std::cerr << argv[0] << ": unknown argument: " << arg << '\n';
    return 2;
  }

  if (print_mode && rpc_mode)
  {
    return fatal_error(2, "use either --print or --rpc, not both");
  }

  if (!print_mode && !rpc_mode && print_output_flag_seen)
  {
    return fatal_error(2, "--json and --output text/json are only supported with --print; use --rpc or --output rpc for RPC");
  }

  if (!print_mode && !rpc_mode && print_permission_flag_seen)
  {
    return fatal_error(2, "--allow and --allow-tool are only supported with --print or --rpc");
  }

  auto const selected_persisted_session_mode = (requested_session_id ? 1 : 0) + (continue_last_session ? 1 : 0) + (fork_session_id ? 1 : 0);
  if (selected_persisted_session_mode > 1)
  {
    return fatal_error(2, "use only one of --session/--session-id, --continue/--resume, or --fork");
  }
  if (sessionless && selected_persisted_session_mode > 0)
  {
    return fatal_error(2, "use either --no-session or session resume options, not both");
  }

  if (!print_mode && !rpc_mode && (!stdin_is_tty() || !stdout_is_tty()))
  {
    return fatal_error(2, "interactive TUI requires a terminal on both stdin and stdout; use --print or --rpc for non-interactive runs");
  }

  prepend_file_arguments_to_prompt(print_prompt, cli_file_arguments);

  auto runtime_paths = paths;
  if (session_dir)
  {
    std::error_code error;
    auto resolved_session_dir = std::filesystem::absolute(*session_dir, error);
    if (error)
    {
      std::cerr << argv[0] << ": failed to resolve --session-dir: " << error.message() << '\n';
      return 2;
    }
    runtime_paths.sessions_dir = resolved_session_dir.lexically_normal();
  }

  ava::app::runtime::OpenContext open_context;
  open_context.application_process_scope = application_process_scope;
  auto cwd = ava::core::launch_workspace_root();
  if (!cwd)
  {
    std::cerr << argv[0] << ": " << cwd.error().format() << '\n';
    return 1;
  }
  open_context.workspace_dir = std::move(*cwd);
  open_context.current_dir = open_context.workspace_dir;
  if (requested_current_dir)
  {
    auto const candidate = requested_current_dir->is_absolute() ? requested_current_dir->lexically_normal()
                                                                : (open_context.workspace_dir / *requested_current_dir).lexically_normal();
    if (!logical_path_is_within(candidate, open_context.workspace_dir))
      return fatal_error(2, "--cwd must be equal to or inside the workspace directory");
    open_context.current_dir = candidate;
  }

  auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(runtime_paths, trace_requested);
  if (!diagnostics)
  {
    if (trace_requested)
      return fatal_error(1, "trace startup failed: private diagnostics storage is unavailable.");
    return fatal_error(1, "runtime diagnostics startup failed.");
  }

  open_context.mode = mode;
  open_context.tool_visibility = std::move(tool_visibility);
  open_context.requested_primary_agent = std::move(requested_primary_agent);
  open_context.paths = runtime_paths;
  open_context.prompt_overrides = std::move(prompt_overrides);
  open_context.offline = offline;
  open_context.diagnostics = *diagnostics;
  auto provider_catalog = ava::provider::ProviderCatalog::build(runtime_paths);
  if (!provider_catalog)
  {
    std::cerr << argv[0] << ": " << provider_catalog.error().format() << '\n';
    return 1;
  }
  open_context.provider_catalog = std::move(*provider_catalog);
  ava::app::runtime::SessionLifecycleRequest lifecycle_request;
  lifecycle_request.sessionless = sessionless;
  lifecycle_request.requested_session_id = std::move(requested_session_id);
  lifecycle_request.fork_session_id = std::move(fork_session_id);
  lifecycle_request.initial_session_name = std::move(initial_session_name);
  lifecycle_request.continue_last_session = continue_last_session;
  lifecycle_request.initial_reasoning_level = std::move(initial_reasoning_level);

  if (print_mode)
  {
    return ava::app::run_print_mode(ava::app::PrintModeOptions{.open_context = open_context,
                                                               .lifecycle_request = lifecycle_request,
                                                               .explicit_prompt = print_prompt,
                                                               .read_stdin = !stdin_is_tty(),
                                                               .output_format = print_output_format,
                                                               .permission_policy = std::move(headless_permission_policy),
                                                               .provider_override = std::nullopt,
                                                               .transport_override = std::nullopt},
                                    std::cin, std::cout, std::cerr);
  }

  if (rpc_mode)
  {
    return ava::app::run_rpc_mode(
        ava::app::RpcModeOptions{
            .open_context = open_context, .lifecycle_request = lifecycle_request, .permission_policy = std::move(headless_permission_policy)},
        std::cin, std::cout, std::cerr, ava::app::rpc::RpcInputWake{});
  }

  auto unlocked_session_result = ava::app::runtime::Session::open(open_context, lifecycle_request);
  if (!unlocked_session_result)
  {
    std::cerr << argv[0] << ": " << unlocked_session_result.error().format() << '\n';
    return 1;
  }

  bool const print_farewell = stdin_is_tty() && stdout_is_tty();
  int const status = run_interactive(*unlocked_session_result);
  if (print_farewell)
    print_exit_card(*unlocked_session_result, status);
  return status;
}

}  // namespace ava::app
