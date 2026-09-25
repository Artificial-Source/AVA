#include "sys.h"
#include "ava/app/command_line.h"
#include "ava/agent/mode.h"

#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

ImmediateInvocation error(int status, std::string text)
{
  return {.kind = ImmediateOutputKind::Text, .status = status, .stderr_output = true, .text = std::move(text)};
}

ImmediateInvocation program_error(std::string_view program, int status, std::string_view text)
{
  return error(status, std::string(program) + ": " + std::string(text) + '\n');
}

bool is_cli_option(std::string_view arg)
{
  return arg == "--help" || arg == "-h" || arg == "--version" || arg == "--mode" || arg == "--session" || arg == "--session-id" || arg == "--continue" ||
         arg == "--resume" || arg == "-c" || arg == "-r" || arg == "--fork" || arg == "--name" || arg == "-n" || arg == "--session-dir" || arg == "--cwd" ||
         arg == "--no-session" || arg == "--offline" || arg == "--trace" || arg == "--thinking" || arg == "--agent" || arg == "--system-prompt" ||
         arg == "--append-system-prompt" || arg == "--print" || arg == "-p" || arg == "--rpc" || arg == "--acp" || arg == "--json" ||
         arg == "--output" || arg == "--allow" || arg == "--allow-tool" || arg == "--tools" || arg == "-t" || arg == "--exclude-tools" || arg == "-xt" ||
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
  return "@\"" + std::string(path) + '"';
}

void append_prompt_argument(std::optional<std::string>& prompt, std::string_view argument)
{
  if (!prompt)
    prompt = std::string(argument);
  else
  {
    if (!prompt->empty())
      *prompt += ' ';
    prompt->append(argument);
  }
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
    combined += "\n\n" + *prompt;
  prompt = std::move(combined);
}

CommandLineInvocation parse_connect(int argc, char const* const* argv, int index, std::optional<std::string> provider)
{
  ConnectInvocation command{.provider = std::move(provider), .source = ConnectCredentialSource::None, .credential_type = std::nullopt, .env_var = std::nullopt};
  auto set_source = [&](ConnectCredentialSource source, ConnectCredentialType type) -> std::optional<ImmediateInvocation> {
    if (command.source != ConnectCredentialSource::None)
      return program_error(argv[0], 2, "connect accepts only one credential source");
    command.source = source;
    command.credential_type = type;
    return std::nullopt;
  };

  while (index + 1 < argc)
  {
    std::string_view const option(argv[++index]);
    std::optional<ImmediateInvocation> failed;
    if (option == "--api-key")
      failed = set_source(ConnectCredentialSource::Prompt, ConnectCredentialType::ApiKey);
    else if (option == "--browser-oauth")
      failed = set_source(ConnectCredentialSource::BrowserOAuth, ConnectCredentialType::ApiKey);
    else if (option == "--headless-oauth")
      failed = set_source(ConnectCredentialSource::HeadlessOAuth, ConnectCredentialType::ApiKey);
    else if (option == "--api-key-stdin")
      failed = set_source(ConnectCredentialSource::Stdin, ConnectCredentialType::ApiKey);
    else if (option == "--api-key-env")
    {
      failed = set_source(ConnectCredentialSource::Env, ConnectCredentialType::ApiKey);
      if (!failed && index + 1 >= argc)
        return program_error(argv[0], 2, "--api-key-env requires an environment variable name");
      if (!failed)
        command.env_var = std::string(argv[++index]);
    }
    else
      return program_error(argv[0], 2, "unknown connect option");
    if (failed)
      return std::move(*failed);
  }

  if ((command.source == ConnectCredentialSource::BrowserOAuth || command.source == ConnectCredentialSource::HeadlessOAuth) &&
      (!command.provider || *command.provider != "openai"))
    return program_error(argv[0], 2, "OpenAI OAuth flags require provider `openai`");
  if ((command.source == ConnectCredentialSource::Stdin || command.source == ConnectCredentialSource::Env) && !command.provider)
    return program_error(argv[0], 2, "connect requires a provider with headless credential sources");
  return command;
}

}  // namespace

bool stdin_is_tty()
{
  return isatty(STDIN_FILENO) == 1;
}

bool stdout_is_tty()
{
  return isatty(STDOUT_FILENO) == 1;
}

CommandLineInvocation parse_command_line(int argc, char const* const* argv)
{
  if (argc >= 2 && std::string_view(argv[1]) == "doctor")
  {
    if (argc == 2)
      return DoctorInvocation{};
    if (argc == 3 && std::string_view(argv[2]) == "--json")
      return DoctorInvocation{.json = true};
    return error(2, "Usage: " + std::string(argv[0]) + " doctor [--json]\n");
  }
  if (argc >= 2 && std::string_view(argv[1]) == "support")
  {
    if (argc == 3 && std::string_view(argv[2]) == "export")
      return SupportExportInvocation{};
    return error(2, "Usage: " + std::string(argv[0]) + " support export\n");
  }

  int acp_flags = 0;
  int trace_flags = 0;
  for (int index = 1; index < argc; ++index)
  {
    acp_flags += std::string_view(argv[index]) == "--acp" ? 1 : 0;
    trace_flags += std::string_view(argv[index]) == "--trace" ? 1 : 0;
  }
  if (acp_flags > 0)
  {
    if (acp_flags != 1 || trace_flags > 1 || argc != 2 + trace_flags)
      return program_error(argv[0], 2, "--acp is a standalone mode and accepts only one optional --trace flag");
    return AcpInvocation{.trace = trace_flags == 1};
  }
  if (trace_flags > 1)
    return program_error(argv[0], 2, "--trace may be specified only once");

  RuntimeInvocation command;
  bool print_mode = false;
  bool rpc_mode = false;
  bool print_output_flag_seen = false;
  bool print_permission_flag_seen = false;
  std::vector<std::string> file_arguments;

  for (int index = 1; index < argc; ++index)
  {
    std::string_view const arg(argv[index]);
    if (arg == "connect" || arg == "login")
    {
      std::optional<std::string> provider;
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--"))
        provider = argv[++index];
      return parse_connect(argc, argv, index, std::move(provider));
    }
    if (arg == "auth")
    {
      if (index + 1 >= argc || std::string_view(argv[++index]) != "login")
        return program_error(argv[0], 2, "auth requires login");
      std::optional<std::string> provider;
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--"))
        provider = argv[++index];
      return parse_connect(argc, argv, index, std::move(provider));
    }
    if (arg == "packages" || arg == "package")
      return ImmediateInvocation{.kind = ImmediateOutputKind::PackageDeferred, .status = 0, .stderr_output = false, .text = {}};
    if (arg == "--help" || arg == "-h")
      return ImmediateInvocation{.kind = ImmediateOutputKind::Help, .status = 0, .stderr_output = false, .text = {}};
    if (arg == "--version")
      return ImmediateInvocation{.kind = ImmediateOutputKind::Version, .status = 0, .stderr_output = false, .text = {}};
    if (arg == "--mode")
    {
      if (index + 1 >= argc)
        return program_error(argv[0], 2, "--mode requires build, plan, text, json, or rpc");
      std::string_view const requested(argv[++index]);
      if (requested == "text" || requested == "json")
      {
        print_mode = true;
        command.print_output_format = requested == "json" ? PrintOutputFormat::Json : PrintOutputFormat::Text;
        print_output_flag_seen = true;
      }
      else if (requested == "rpc")
        rpc_mode = true;
      else if (auto parsed = ava::agent::parse_mode(requested))
        command.mode = *parsed;
      else
        return program_error(argv[0], 2, "--mode requires build, plan, text, json, or rpc");
      continue;
    }
    if (arg == "--agent")
    {
      if (command.requested_primary_agent)
        return error(2, "--agent may be specified only once\n");
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
        return error(2, "--agent requires one agent name\n");
      command.requested_primary_agent = argv[++index];
      continue;
    }
    if (arg == "--print" || arg == "-p")
    {
      print_mode = true;
      if (index + 1 < argc && !is_cli_option(argv[index + 1]) && !is_cli_file_argument(argv[index + 1]))
        append_prompt_argument(command.print_prompt, argv[++index]);
      continue;
    }
    if (arg == "--rpc")
    {
      rpc_mode = true;
      continue;
    }
    if (arg == "--json")
    {
      command.print_output_format = PrintOutputFormat::Json;
      print_output_flag_seen = true;
      continue;
    }
    if (arg == "--output")
    {
      if (index + 1 >= argc)
        return program_error(argv[0], 2, "--output requires json, text, or rpc");
      std::string_view const output(argv[++index]);
      if (output == "json" || output == "text")
        command.print_output_format = output == "json" ? PrintOutputFormat::Json : PrintOutputFormat::Text;
      else if (output == "rpc")
        rpc_mode = true;
      else
        return program_error(argv[0], 2, "--output requires json, text, or rpc");
      print_output_flag_seen = true;
      continue;
    }
    if (arg == "--allow" || arg == "--allow-tool")
    {
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
        return program_error(argv[0], 2, arg == "--allow" ? "--allow requires read-only" : "--allow-tool requires a comma-separated tool list");
      auto added = arg == "--allow" ? add_headless_allow_policy(command.headless_permission_policy, argv[++index])
                                    : add_headless_allowed_tools(command.headless_permission_policy, argv[++index]);
      if (!added)
        return program_error(argv[0], 2, added.error().format());
      print_permission_flag_seen = true;
      continue;
    }
    if (arg == "--tools" || arg == "-t" || arg == "--exclude-tools" || arg == "-xt")
    {
      if (index + 1 >= argc || is_cli_option(argv[index + 1]))
        return program_error(argv[0], 2, std::string(arg) + " requires a comma-separated tool list");
      auto& target = (arg == "--tools" || arg == "-t") ? command.tool_visibility.included_tools : command.tool_visibility.excluded_tools;
      append_tool_visibility_names(target, argv[++index]);
      continue;
    }
    if (arg == "--no-builtin-tools" || arg == "-nbt")
    {
      if (command.tool_visibility.mode != ava::agent::ToolVisibilityMode::NoTools)
        command.tool_visibility.mode = ava::agent::ToolVisibilityMode::NoBuiltinTools;
      continue;
    }
    if (arg == "--no-tools" || arg == "-nt")
    {
      command.tool_visibility.mode = ava::agent::ToolVisibilityMode::NoTools;
      continue;
    }
    auto require_value = [&](std::string_view message) -> std::optional<ImmediateInvocation> {
      if (index + 1 < argc)
        return std::nullopt;
      return program_error(argv[0], 2, message);
    };
    if (arg == "--session" || arg == "--session-id")
    {
      if (auto failed = require_value(std::string(arg) + " requires a session id"))
        return std::move(*failed);
      command.requested_session_id = argv[++index];
      continue;
    }
    if (arg == "--fork")
    {
      if (auto failed = require_value("--fork requires a session id"))
        return std::move(*failed);
      command.fork_session_id = argv[++index];
      continue;
    }
    if (arg == "--name" || arg == "-n")
    {
      if (auto failed = require_value(std::string(arg) + " requires a session name"))
        return std::move(*failed);
      command.initial_session_name = argv[++index];
      continue;
    }
    if (arg == "--session-dir" || arg == "--cwd")
    {
      if (index + 1 >= argc || (arg == "--cwd" && is_cli_option(argv[index + 1])))
        return program_error(argv[0], 2, arg == "--cwd" ? "--cwd requires a path" : "--session-dir requires a directory");
      if (arg == "--cwd")
        command.requested_current_dir = argv[++index];
      else
        command.session_dir = argv[++index];
      continue;
    }
    if (arg == "--continue" || arg == "--resume" || arg == "-c" || arg == "-r")
      command.continue_last_session = true;
    else if (arg == "--no-session")
      command.sessionless = true;
    else if (arg == "--offline")
      command.offline = true;
    else if (arg == "--trace")
      command.trace_requested = true;
    else if (arg == "--thinking" || arg == "--system-prompt" || arg == "--append-system-prompt")
    {
      if (index + 1 >= argc || (arg == "--thinking" && is_cli_option(argv[index + 1])))
      {
        if (arg == "--thinking")
          return program_error(argv[0], 2, "--thinking requires a reasoning level (off or a level supported by the active model)");
        return program_error(argv[0], 2, arg == "--system-prompt" ? "--system-prompt requires text" : "--append-system-prompt requires text");
      }
      if (arg == "--thinking")
        command.initial_reasoning_level = argv[++index];
      else if (arg == "--system-prompt")
        command.prompt_overrides.system_prompt = argv[++index];
      else
        command.prompt_overrides.append_system_prompts.emplace_back(argv[++index]);
    }
    else if (is_cli_file_argument(arg))
    {
      print_mode = true;
      file_arguments.emplace_back(arg.substr(1));
    }
    else if (!arg.starts_with('-'))
    {
      print_mode = true;
      append_prompt_argument(command.print_prompt, arg);
    }
    else
      return program_error(argv[0], 2, "unknown argument: " + std::string(arg));
  }

  if (print_mode && rpc_mode)
    return program_error(argv[0], 2, "use either --print or --rpc, not both");
  if (!print_mode && !rpc_mode && print_output_flag_seen)
    return program_error(argv[0], 2, "--json and --output text/json are only supported with --print; use --rpc or --output rpc for RPC");
  if (!print_mode && !rpc_mode && print_permission_flag_seen)
    return program_error(argv[0], 2, "--allow and --allow-tool are only supported with --print or --rpc");
  int const persisted_modes = (command.requested_session_id ? 1 : 0) + (command.continue_last_session ? 1 : 0) + (command.fork_session_id ? 1 : 0);
  if (persisted_modes > 1)
    return program_error(argv[0], 2, "use only one of --session/--session-id, --continue/--resume, or --fork");
  if (command.sessionless && persisted_modes > 0)
    return program_error(argv[0], 2, "use either --no-session or session resume options, not both");
  if (!print_mode && !rpc_mode && (!stdin_is_tty() || !stdout_is_tty()))
    return program_error(argv[0], 2, "interactive TUI requires a terminal on both stdin and stdout; use --print or --rpc for non-interactive runs");

  prepend_file_arguments_to_prompt(command.print_prompt, file_arguments);
  command.frontend = print_mode   ? RuntimeFrontend::Print
                     : rpc_mode   ? RuntimeFrontend::Rpc
                                  : RuntimeFrontend::Interactive;
  return command;
}

}  // namespace ava::app
