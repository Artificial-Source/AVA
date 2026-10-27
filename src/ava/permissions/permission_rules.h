#pragma once

#include "ava/permissions/permission.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::permissions {

// Durable permission rules are intentionally small and inspectable.  AVA stores
// one JSON object per scope (`schema_version: 2`, `rules: [...]`): global rules
// live under the caller-provided config path and enforceable workspace rules are
// stored next to AVA config under a workspace-keyed directory rather than inside
// the model-writable workspace.  A rule matches exact operation plus all
// non-empty target fields (path, command, tool name) and, for workspace rules,
// the normalized workspace directory. Persistent rules are consulted before
// session grants and resolver prompts; matching denies also receive a dedicated
// non-interactive preflight before backend auto-Allow decisions. Explicit
// persistent denies still win over prompt-free task launches. Built-in hard
// Deny decisions are never upgraded by durable allow rules. Schema-v2
// RunCommand Allows are exact stable recipe-key grants; schema-v1 command
// Allows remain inspectable but are non-authoritative, while exact v1 Denies
// remain authoritative. An exact Critical Allow is an advanced manual-only
// exception requiring explicit acknowledgement. Matching deny rules always win over
// matching allow rules, and malformed or unsupported storage fails closed by
// returning an authoritative deny
// instead of falling through to a resolver prompt.

inline constexpr long long kCurrentPermissionRulesSchemaVersion = 2;
inline constexpr long long kLegacyPermissionRulesSchemaVersion = 1;

enum class PermissionRuleScope
{
  Global,
  Workspace,
};

enum class PermissionRuleMode
{
  Any,
  Build,
  Plan,
};

struct PermissionRuleStore
{
  std::filesystem::path global_rules_file;
  std::filesystem::path workspace_rules_file;
  std::filesystem::path workspace_dir;
  // Session-lifetime descriptor authority for the configured storage roots.
  // Logical path spelling remains public; secure storage walks are relative to
  // the matching pre-opened anchor when one is supplied.
  std::shared_ptr<ava::core::AnchorSet> anchor_set = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PersistentPermissionRule
{
  std::string rule_id;
  PermissionRuleScope scope = PermissionRuleScope::Workspace;
  std::filesystem::path workspace_dir;
  PermissionAction action = PermissionAction::Deny;
  Operation operation = Operation::ReadFile;
  PermissionRuleMode mode = PermissionRuleMode::Any;
  std::string tool_name;
  std::filesystem::path target_path;
  ava::core::NoLogString command;
  std::string command_recipe_key;
  ava::core::NoLogString recipe_display;
  bool critical_acknowledged = false;
  long long schema_version = kCurrentPermissionRulesSchemaVersion;
  ava::core::NoLogString reason;
  std::string actor;
  std::string created_at;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PermissionRuleDraft
{
  PermissionRuleScope scope = PermissionRuleScope::Workspace;
  PermissionAction action = PermissionAction::Deny;
  Operation operation = Operation::ReadFile;
  PermissionRuleMode mode = PermissionRuleMode::Any;
  std::string tool_name;
  std::filesystem::path target_path;
  ava::core::NoLogString command;
  // RunCommand recipe keys must originate in sealed prompt/audit metadata.
  std::string command_recipe_key;
  ava::core::NoLogString recipe_display;
  bool critical_acknowledged = false;
  ava::core::NoLogString reason;
  std::string actor = "rpc";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::optional<PermissionRuleScope> parse_permission_rule_scope(std::string_view value);
[[nodiscard]] std::optional<PermissionRuleMode> parse_permission_rule_mode(std::string_view value);
[[nodiscard]] std::string to_string(PermissionRuleScope scope);
[[nodiscard]] std::string to_string(PermissionRuleMode mode);
[[nodiscard]] std::filesystem::path enforceable_permission_rules_file(PermissionRuleStore const& store, PermissionRuleScope scope);
[[nodiscard]] bool is_enforceable_permission_rules_file(PermissionRuleStore const& store, std::filesystem::path const& path);
void register_enforceable_permission_rule_files(PermissionRuleStore const& store);
[[nodiscard]] bool is_registered_enforceable_permission_rules_file(std::filesystem::path const& path);

[[nodiscard]] ava::core::Result<std::vector<PersistentPermissionRule>> load_persistent_permission_rules(PermissionRuleStore const& store);
[[nodiscard]] ava::core::Result<PersistentPermissionRule> add_persistent_permission_rule(PermissionRuleStore const& store, PermissionRuleDraft draft);
[[nodiscard]] ava::core::Result<PersistentPermissionRule> remove_persistent_permission_rule(PermissionRuleStore const& store, std::string_view rule_id);
[[nodiscard]] ava::core::Result<std::optional<PersistentPermissionRule>> match_persistent_permission_rule(PermissionRuleStore const& store,
                                                                                                          PermissionPrompt const& prompt);

[[nodiscard]] PermissionResolver build_persistent_permission_rule_resolver(PermissionRuleStore store, PermissionResolver fallback);
// Non-interactive deny-only preflight for backend auto-Allow decisions,
// including prompt-free task launches. A matching deny or malformed store is
// authoritative; allows are deliberately ignored and absence of a deny returns
// Allow without prompting.
[[nodiscard]] PermissionResolver build_persistent_permission_deny_preflight(PermissionRuleStore store);

[[nodiscard]] std::string permission_rule_json(PersistentPermissionRule const& rule);
[[nodiscard]] std::string permission_rules_result_json(PermissionRuleStore const& store, std::vector<PersistentPermissionRule> const& rules);

}  // namespace ava::permissions
