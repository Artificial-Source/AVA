#pragma once
#include "ava/http/transport.h"
#include "ava/app/headless_policy.h"
#include "ava/app/runtime.h"
#include "ava/provider/provider.h"
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
  std::optional<std::string> explicit_prompt;
  std::optional<std::string> stdin_prompt;

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
  std::optional<std::string> explicit_prompt;
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

[[nodiscard]] int run_print_mode(PrintModeOptions const& options, std::istream& in, std::ostream& out, std::ostream& err);

}  // namespace ava::app
