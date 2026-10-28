#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/agent/tool_metadata.h"
#include "ava/agent/tool_types.h"
#include "ava/core/result.h"

#include <cstddef>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace ava::agent {

enum class ToolScheduleEligibility
{
  ReadOnlyCandidate,
  Barrier,
  Deferred,
};

// Permission gate populated by AgentLoop before parallel tool scheduling.
// A ReadOnlyCandidate is only structurally eligible. It must be marked as
// PreflightProvenNonInteractive after permission preflight proves the call
// cannot Ask before the parallel scheduler may include it in a worker epoch.
enum class ToolScheduleParallelReadiness
{
  SequentialOnly,
  PreflightProvenNonInteractive,
};

struct ToolScheduleClassification
{
  ToolScheduleEligibility eligibility = ToolScheduleEligibility::Deferred;
  std::string reason = "unknown_tool";
  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ToolScheduleSlot
{
  std::size_t provider_index = 0;
  ProviderToolCall call;
  ToolScheduleClassification classification;
  ToolScheduleParallelReadiness parallel_readiness = ToolScheduleParallelReadiness::SequentialOnly;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ToolScheduleOutcome
{
  ToolScheduleSlot slot;
  ToolDispatchResult result;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using ToolScheduleExecutor = std::function<ava::core::Result<ToolDispatchResult>(ToolScheduleSlot const& slot)>;
using ToolParallelScheduleExecutor = std::function<ava::core::Result<ToolDispatchResult>(ToolScheduleSlot const& slot, std::stop_token stop_token)>;

struct ToolParallelScheduleOptions
{
  std::size_t max_workers = 4;
  std::stop_token stop_token = {};
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ToolScheduleClassification classify_tool_for_scheduling(ProviderToolCall const& call, std::span<ToolMetadata const> tool_metadata);

[[nodiscard]] std::vector<ToolScheduleSlot> build_sequential_tool_schedule(std::span<ProviderToolCall const> calls,
                                                                           std::span<ToolMetadata const> tool_metadata);

[[nodiscard]] ava::core::Result<std::vector<ToolScheduleOutcome>> run_sequential_tool_schedule(std::span<ToolScheduleSlot const> schedule,
                                                                                               ToolScheduleExecutor const& executor);

[[nodiscard]] ava::core::Result<std::vector<ToolScheduleOutcome>> run_parallel_tool_schedule(std::span<ToolScheduleSlot const> schedule,
                                                                                             ToolParallelScheduleExecutor const& executor,
                                                                                             ToolParallelScheduleOptions options = {});

}  // namespace ava::agent
