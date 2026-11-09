#pragma once
#include "ava/http/transport.h"
#include "ava/app/ExitCode.h"
#include "ava/app/headless_policy.h"
#include "ava/app/runtime.h"
#include "ava/provider/provider.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace ava::app {

enum class PrintOutputFormat
{
  Text,
  Json,
};

struct PrintPromptInputs
{
  std::optional<ava::core::NoLogString> explicit_prompt;
  std::optional<ava::core::NoLogString> stdin_prompt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PrintModeRunOptions
{
  PrintOutputFormat output_format = PrintOutputFormat::Text;
  runtime::RunOptions runtime_options;
  bool sanitize_terminal_output = false;
  bool sanitize_terminal_diagnostics = false;

  // runtime_options may hold provider credentials.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PrintModeOptions
{
  runtime::OpenContext open_context;
  runtime::SessionLifecycleRequest lifecycle_request;
  std::optional<ava::core::NoLogString> explicit_prompt;
  bool read_stdin = false;
  PrintOutputFormat output_format = PrintOutputFormat::Text;
  HeadlessPermissionPolicyOptions permission_policy;
  bool parallel_read_search_tools = false;
  std::size_t parallel_read_search_max_workers = 4;
  std::optional<std::reference_wrapper<ava::provider::Provider const>> provider_override;
  std::optional<std::reference_wrapper<ava::http::Transport>> transport_override;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<std::string> merge_print_prompt(PrintPromptInputs const& inputs);

[[nodiscard]] ava::core::Result<ava::agent::AgentLoopResult> run_print_prompt(runtime::session_ts& unlocked_session, std::string const& prompt,
                                                                              ava::provider::Provider const& provider, ava::http::Transport& transport,
                                                                              PrintModeRunOptions const& options, std::ostream& out, std::ostream& err);

// Run one prompt in print mode with options, reading the prompt from options and/or in, writing the answer to out
// and run diagnostics to err.
//
// Returns normally only when the prompt ran successfully.
// Otherwise throws an AIAlert::ErrorCode whose code is the ExitCode that main must return
// (usage, terminal_io_failed, interrupted, session_unavailable, environment_unavailable, configuration_invalid,
// credentials_unavailable or run_failed) and whose message is meant to be printed after the program name.
// When the run itself fails (run_failed) its cause was already reported on err, or as an error event on out.
void run_print_mode(PrintModeOptions const& options, std::istream& in, std::ostream& out, std::ostream& err);

}  // namespace ava::app
