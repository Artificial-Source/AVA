#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/agent/question.h"
#include "ava/agent/subagent_config.h"
#include "ava/agent/subagent_launch.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ava::agent {

class SubagentCoordinator;

struct TaskSubagentRequest
{
  std::string description;
  std::string prompt;
  std::string subagent_type;
  std::string subagent_system_prompt;
  SubagentToolPreset tool_preset = SubagentToolPreset::Inherit;
  std::optional<std::size_t> max_tool_iterations = std::nullopt;
  std::optional<std::string> task_id = std::nullopt;
  std::string command;
  bool background = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TaskSubagentResult
{
  std::string task_id;
  std::string job_id;
  std::filesystem::path session_path;
  std::string subagent_type;
  std::string state = "completed";
  std::string final_text;
  std::string stop_reason;
  std::size_t provider_iterations = 0;
  std::size_t tool_calls = 0;
  std::size_t tool_iterations = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using TaskSubagentRunner = std::function<ava::core::Result<TaskSubagentResult>(TaskSubagentRequest const&)>;

// Agent-owned collaborators for interactive dispatch (question/task/job). Lower-level
// ToolContext remains execution/safety authority and must not carry these fields.
struct ToolDispatchServices
{
  QuestionResolver question_resolver = nullptr;
  TaskSubagentRunner task_subagent_runner = nullptr;
  // Dedicated private observer for validated built-in task dispatch only.
  SubagentLaunchObserver subagent_launch = {};
  // Exact coordinator/owner pair for public job controls. Child loops clear
  // both this coordinator and the task runner, then hide both schemas.
  std::shared_ptr<SubagentCoordinator> subagent_coordinator = nullptr;
  std::vector<SubagentDefinition> subagents = {};

#ifdef CWDEBUG
  // OPT_OUT keeps callbacks/launch metadata out of generated printing; this
  // hand-written print_on is only for Debug nesting under generated parents and
  // emits a fixed opaque token (no callbacks, coordinator, subagents, or pointers).
  void print_on(std::ostream& os) const
  {
    os << "{ToolDispatchServices}";
  }
#endif

  // Carries callbacks and private launch metadata; never auto-print it.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::agent
