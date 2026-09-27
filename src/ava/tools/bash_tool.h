#pragma once

#include "ava/tools/tool_context.h"
#include "ava/core/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ava::tools {

struct BashOptions
{
  // Model-visible tool calls preserve a lossless direct-argv recipe when
  // possible. Explicit user shell helpers must choose UserRawShell instead;
  // source is never inferred from command text.
  enum class InvocationSource
  {
    ModelCompatibility,
    UserRawShell,
  };

  std::chrono::milliseconds timeout = std::chrono::milliseconds(30'000);
  std::size_t max_bytes = 50 * 1024;
  std::size_t max_lines = 200;
  InvocationSource invocation_source = InvocationSource::ModelCompatibility;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct BashResult
{
  std::int64_t exit_code = -1;
  bool timed_out = false;
  bool canceled = false;
  bool truncated = false;
  bool byte_limited = false;
  bool line_limited = false;
  bool spill_truncated = false;
  bool totals_known = true;
  std::size_t total_bytes = 0;
  std::size_t output_bytes = 0;
  std::size_t total_lines = 0;
  std::size_t output_lines = 0;
  std::size_t omitted_lines = 0;
  std::string output;
  std::filesystem::path spill_path;
  // Containment status reported only after the parent verifies the child
  // installed containment before exec. Pre-permission metadata never claims
  // Active; these fields remain default (not applied) when containment is
  // unavailable or not required.
  bool containment_applied = false;
  std::string containment_profile_id;
  // "denied" when a network filter was installed; "allowed" when network was
  // explicitly enabled; empty when no containment was applied.
  std::string containment_network_mode;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<BashResult> run_bash(ToolContext const& context, std::string_view command, BashOptions options = {});

}  // namespace ava::tools
