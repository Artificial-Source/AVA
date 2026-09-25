#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/connect_openai.h"
#include "ava/app/headless_policy.h"
#include "ava/app/print_mode.h"
#include "ava/app/runtime.h"
#include "ava/agent/tool_visibility.h"

#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace ava::app {

// Identify output-only commands whose payload can be emitted after parsing has
// selected the default signal policy.
enum class ImmediateOutputKind
{
  Help,
  Version,
  PackageDeferred,
  Text,
};

struct ImmediateInvocation
{
  ImmediateOutputKind kind = ImmediateOutputKind::Text;
  int status = 0;
  bool stderr_output = false;
  std::string text;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Carry the validated trace choice for a standalone ACP invocation.
struct AcpInvocation
{
  bool trace = false;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Carry the validated output format for a doctor invocation.
struct DoctorInvocation
{
  bool json = false;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Identify the validated support-export command.
struct SupportExportInvocation
{
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Identify how a connect invocation obtains its credential.
enum class ConnectCredentialSource
{
  None,
  Stdin,
  Env,
  Prompt,
  BrowserOAuth,
  HeadlessOAuth,
};

// Carry the provider and credential source selected by connect, login, or auth login.
struct ConnectInvocation
{
  std::optional<std::string> provider;
  ConnectCredentialSource source = ConnectCredentialSource::None;
  std::optional<ConnectCredentialType> credential_type;
  std::optional<std::string> env_var;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Identify which runtime frontend receives the common runtime options.
enum class RuntimeFrontend
{
  Interactive,
  LineShell,
  Print,
  Rpc,
};

// Carry validated common runtime and frontend-specific command-line options.
struct RuntimeInvocation
{
  RuntimeFrontend frontend = RuntimeFrontend::Interactive;
  ava::agent::Mode mode = ava::agent::Mode::Build;
  std::optional<std::string> requested_session_id;
  std::optional<std::string> fork_session_id;
  std::optional<std::string> initial_session_name;
  std::optional<std::filesystem::path> session_dir;
  std::optional<std::filesystem::path> requested_current_dir;
  bool continue_last_session = false;
  bool sessionless = false;
  bool offline = false;
  bool trace_requested = false;
  std::optional<std::string> print_prompt;
  PrintOutputFormat print_output_format = PrintOutputFormat::Text;
  runtime::PromptOverrides prompt_overrides;
  HeadlessPermissionPolicyOptions headless_permission_policy;
  ava::agent::ToolVisibilityOptions tool_visibility;
  std::optional<std::string> requested_primary_agent;
  std::optional<std::string> initial_reasoning_level;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using CommandLineInvocation = std::variant<ImmediateInvocation, AcpInvocation, DoctorInvocation, SupportExportInvocation, ConnectInvocation, RuntimeInvocation>;

bool stdin_is_tty();
bool stdout_is_tty();

// Parse argc entries from argv into one fully resolved invocation.
//
// Parsing performs no output, filesystem access, TTY queries, or mode dispatch.
// Validation failures are represented as immediate stderr invocations so the
// caller can select the default signal policy before writing the diagnostic.
[[nodiscard]] CommandLineInvocation parse_command_line(int argc, char const* const* argv);

}  // namespace ava::app
