#pragma once

#include "ava/command/command.h"
#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tools {

using ToolIoCancelCallback = std::function<bool()>;

struct ExactFileReadOptions
{
  std::optional<std::size_t> line;
  std::optional<std::size_t> limit;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Optional exact-file routing for frontends that own the bytes. Implementations
// receive only absolute paths that have just passed the tool layer's workspace
// and symlink checks. Shared ownership lets one immutable frontend route serve
// all tool-context copies for a session.
class ExactFileAccess
{
 public:
  virtual ~ExactFileAccess() = default;

  // Existing protocol-neutral adapters own both operations unless they opt in
  // to narrower capability reporting.
  [[nodiscard]] virtual bool supports_read_text_file() const noexcept { return true; }
  [[nodiscard]] virtual bool supports_write_text_file() const noexcept { return true; }

  [[nodiscard]] virtual ava::core::Result<std::string> read_text_file(std::filesystem::path const& absolute_path,
                                                                      ToolIoCancelCallback cancel_requested) const = 0;
  [[nodiscard]] virtual ava::core::Result<std::string> read_text_file_window(std::filesystem::path const& absolute_path, ExactFileReadOptions options,
                                                                             ToolIoCancelCallback cancel_requested) const
  {
    auto content = read_text_file(absolute_path, std::move(cancel_requested));
    if (!content || (!options.line && !options.limit))
      return content;

    auto const start_line = std::max<std::size_t>(options.line.value_or(1), 1);
    std::size_t current_line = 1;
    std::size_t begin = 0;
    while (current_line < start_line && begin < content->size())
    {
      auto const newline = content->find('\n', begin);
      if (newline == std::string::npos)
      {
        begin = content->size();
        break;
      }
      begin = newline + 1;
      ++current_line;
    }

    auto end = content->size();
    if (options.limit)
    {
      end = begin;
      std::size_t lines = 0;
      while (end < content->size() && lines < *options.limit)
      {
        auto const newline = content->find('\n', end);
        if (newline == std::string::npos)
        {
          end = content->size();
          ++lines;
          break;
        }
        end = newline + 1;
        ++lines;
      }
    }
    return content->substr(begin, end - begin);
  }
  [[nodiscard]] virtual ava::core::VoidResult write_text_file(std::filesystem::path const& absolute_path, std::string_view content,
                                                              ToolIoCancelCallback cancel_requested) const = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Delegated executors receive a compact plan binding and an explicit
// environment-profile contract. They never receive the local environment
// entries or authority to reuse a local approval.
struct CommandEnvironmentProfileContract
{
  std::string profile_id;
  std::string digest;
  bool local_execution_authority = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandExecutionPlanMetadata
{
  std::string fingerprint;
  ava::command::CommandExecutionDomain execution_domain = ava::command::CommandExecutionDomain::DirectArgv;
  ava::command::CommandLevel level = ava::command::CommandLevel::Critical;
  ava::command::CommandFamily family = ava::command::CommandFamily::UnknownWrapper;
  ava::command::InteractiveScope backend_maximum_scope = ava::command::InteractiveScope::Once;
  std::filesystem::path resolved_executable;
  std::filesystem::path cwd;
  bool executor_identity_verified = false;
  bool containment_available = false;
  std::string containment_profile_id;
  bool containment_network_allowed = false;
  // Set to true only after the parent verifies the child installed
  // containment before exec. Pre-permission metadata never claims applied.
  bool containment_applied = false;
  std::string containment_network_mode;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandExecutionRequest
{
  std::vector<ava::core::NoLogString> argv;
  std::filesystem::path cwd;
  std::chrono::milliseconds timeout{0};
  std::size_t output_byte_limit = 0;
  ToolIoCancelCallback cancel_requested = nullptr;
  std::optional<CommandExecutionPlanMetadata> plan_metadata = std::nullopt;
  std::optional<CommandEnvironmentProfileContract> environment_profile = std::nullopt;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct CommandExecutionResult
{
  std::int64_t exit_code = -1;
  bool timed_out = false;
  bool canceled = false;
  bool truncated = false;
  ava::core::NoLogString output;
  // Containment status reported by a delegated executor only after it
  // verifies the child installed containment before exec.
  bool containment_applied = false;
  std::string containment_profile_id;
  std::string containment_network_mode;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Optional argv-style execution route. The tool layer remains responsible for
// parsing, permission, canonical cwd selection, and user-visible output limits.
class CommandExecutor
{
 public:
  virtual ~CommandExecutor() = default;

  [[nodiscard]] virtual ava::core::Result<CommandExecutionResult> execute(CommandExecutionRequest request) const = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::tools
