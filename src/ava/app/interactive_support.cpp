#include "sys.h"
#include "ava/app/interactive_internal.h"
#include "ava/app/runtime/Session.h"
#include "ava/permissions/permission.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/mode.h"

#include <string>
#include <utility>

namespace ava::app::interactive_internal {
namespace {

ava::permissions::PermissionRuleMode permission_rule_mode_for_agent_mode(ava::core::Mode mode)
{
  switch (mode)
  {
    case ava::core::Mode::Build:
      return ava::permissions::PermissionRuleMode::Build;
    case ava::core::Mode::Plan:
      return ava::permissions::PermissionRuleMode::Plan;
  }
  return ava::permissions::PermissionRuleMode::Any;
}

}  // namespace

ava::core::Result<std::string> remember_permission_rule(runtime::session_ts const& unlocked_session, ava::permissions::PermissionPrompt const& prompt,
                                                        ava::permissions::PermissionAction action, std::string actor)
{
  auto reason = prompt.reason.empty() ? std::string("remembered from TUI permission prompt") : prompt.reason;
  std::string recipe_key;
  std::string recipe_display;
  if (prompt.operation == ava::permissions::Operation::RunCommand)
  {
    auto const reusable = prompt.command_metadata && ava::permissions::command_permission_allows_reusable_grant(*prompt.command_metadata);
    if (action == ava::permissions::PermissionAction::Allow && !ava::permissions::command_prompt_allows_persistent_allow(prompt))
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                              "this command cannot be remembered because no reusable sealed workspace recipe is available"));
    }
    if (reusable)
    {
      recipe_key = prompt.command_metadata->workspace_recipe_key;
      recipe_display = prompt.command_metadata->recipe_display;
    }
  }
  auto const rule_store = runtime::session_ts::crat(unlocked_session)->permission_rule_store();
  auto added = ava::permissions::add_persistent_permission_rule(
      rule_store, ava::permissions::PermissionRuleDraft{
                      .scope = ava::permissions::PermissionRuleScope::Workspace,
                      .action = action,
                      .operation = prompt.operation,
                      .mode = permission_rule_mode_for_agent_mode(prompt.mode),
                      .tool_name = prompt.tool_name,
                      .target_path = prompt.target_path,
                      .command = prompt.operation == ava::permissions::Operation::RunCommand && !recipe_key.empty() ? std::string{} : prompt.command,
                      .command_recipe_key = std::move(recipe_key),
                      .recipe_display = std::move(recipe_display),
                      .reason = std::move(reason),
                      .actor = std::move(actor)});
  if (!added)
    return std::unexpected(std::move(added.error()));
  return added->rule_id;
}

}  // namespace ava::app::interactive_internal
