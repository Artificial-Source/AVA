#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/command/private_group.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/search_tools.h"
#include "ava/tools/secure_workspace.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/mode.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

ava::permissions::PermissionRuleStore test_store(std::filesystem::path const& root)
{
  auto const workspace = root / "workspace";
  auto const config_dir = root / "config" / "ava";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(config_dir);
  static_cast<void>(::chmod(temp_root().c_str(), S_IRWXU));
  static_cast<void>(::chmod(root.c_str(), S_IRWXU));
  static_cast<void>(::chmod(workspace.c_str(), S_IRWXU));
  static_cast<void>(::chmod(config_dir.c_str(), S_IRWXU));
  auto anchors = ava::core::AnchorSet::open({workspace, config_dir});
  if (!anchors)
    throw std::runtime_error("failed to open permission rule test AnchorSet");
  return ava::permissions::PermissionRuleStore{.global_rules_file = config_dir / "permission-rules.json",
                                               .workspace_rules_file = workspace / ".ava" / "permission-rules.json",
                                               .workspace_dir = workspace,
                                               .anchor_set = *anchors};
}

void write_file_with_mode(std::filesystem::path const& path, std::string const& content, mode_t mode)
{
  std::filesystem::create_directories(path.parent_path());
  for (auto current = path.parent_path(); current != temp_root().parent_path(); current = current.parent_path())
  {
    static_cast<void>(::chmod(current.c_str(), S_IRWXU));
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << content;
  file.close();
  ::chmod(path.c_str(), mode);
}

bool owner_only_file(std::filesystem::path const& path)
{
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0)
    return false;
  return (st.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

ava::permissions::PermissionPrompt read_prompt(ava::permissions::PermissionRuleStore const& store, std::filesystem::path const& target)
{
  return ava::permissions::PermissionPrompt{.permission_request_id = "permreq_test",
                                            .operation = ava::permissions::Operation::ReadFile,
                                            .mode = ava::core::Mode::Build,
                                            .workspace_dir = store.workspace_dir,
                                            .target_path = target,
                                            .command = "",
                                            .tool_name = "read_file",
                                            .reason = "target is outside the workspace",
                                            .risk = ava::permissions::PermissionRisk::High};
}

void test_permission_rule_storage_add_list_remove()
{
  auto const root = create_empty_root("permission-rules-storage");

  auto const workspace = root / "workspace";
  auto const store = test_store(root);
  auto const outside = root / "outside.txt";
  static_cast<void>(::chmod(root.c_str(), S_IRWXU));

  auto added =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::ReadFile,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Any,
                                                                                                    .tool_name = "",
                                                                                                    .target_path = outside,
                                                                                                    .command = "",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "allow this exact file",
                                                                                                    .actor = "test"});
  expect(added.has_value() && added->rule_id.starts_with("permrule_"), "permission rule storage creates a stable rule id");
  auto const workspace_rules_file = ava::permissions::enforceable_permission_rules_file(store, ava::permissions::PermissionRuleScope::Workspace);
  expect(workspace_rules_file != store.workspace_rules_file && std::filesystem::exists(workspace_rules_file) && owner_only_file(workspace_rules_file),
         "permission rule storage writes an owner-only inspectable workspace file outside the workspace");

  auto loaded = ava::permissions::load_persistent_permission_rules(store);
  expect(loaded && loaded->size() == 1 && loaded->front().rule_id == added->rule_id && loaded->front().target_path == added->target_path,
         "permission rule storage loads persisted workspace rules");

  auto matched = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, outside));
  expect(matched && *matched && (*matched)->rule_id == added->rule_id && (*matched)->action == ava::permissions::PermissionAction::Allow,
         "permission rule matcher applies exact persisted path rules");

  auto removed = ava::permissions::remove_persistent_permission_rule(store, added->rule_id);
  expect(removed && removed->rule_id == added->rule_id, "permission rule removal returns the removed rule");
  auto after_remove = ava::permissions::load_persistent_permission_rules(store);
  expect(after_remove && after_remove->empty(), "permission rule removal persists an empty rules array");
}

void test_legacy_command_allows_are_removed_one_at_a_time()
{
  auto const root = temp_root() / "permission-rules-legacy-sequential-remove";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto legacy_allow = ava::permissions::PersistentPermissionRule{.rule_id = "permrule_legacy_first",
                                                                 .scope = ava::permissions::PermissionRuleScope::Global,
                                                                 .workspace_dir = {},
                                                                 .action = ava::permissions::PermissionAction::Allow,
                                                                 .operation = ava::permissions::Operation::RunCommand,
                                                                 .mode = ava::permissions::PermissionRuleMode::Build,
                                                                 .tool_name = "bash",
                                                                 .target_path = {},
                                                                 .command = "legacy first",
                                                                 .command_recipe_key = {},
                                                                 .recipe_display = {},
                                                                 .critical_acknowledged = false,
                                                                 .schema_version = ava::permissions::kLegacyPermissionRulesSchemaVersion,
                                                                 .reason = "legacy command allow",
                                                                 .actor = "test",
                                                                 .created_at = "2026-07-19T00:00:00Z"};
  auto second_legacy_allow = legacy_allow;
  second_legacy_allow.rule_id = "permrule_legacy_second";
  second_legacy_allow.command = "legacy second";
  write_file_with_mode(store.global_rules_file,
                       std::string("{\"schema_version\":1,\"rules\":[") + ava::permissions::permission_rule_json(legacy_allow) + "," +
                           ava::permissions::permission_rule_json(second_legacy_allow) + "]}",
                       S_IRUSR | S_IWUSR);

  auto first_removed = ava::permissions::remove_persistent_permission_rule(store, legacy_allow.rule_id);
  std::ifstream after_first_file(store.global_rules_file);
  std::string after_first((std::istreambuf_iterator<char>(after_first_file)), std::istreambuf_iterator<char>());
  auto add_while_legacy_remains =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Global,
                                                                                                    .action = ava::permissions::PermissionAction::Deny,
                                                                                                    .operation = ava::permissions::Operation::ReadFile,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Any,
                                                                                                    .tool_name = "",
                                                                                                    .target_path = root / "outside.txt",
                                                                                                    .command = "",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "must not rewrite legacy command allows",
                                                                                                    .actor = "test"});
  auto second_removed = ava::permissions::remove_persistent_permission_rule(store, second_legacy_allow.rule_id);
  std::ifstream after_second_file(store.global_rules_file);
  std::string after_second((std::istreambuf_iterator<char>(after_second_file)), std::istreambuf_iterator<char>());
  auto reloaded = ava::permissions::load_persistent_permission_rules(store);

  expect(
      first_removed && first_removed->rule_id == legacy_allow.rule_id && after_first.find("\"schema_version\":1") != std::string::npos &&
          after_first.find(second_legacy_allow.rule_id) != std::string::npos && !add_while_legacy_remains && second_removed &&
          second_removed->rule_id == second_legacy_allow.rule_id && after_second.find("\"schema_version\":2") != std::string::npos && reloaded &&
          reloaded->empty(),
      "legacy command Allows can be removed sequentially without rewriting the remaining v1 Allow, additions remain blocked, and final removal migrates to v2");
}

void test_permission_rule_precedence_denies_win()
{
  auto const root = create_empty_root("permission-rules-precedence");

  auto const workspace = root / "workspace";
  auto const store = test_store(root);
  auto const outside = root / "outside.txt";

  auto global_allow =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Global,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::ReadFile,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Any,
                                                                                                    .tool_name = "",
                                                                                                    .target_path = outside,
                                                                                                    .command = "",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "global allow",
                                                                                                    .actor = "test"});
  auto workspace_deny =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Deny,
                                                                                                    .operation = ava::permissions::Operation::ReadFile,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Any,
                                                                                                    .tool_name = "",
                                                                                                    .target_path = outside,
                                                                                                    .command = "",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "workspace deny",
                                                                                                    .actor = "test"});
  expect(global_allow && workspace_deny, "permission rule precedence test creates allow and deny rules");

  auto matched = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, outside));
  expect(matched && *matched && (*matched)->rule_id == workspace_deny->rule_id && (*matched)->action == ava::permissions::PermissionAction::Deny,
         "persistent deny rules take precedence over matching allows");
}

ava::permissions::PermissionPrompt command_prompt(ava::permissions::PermissionRuleStore const& store, std::string command)
{
  return ava::permissions::PermissionPrompt{.permission_request_id = "permreq_command",
                                            .operation = ava::permissions::Operation::RunCommand,
                                            .mode = ava::core::Mode::Build,
                                            .workspace_dir = store.workspace_dir,
                                            .target_path = {},
                                            .command = std::move(command),
                                            .tool_name = "bash",
                                            .reason = "command requires explicit approval",
                                            .risk = ava::permissions::PermissionRisk::High};
}

ava::permissions::CommandPermissionMetadata stable_command_metadata(std::string global_key, std::string workspace_key)
{
  ava::permissions::CommandPermissionMetadata metadata;
  metadata.level = ava::command::CommandLevel::Standard;
  metadata.family = ava::command::CommandFamily::CmakeBuild;
  metadata.containment_status = ava::permissions::CommandContainmentStatus::Available;
  metadata.backend_maximum_scope = ava::command::InteractiveScope::Workspace;
  metadata.global_recipe_key = std::move(global_key);
  metadata.workspace_recipe_key = std::move(workspace_key);
  metadata.recipe_display = "cmake-build: workspace:build";
  metadata.effective_allowed_scopes = {ava::command::InteractiveScope::Once, ava::command::InteractiveScope::Session,
                                       ava::command::InteractiveScope::Workspace};
  return metadata;
}

void test_schema_v2_recipe_rules_bind_scope_and_deny_precedence()
{
  auto const root = temp_root() / "permission-rules-stable-recipe";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace_a = root / "workspace-a";
  auto const workspace_b = root / "workspace-b";
  std::filesystem::create_directories(workspace_a);
  std::filesystem::create_directories(workspace_b);
  static_cast<void>(::chmod(root.c_str(), S_IRWXU));
  auto const global_file = root / "config" / "ava" / "permission-rules.json";
  auto const store_a = ava::permissions::PermissionRuleStore{
      .global_rules_file = global_file, .workspace_rules_file = workspace_a / ".ava" / "permission-rules.json", .workspace_dir = workspace_a};
  auto const store_b = ava::permissions::PermissionRuleStore{
      .global_rules_file = global_file, .workspace_rules_file = workspace_b / ".ava" / "permission-rules.json", .workspace_dir = workspace_b};
  auto prompt_a = command_prompt(store_a, "cmake --build build");
  auto prompt_b = command_prompt(store_b, "cmake --build build");
  auto const global_key = std::string("sha256:ava-command-recipe-v1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  auto const workspace_a_key = std::string("sha256:ava-command-workspace-recipe-v1:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  auto const workspace_b_key = std::string("sha256:ava-command-workspace-recipe-v1:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
  prompt_a.command_metadata = stable_command_metadata(global_key, workspace_a_key);
  prompt_b.command_metadata = stable_command_metadata(global_key, workspace_b_key);

  auto workspace_allow =
      ava::permissions::add_persistent_permission_rule(store_a, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                      .action = ava::permissions::PermissionAction::Allow,
                                                                                                      .operation = ava::permissions::Operation::RunCommand,
                                                                                                      .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                      .tool_name = "bash",
                                                                                                      .target_path = {},
                                                                                                      .command = "cmake --build ./build",
                                                                                                      .command_recipe_key = workspace_a_key,
                                                                                                      .recipe_display = "cmake-build: workspace:build",
                                                                                                      .critical_acknowledged = false,
                                                                                                      .reason = "remember exact workspace recipe",
                                                                                                      .actor = "test"});
  auto reloaded = ava::permissions::load_persistent_permission_rules(store_a);
  auto workspace_match = ava::permissions::match_persistent_permission_rule(store_a, prompt_a);
  auto cross_workspace_miss = ava::permissions::match_persistent_permission_rule(store_b, prompt_b);

  auto global_allow =
      ava::permissions::add_persistent_permission_rule(store_a, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Global,
                                                                                                      .action = ava::permissions::PermissionAction::Allow,
                                                                                                      .operation = ava::permissions::Operation::RunCommand,
                                                                                                      .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                      .tool_name = "bash",
                                                                                                      .target_path = {},
                                                                                                      .command = {},
                                                                                                      .command_recipe_key = global_key,
                                                                                                      .recipe_display = "cmake-build: workspace:build",
                                                                                                      .critical_acknowledged = false,
                                                                                                      .reason = "allow exact typed recipe across workspaces",
                                                                                                      .actor = "test"});
  auto global_match = ava::permissions::match_persistent_permission_rule(store_b, prompt_b);
  auto global_deny = ava::permissions::add_persistent_permission_rule(
      store_a, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Global,
                                                     .action = ava::permissions::PermissionAction::Deny,
                                                     .operation = ava::permissions::Operation::RunCommand,
                                                     .mode = ava::permissions::PermissionRuleMode::Build,
                                                     .tool_name = "bash",
                                                     .target_path = {},
                                                     .command = {},
                                                     .command_recipe_key = global_key,
                                                     .recipe_display = "cmake-build: workspace:build",
                                                     .critical_acknowledged = false,
                                                     .reason = "deny overrides standard auto-allow before execution",
                                                     .actor = "test"});
  auto denied = ava::permissions::match_persistent_permission_rule(store_b, prompt_b);

  expect(
      workspace_allow && reloaded && reloaded->size() == 1 && workspace_match && *workspace_match && (*workspace_match)->rule_id == workspace_allow->rule_id &&
          cross_workspace_miss && !*cross_workspace_miss && global_allow && global_match && *global_match &&
          (*global_match)->rule_id == global_allow->rule_id && global_deny && denied && *denied &&
          (*denied)->action == ava::permissions::PermissionAction::Deny && (*denied)->rule_id == global_deny->rule_id,
      "schema-v2 recipe rules match by typed workspace key rather than raw command spelling, global recipe rules span workspaces, and exact recipe denies win "
      "before Standard auto-allow");
}

void test_critical_command_allows_are_always_one_shot()
{
  auto const root = temp_root() / "permission-rules-critical-ack";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto prompt = command_prompt(store, "python -c 'print(1)'");
  ava::permissions::CommandPermissionMetadata critical;
  critical.level = ava::command::CommandLevel::Critical;
  critical.backend_maximum_scope = ava::command::InteractiveScope::Once;
  critical.containment_status = ava::permissions::CommandContainmentStatus::Available;
  prompt.command_metadata = critical;

  auto missing_ack =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::RunCommand,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "bash",
                                                                                                    .target_path = {},
                                                                                                    .command = prompt.command,
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "must fail without acknowledgement",
                                                                                                    .actor = "test"});
  auto acknowledged =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::RunCommand,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "bash",
                                                                                                    .target_path = {},
                                                                                                    .command = prompt.command,
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = true,
                                                                                                    .reason = "advanced exact critical allow",
                                                                                                    .actor = "test"});
  auto matched = ava::permissions::match_persistent_permission_rule(store, prompt);
  critical.executor_identity_verified = false;
  critical.containment_status = ava::permissions::CommandContainmentStatus::UnverifiedDelegatedExecutor;
  prompt.command_metadata = critical;
  auto unverified = ava::permissions::match_persistent_permission_rule(store, prompt);

  expect(!missing_ack && !acknowledged && acknowledged.error().category() == ava::core::ErrorCategory::InvalidArgument && matched && !*matched && unverified &&
             !*unverified,
         "Critical command Allows cannot be persisted with or without an acknowledgement and remain one-shot for every executor");
}

void test_critical_acknowledgements_cannot_authorize_repository_build_or_test_text()
{
  auto const root = temp_root() / "permission-rules-critical-ack-repository-build";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto const command = std::string("cmake --build build");
  auto created =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::RunCommand,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "bash",
                                                                                                    .target_path = {},
                                                                                                    .command = command,
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = true,
                                                                                                    .reason = "forged raw cmake acknowledgement",
                                                                                                    .actor = "test"});
  auto forged = ava::permissions::PersistentPermissionRule{.rule_id = "permrule_forged_cmake_ack",
                                                           .scope = ava::permissions::PermissionRuleScope::Global,
                                                           .workspace_dir = {},
                                                           .action = ava::permissions::PermissionAction::Allow,
                                                           .operation = ava::permissions::Operation::RunCommand,
                                                           .mode = ava::permissions::PermissionRuleMode::Build,
                                                           .tool_name = "bash",
                                                           .target_path = {},
                                                           .command = command,
                                                           .command_recipe_key = {},
                                                           .recipe_display = {},
                                                           .critical_acknowledged = true,
                                                           .schema_version = ava::permissions::kCurrentPermissionRulesSchemaVersion,
                                                           .reason = "forged persisted cmake acknowledgement",
                                                           .actor = "test",
                                                           .created_at = "2026-07-19T00:00:00Z"};
  write_file_with_mode(store.global_rules_file, std::string("{\"schema_version\":2,\"rules\":[") + ava::permissions::permission_rule_json(forged) + "]}",
                       S_IRUSR | S_IWUSR);
  auto prompt = command_prompt(store, command);
  ava::permissions::CommandPermissionMetadata critical;
  critical.level = ava::command::CommandLevel::Critical;
  critical.backend_maximum_scope = ava::command::InteractiveScope::Once;
  prompt.command_metadata = critical;
  auto matched = ava::permissions::match_persistent_permission_rule(store, prompt);
  auto ctest_created =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::RunCommand,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "bash",
                                                                                                    .target_path = {},
                                                                                                    .command = "ctest --test-dir build",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = true,
                                                                                                    .reason = "forged raw ctest acknowledgement",
                                                                                                    .actor = "test"});
  auto forged_ctest = forged;
  forged_ctest.rule_id = "permrule_forged_ctest_ack";
  forged_ctest.command = "ctest --test-dir build";
  forged_ctest.reason = "forged persisted ctest acknowledgement";
  write_file_with_mode(store.global_rules_file, std::string("{\"schema_version\":2,\"rules\":[") + ava::permissions::permission_rule_json(forged_ctest) + "]}",
                       S_IRUSR | S_IWUSR);
  auto ctest_prompt = command_prompt(store, forged_ctest.command);
  ctest_prompt.command_metadata = critical;
  auto ctest_matched = ava::permissions::match_persistent_permission_rule(store, ctest_prompt);

  expect(!created && created.error().category() == ava::core::ErrorCategory::InvalidArgument && !matched && !ctest_created &&
             ctest_created.error().category() == ava::core::ErrorCategory::InvalidArgument && !ctest_matched,
         "raw critical acknowledgements for repository-controlled cmake build and ctest text are rejected both on creation and during persisted-rule matching");
}

void test_permission_rule_precedence_prefers_specific_same_scope_rules()
{
  auto const root = create_empty_root("permission-rules-specificity");

  auto const store = test_store(root);
  auto const outside = root / "outside.txt";

  auto broad_allow =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::ReadFile,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Any,
                                                                                                    .tool_name = "",
                                                                                                    .target_path = outside,
                                                                                                    .command = "",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "allow this path",
                                                                                                    .actor = "test"});
  auto specific_deny =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Deny,
                                                                                                    .operation = ava::permissions::Operation::ReadFile,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Any,
                                                                                                    .tool_name = "read_file",
                                                                                                    .target_path = outside,
                                                                                                    .command = "",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "deny this tool/path pair",
                                                                                                    .actor = "test"});
  expect(broad_allow && specific_deny, "permission rule specificity test creates broad and specific rules");

  auto matched = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, outside));
  expect(matched && *matched && (*matched)->rule_id == specific_deny->rule_id && (*matched)->action == ava::permissions::PermissionAction::Deny,
         "persistent permission rules prefer more specific same-scope matches");
}

void test_permission_rule_matches_command_operations_without_path_targets()
{
  auto const root = create_empty_root("permission-rules-command-operation");

  auto const store = test_store(root);

  auto allow_echo =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::RunCommand,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "bash",
                                                                                                    .target_path = {},
                                                                                                    .command = "echo safe",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "allow exact verification command",
                                                                                                    .actor = "test"});
  expect(!allow_echo && allow_echo.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "schema-v2 raw command Allows are rejected without a stable recipe key or explicit critical acknowledgement");

  // Schema-v1 command Allows predate sealed recipe identities and remain
  // non-authoritative; exact Denies remain authoritative.
  auto matched = ava::permissions::match_persistent_permission_rule(store, command_prompt(store, "echo safe"));
  expect(matched && !*matched, "legacy v1 command Allow rules are ignored for one-shot command plans");

  auto deny_echo =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Deny,
                                                                                                    .operation = ava::permissions::Operation::RunCommand,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "bash",
                                                                                                    .target_path = {},
                                                                                                    .command = "echo safe",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "deny exact verification command",
                                                                                                    .actor = "test"});
  expect(deny_echo.has_value(), "permission rule storage accepts exact command deny rules");
  auto matched_deny = ava::permissions::match_persistent_permission_rule(store, command_prompt(store, "echo safe"));
  expect(matched_deny && *matched_deny && (*matched_deny)->rule_id == deny_echo->rule_id && (*matched_deny)->action == ava::permissions::PermissionAction::Deny,
         "legacy v1 command Deny rules remain authoritative for one-shot command plans");

  auto unmatched = ava::permissions::match_persistent_permission_rule(store, command_prompt(store, "echo unsafe"));
  expect(unmatched && !*unmatched, "non-matching command prompts produce no persistent rule match");
}

void test_repository_build_test_persistent_allows_are_rejected_but_denies_win()
{
  auto const root = create_empty_root("permission-rules-build-test");

  auto const store = test_store(root);

  for (auto const& command : std::array{"ctest --test-dir build", "cmake --build=build", "cmake --build-and-test source build --build-generator Ninja",
                                        "cmake --workflow --preset=ci", "/usr/bin/ctest --test-dir build", "/usr/bin/cmake --build build"})
  {
    auto const prompt = command_prompt(store, command);
    auto persistent_allow =
        ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                      .action = ava::permissions::PermissionAction::Allow,
                                                                                                      .operation = ava::permissions::Operation::RunCommand,
                                                                                                      .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                      .tool_name = "bash",
                                                                                                      .target_path = {},
                                                                                                      .command = prompt.command,
                                                                                                      .command_recipe_key = {},
                                                                                                      .recipe_display = {},
                                                                                                      .critical_acknowledged = false,
                                                                                                      .reason = "never persist build test approval",
                                                                                                      .actor = "test"});
    expect(!persistent_allow && persistent_allow.error().category() == ava::core::ErrorCategory::InvalidArgument,
           "persistent rules reject repository build/test allow command shapes");

    ava::permissions::PersistentPermissionRule const legacy_allow{.rule_id = "permrule_legacy_build_test",
                                                                  .scope = ava::permissions::PermissionRuleScope::Global,
                                                                  .workspace_dir = {},
                                                                  .action = ava::permissions::PermissionAction::Allow,
                                                                  .operation = ava::permissions::Operation::RunCommand,
                                                                  .mode = ava::permissions::PermissionRuleMode::Build,
                                                                  .tool_name = "bash",
                                                                  .target_path = {},
                                                                  .command = prompt.command,
                                                                  .command_recipe_key = {},
                                                                  .recipe_display = {},
                                                                  .critical_acknowledged = false,
                                                                  .reason = "legacy repository test allow",
                                                                  .actor = "test",
                                                                  .created_at = "2026-07-11T00:00:00Z"};
    write_file_with_mode(store.global_rules_file,
                         std::string("{\"schema_version\":1,\"rules\":[") + ava::permissions::permission_rule_json(legacy_allow) + "]}", S_IRUSR | S_IWUSR);
    auto legacy_match = ava::permissions::match_persistent_permission_rule(store, prompt);
    expect(legacy_match && !*legacy_match, "legacy persistent repository build/test allows are ignored rather than auto-executed");
  }

  auto const prompt = command_prompt(store, "ctest --test-dir build");
  auto persistent_deny =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Deny,
                                                                                                    .operation = ava::permissions::Operation::RunCommand,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "bash",
                                                                                                    .target_path = {},
                                                                                                    .command = prompt.command,
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "deny repository test",
                                                                                                    .actor = "test"});
  auto matched = ava::permissions::match_persistent_permission_rule(store, prompt);
  expect(persistent_deny && matched && *matched && (*matched)->action == ava::permissions::PermissionAction::Deny,
         "persistent deny rules continue to override repository build and test approvals");
}

void test_legacy_command_allows_do_not_authorize_sealed_critical_or_unverified_plans()
{
  auto const root = temp_root() / "permission-rules-sealed-command-v1";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto prompt = command_prompt(store, "python -c 'print(1)'");
  ava::permissions::PersistentPermissionRule const legacy_allow{.rule_id = "permrule_legacy_command_allow",
                                                                .scope = ava::permissions::PermissionRuleScope::Global,
                                                                .workspace_dir = {},
                                                                .action = ava::permissions::PermissionAction::Allow,
                                                                .operation = ava::permissions::Operation::RunCommand,
                                                                .mode = ava::permissions::PermissionRuleMode::Build,
                                                                .tool_name = "bash",
                                                                .target_path = {},
                                                                .command = prompt.command,
                                                                .command_recipe_key = {},
                                                                .recipe_display = {},
                                                                .critical_acknowledged = false,
                                                                .reason = "legacy exact command allow",
                                                                .actor = "test",
                                                                .created_at = "2026-07-19T00:00:00Z"};
  ava::permissions::PersistentPermissionRule legacy_deny = legacy_allow;
  legacy_deny.rule_id = "permrule_legacy_command_deny";
  legacy_deny.action = ava::permissions::PermissionAction::Deny;
  legacy_deny.reason = "legacy exact command deny";

  auto verify_legacy_allow_blocked = [&](ava::permissions::CommandPermissionMetadata metadata, std::string_view label) {
    prompt.command_metadata = std::move(metadata);
    write_file_with_mode(store.global_rules_file,
                         std::string("{\"schema_version\":1,\"rules\":[") + ava::permissions::permission_rule_json(legacy_allow) + "]}", S_IRUSR | S_IWUSR);
    auto ignored_allow = ava::permissions::match_persistent_permission_rule(store, prompt);
    expect(ignored_allow && !*ignored_allow, std::string("legacy v1 exact Allow cannot authorize ") + std::string(label) + " sealed command plans");

    write_file_with_mode(store.global_rules_file,
                         std::string("{\"schema_version\":1,\"rules\":[") + ava::permissions::permission_rule_json(legacy_allow) + "," +
                             ava::permissions::permission_rule_json(legacy_deny) + "]}",
                         S_IRUSR | S_IWUSR);
    auto matched_deny = ava::permissions::match_persistent_permission_rule(store, prompt);
    expect(matched_deny && *matched_deny && (*matched_deny)->rule_id == legacy_deny.rule_id,
           std::string("legacy v1 exact Deny remains authoritative for ") + std::string(label) + " sealed command plans");
  };

  ava::permissions::CommandPermissionMetadata critical;
  critical.level = ava::command::CommandLevel::Critical;
  critical.backend_maximum_scope = ava::command::InteractiveScope::Once;
  verify_legacy_allow_blocked(critical, "critical");

  ava::permissions::CommandPermissionMetadata unverified;
  unverified.level = ava::command::CommandLevel::Critical;
  unverified.backend_maximum_scope = ava::command::InteractiveScope::Once;
  unverified.executor_identity_verified = false;
  unverified.containment_status = ava::permissions::CommandContainmentStatus::UnverifiedDelegatedExecutor;
  verify_legacy_allow_blocked(unverified, "unverified delegated");
}

void test_old_critical_acknowledgements_never_recover_authority()
{
  auto const root = temp_root() / "permission-rules-critical-unavailable";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto const command = std::string("curl https://example.test/releases");
  auto rejected = ava::permissions::add_persistent_permission_rule(
      store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                   .action = ava::permissions::PermissionAction::Allow,
                                                   .operation = ava::permissions::Operation::RunCommand,
                                                   .mode = ava::permissions::PermissionRuleMode::Build,
                                                   .tool_name = "bash",
                                                   .target_path = {},
                                                   .command = command,
                                                   .command_recipe_key = {},
                                                   .recipe_display = {},
                                                   .critical_acknowledged = true,
                                                   .reason = "exact critical allow for uncontained network command",
                                                   .actor = "test"});
  auto old_rule = ava::permissions::PersistentPermissionRule{.rule_id = "permrule_old_critical_ack",
                                                             .scope = ava::permissions::PermissionRuleScope::Workspace,
                                                             .workspace_dir = store.workspace_dir,
                                                             .action = ava::permissions::PermissionAction::Allow,
                                                             .operation = ava::permissions::Operation::RunCommand,
                                                             .mode = ava::permissions::PermissionRuleMode::Build,
                                                             .tool_name = "bash",
                                                             .target_path = {},
                                                             .command = command,
                                                             .command_recipe_key = {},
                                                             .recipe_display = {},
                                                             .critical_acknowledged = true,
                                                             .schema_version = ava::permissions::kCurrentPermissionRulesSchemaVersion,
                                                             .reason = "old exact critical allow",
                                                             .actor = "test",
                                                             .created_at = "2026-07-19T00:00:00Z"};
  std::filesystem::create_directories(store.workspace_rules_file.parent_path());
  write_file_with_mode(store.workspace_rules_file, std::string("{\"schema_version\":2,\"rules\":[") + ava::permissions::permission_rule_json(old_rule) + "]}",
                       S_IRUSR | S_IWUSR);
  auto prompt = command_prompt(store, command);

  ava::permissions::CommandPermissionMetadata unavailable;
  unavailable.level = ava::command::CommandLevel::Critical;
  unavailable.backend_maximum_scope = ava::command::InteractiveScope::Once;
  unavailable.executor_identity_verified = true;
  unavailable.containment_status = ava::permissions::CommandContainmentStatus::Unavailable;
  prompt.command_metadata = unavailable;
  auto unavailable_match = ava::permissions::match_persistent_permission_rule(store, prompt);

  auto available = unavailable;
  available.containment_status = ava::permissions::CommandContainmentStatus::Available;
  prompt.command_metadata = available;
  auto available_match = ava::permissions::match_persistent_permission_rule(store, prompt);

  auto unverified = unavailable;
  unverified.executor_identity_verified = false;
  unverified.containment_status = ava::permissions::CommandContainmentStatus::UnverifiedDelegatedExecutor;
  prompt.command_metadata = unverified;
  auto unverified_match = ava::permissions::match_persistent_permission_rule(store, prompt);
  expect(!rejected && rejected.error().category() == ava::core::ErrorCategory::InvalidArgument && unavailable_match && !*unavailable_match && available_match &&
             !*available_match && unverified_match && !*unverified_match,
         "new and old exact Critical acknowledgements never recover authority, regardless of containment or executor status");
}

void test_permission_rule_storage_fail_closed()
{
  auto const root = create_empty_root("permission-rules-corrupt");

  auto const store = test_store(root);
  write_file_with_mode(store.global_rules_file, "{\"schema_version\":3,\"rules\":[]}", S_IRUSR | S_IWUSR);

  auto loaded = ava::permissions::load_persistent_permission_rules(store);
  expect(!loaded && loaded.error().message() == "unsupported permission rules schema_version", "permission rule storage rejects unsupported schema versions");

  bool fallback_called = false;
  auto resolver = ava::permissions::build_persistent_permission_rule_resolver(
      store, [&](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        fallback_called = true;
        return ava::permissions::PermissionResolution::Allow;
      });
  auto decision = resolver(read_prompt(store, root / "outside.txt"));
  expect(decision && decision->resolution == ava::permissions::PermissionResolution::Deny && decision->resolution_source == "persistent_rule_error" &&
             decision->authoritative && !fallback_called,
         "malformed persistent rule storage fails closed before resolver fallback");

  write_file_with_mode(store.global_rules_file, "{\"schema_version\":2,\"rules\":[],\"unexpected\":true}", S_IRUSR | S_IWUSR);
  auto unknown_v2_member = ava::permissions::load_persistent_permission_rules(store);
  expect(!unknown_v2_member && unknown_v2_member.error().message() == "schema-v2 permission rules file has unsupported member",
         "strict bounded schema-v2 rule storage rejects unknown members rather than silently broadening policy");
}

void test_permission_rule_broad_permissions_rejected()
{
  auto const root = create_empty_root("permission-rules-broad");

  auto const store = test_store(root);
  write_file_with_mode(store.global_rules_file, "{\"schema_version\":1,\"rules\":[]}", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  auto loaded = ava::permissions::load_persistent_permission_rules(store);
  expect(!loaded && loaded.error().category() == ava::core::ErrorCategory::PermissionDenied, "permission rule storage rejects group/world-readable rule files");
}

ava::core::Result<ava::permissions::PersistentPermissionRule> add_exact_workspace_deny(ava::permissions::PermissionRuleStore const& store,
                                                                                       ava::permissions::Operation operation,
                                                                                       std::filesystem::path const& target_path, std::string tool_name,
                                                                                       std::string reason)
{
  return ava::permissions::add_persistent_permission_rule(store,
                                                          ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                .action = ava::permissions::PermissionAction::Deny,
                                                                                                .operation = operation,
                                                                                                .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                .tool_name = std::move(tool_name),
                                                                                                .target_path = std::move(target_path),
                                                                                                .command = "",
                                                                                                .command_recipe_key = {},
                                                                                                .recipe_display = {},
                                                                                                .critical_acknowledged = false,
                                                                                                .reason = std::move(reason),
                                                                                                .actor = "test"});
}

ava::tools::ToolContext persistent_rule_tool_context(ava::permissions::PermissionRuleStore const& store, int& fallback_prompts,
                                                     std::vector<ava::tools::PermissionAuditEvent>& audits)
{
  auto fallback = [&fallback_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    ++fallback_prompts;
    return ava::permissions::PermissionResolution::Allow;
  };
  return ava::tools::ToolContext{
      .workspace_dir = store.workspace_dir,
      .mode = ava::core::Mode::Build,
      .permission_resolver = ava::permissions::build_persistent_permission_rule_resolver(store, std::move(fallback)),
      .auto_allow_deny_preflight = ava::permissions::build_persistent_permission_deny_preflight(store),
      .permission_audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        audits.push_back(event);
        return {};
      },
  };
}

void test_deny_path_matching_follows_physical_aliases_without_broadening_allow()
{
  auto const root = create_empty_root("permission-rules-physical-matcher");
  auto const store = test_store(root);
  auto const real_dir = store.workspace_dir / "real";
  auto const alias_dir = store.workspace_dir / "alias";
  auto const denied_path = real_dir / "secret.txt";
  auto const allowed_path = real_dir / "allowed.txt";
  auto const unrelated_path = real_dir / "unrelated.txt";
  auto const final_alias = store.workspace_dir / "final-secret.txt";
  auto const denied_hardlink = store.workspace_dir / "secret-hardlink.txt";
  auto const allowed_hardlink = store.workspace_dir / "allowed-hardlink.txt";
  std::filesystem::create_directories(real_dir);
  write_file_with_mode(denied_path, "matcher denied bytes\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(allowed_path, "matcher allowed bytes\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(unrelated_path, "matcher unrelated bytes\n", S_IRUSR | S_IWUSR);
  std::error_code fixture_error;
  std::filesystem::create_directory_symlink("real", alias_dir, fixture_error);
  if (!fixture_error)
    std::filesystem::create_symlink("real/secret.txt", final_alias, fixture_error);
  if (!fixture_error)
    std::filesystem::create_hard_link(denied_path, denied_hardlink, fixture_error);
  if (!fixture_error)
    std::filesystem::create_hard_link(allowed_path, allowed_hardlink, fixture_error);
  expect(!fixture_error, "physical permission matcher creates contained symlink and hardlink aliases");
  if (fixture_error)
    return;

  auto read_deny = add_exact_workspace_deny(store, ava::permissions::Operation::ReadFile, denied_path, "read_file", "deny physical read identity");
  auto search_deny = add_exact_workspace_deny(store, ava::permissions::Operation::SearchFiles, real_dir, "list_directory", "deny physical directory identity");
  auto missing_deny =
      add_exact_workspace_deny(store, ava::permissions::Operation::EditFile, real_dir / "missing.txt", "write_file", "deny missing physical write identity");
  auto read_allow =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::ReadFile,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "read_file",
                                                                                                    .target_path = allowed_path,
                                                                                                    .command = "",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "allow lexical path only",
                                                                                                    .actor = "test"});
  expect(read_deny && search_deny && missing_deny && read_allow, "physical matcher fixture writes exact persistent rules");
  if (!read_deny || !search_deny || !missing_deny || !read_allow)
    return;

  std::array<std::filesystem::path, 3> const denied_aliases{alias_dir / "secret.txt", final_alias, denied_hardlink};
  for (auto const& alias : denied_aliases)
  {
    auto prompt = read_prompt(store, alias);
    auto matched = ava::permissions::match_persistent_permission_rule(store, prompt);
    expect(matched && *matched && (*matched)->rule_id == read_deny->rule_id && (*matched)->action == ava::permissions::PermissionAction::Deny,
           "exact persistent Deny matches intermediate, final symlink, and hardlink file identities");
  }

  auto list_prompt = ava::permissions::PermissionPrompt{.permission_request_id = "permreq_physical_list",
                                                        .operation = ava::permissions::Operation::SearchFiles,
                                                        .mode = ava::core::Mode::Build,
                                                        .workspace_dir = store.workspace_dir,
                                                        .target_path = alias_dir,
                                                        .command = "",
                                                        .tool_name = "list_directory",
                                                        .reason = "tool requires permission",
                                                        .risk = ava::permissions::PermissionRisk::Low};
  auto list_match = ava::permissions::match_persistent_permission_rule(store, list_prompt);
  auto missing_prompt = ava::permissions::PermissionPrompt{.permission_request_id = "permreq_physical_missing",
                                                           .operation = ava::permissions::Operation::EditFile,
                                                           .mode = ava::core::Mode::Build,
                                                           .workspace_dir = store.workspace_dir,
                                                           .target_path = alias_dir / "missing.txt",
                                                           .command = "",
                                                           .tool_name = "write_file",
                                                           .reason = "tool requires permission",
                                                           .risk = ava::permissions::PermissionRisk::Medium};
  auto missing_match = ava::permissions::match_persistent_permission_rule(store, missing_prompt);
  auto allow_alias_match = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, alias_dir / "allowed.txt"));
  auto allow_hardlink_match = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, allowed_hardlink));
  auto unrelated_match = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, unrelated_path));

  expect(list_match && *list_match && (*list_match)->rule_id == search_deny->rule_id && missing_match && *missing_match &&
             (*missing_match)->rule_id == missing_deny->rule_id,
         "persistent Deny matches a list root and missing write target beneath an aliased existing parent");
  expect(allow_alias_match && !*allow_alias_match && allow_hardlink_match && !*allow_hardlink_match && unrelated_match && !*unrelated_match,
         "persistent Allow and unrelated paths remain tied to exact lexical authority rather than physical identity");

  auto const loop_a = store.workspace_dir / "loop-a";
  auto const loop_b = store.workspace_dir / "loop-b";
  fixture_error.clear();
  std::filesystem::create_symlink("loop-b", loop_a, fixture_error);
  if (!fixture_error)
    std::filesystem::create_symlink("loop-a", loop_b, fixture_error);
  expect(!fixture_error, "physical matcher creates an unresolvable identity fixture");
  if (fixture_error)
    return;
  auto unresolved_prompt = read_prompt(store, loop_a);
  auto unresolved_match = ava::permissions::match_persistent_permission_rule(store, unresolved_prompt);
  int fallback_prompts = 0;
  auto resolver = ava::permissions::build_persistent_permission_rule_resolver(
      store, [&fallback_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++fallback_prompts;
        return ava::permissions::PermissionResolution::Allow;
      });
  auto unresolved_resolution = resolver(unresolved_prompt);
  expect(!unresolved_match && unresolved_match.error().message() == "failed to establish permission path identity" && unresolved_resolution &&
             *unresolved_resolution == ava::permissions::PermissionResolution::Deny && unresolved_resolution->authoritative &&
             unresolved_resolution->resolution_source == "persistent_rule_error" && fallback_prompts == 0,
         "a required Deny identity comparison error is explicit and fails closed without turning valid unrelated path misses into Deny");
}

void test_file_and_search_tools_enforce_denies_across_physical_aliases()
{
  auto const root = create_empty_root("permission-rules-physical-tools");
  auto const store = test_store(root);
  auto const real_dir = store.workspace_dir / "real";
  auto const alias_dir = store.workspace_dir / "alias";
  auto const secret_path = real_dir / "denied.txt";
  auto const edit_path = real_dir / "edit.txt";
  auto const final_edit_path = real_dir / "final-edit.txt";
  auto const hardlink_edit_path = real_dir / "hardlink-edit.txt";
  auto const visible_path = real_dir / "visible.txt";
  auto const final_secret_alias = store.workspace_dir / "final-denied.txt";
  auto const final_edit_alias = store.workspace_dir / "final-edit-alias.txt";
  auto const secret_hardlink = store.workspace_dir / "denied-hardlink.txt";
  auto const edit_hardlink = store.workspace_dir / "edit-hardlink.txt";
  std::filesystem::create_directories(real_dir);
  write_file_with_mode(secret_path, "DENIED_ALIAS_BYTES search-marker\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(edit_path, "EDIT_ALIAS_OLD\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(final_edit_path, "FINAL_EDIT_ALIAS_OLD\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(hardlink_edit_path, "HARDLINK_EDIT_OLD\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(visible_path, "visible search-marker\n", S_IRUSR | S_IWUSR);
  std::error_code fixture_error;
  std::filesystem::create_directory_symlink("real", alias_dir, fixture_error);
  if (!fixture_error)
    std::filesystem::create_symlink("real/denied.txt", final_secret_alias, fixture_error);
  if (!fixture_error)
    std::filesystem::create_symlink("real/final-edit.txt", final_edit_alias, fixture_error);
  if (!fixture_error)
    std::filesystem::create_hard_link(secret_path, secret_hardlink, fixture_error);
  if (!fixture_error)
    std::filesystem::create_hard_link(hardlink_edit_path, edit_hardlink, fixture_error);
  expect(!fixture_error, "physical tool fixture creates contained symlink and hardlink aliases");
  if (fixture_error)
    return;

  auto read_deny = add_exact_workspace_deny(store, ava::permissions::Operation::ReadFile, secret_path, "", "deny aliased secret reads");
  auto list_deny = add_exact_workspace_deny(store, ava::permissions::Operation::SearchFiles, real_dir, "list_directory", "deny aliased directory listing");
  auto edit_deny = add_exact_workspace_deny(store, ava::permissions::Operation::EditFile, edit_path, "", "deny aliased existing edit");
  auto final_edit_deny = add_exact_workspace_deny(store, ava::permissions::Operation::EditFile, final_edit_path, "edit_file", "deny final symlink edit");
  auto hardlink_edit_deny = add_exact_workspace_deny(store, ava::permissions::Operation::EditFile, hardlink_edit_path, "write_file", "deny hardlink write");
  auto missing_edit_deny =
      add_exact_workspace_deny(store, ava::permissions::Operation::EditFile, real_dir / "missing.txt", "write_file", "deny aliased missing write");
  expect(read_deny && list_deny && edit_deny && final_edit_deny && hardlink_edit_deny && missing_edit_deny,
         "physical tool fixture writes exact persistent denies");
  if (!read_deny || !list_deny || !edit_deny || !final_edit_deny || !hardlink_edit_deny || !missing_edit_deny)
    return;

  auto secure_workspace = ava::tools::SecureWorkspace::open(store.workspace_dir);
  expect(secure_workspace.has_value(), "physical tool fixture opens descriptor-secure workspace");
  if (!secure_workspace)
    return;
  int fallback_prompts = 0;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto context = persistent_rule_tool_context(store, fallback_prompts, audits);
  context.secure_workspace = *secure_workspace;

  context.permission_tool_name = "read_file";
  auto intermediate_read = ava::tools::read_file(context, alias_dir / "denied.txt");
  auto final_read = ava::tools::read_file(context, final_secret_alias);
  auto hardlink_read = ava::tools::read_file(context, secret_hardlink);
  context.permission_tool_name = "list_directory";
  auto listed = ava::tools::list_directory(context, alias_dir);
  context.permission_tool_name = "glob";
  auto globbed = ava::tools::glob_files(context, "**/*.txt");
  context.permission_tool_name = "grep";
  auto grepped = ava::tools::grep_files(context, "search-marker", "**/*.txt");
  context.permission_tool_name = "write_file";
  auto written = ava::tools::write_file(context, alias_dir / "edit.txt", "EDIT_ALIAS_REPLACEMENT\n");
  context.permission_tool_name = "edit_file";
  auto edited = ava::tools::edit_file(context, alias_dir / "edit.txt", "EDIT_ALIAS_OLD", "EDIT_ALIAS_REPLACEMENT");
  auto final_edited = ava::tools::edit_file(context, final_edit_alias, "FINAL_EDIT_ALIAS_OLD", "FINAL_EDIT_ALIAS_REPLACEMENT");
  context.permission_tool_name = "write_file";
  auto hardlink_written = ava::tools::write_file(context, edit_hardlink, "HARDLINK_EDIT_REPLACEMENT\n");
  auto missing_written = ava::tools::write_file(context, alias_dir / "missing.txt", "MISSING_ALIAS_REPLACEMENT\n");

  auto const read_text = [](std::filesystem::path const& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  };
  bool const reads_do_not_leak = !intermediate_read && !final_read && !hardlink_read &&
                                 intermediate_read.error().format().find("DENIED_ALIAS_BYTES") == std::string::npos &&
                                 final_read.error().format().find("DENIED_ALIAS_BYTES") == std::string::npos &&
                                 hardlink_read.error().format().find("DENIED_ALIAS_BYTES") == std::string::npos;
  bool const list_does_not_leak =
      !listed && listed.error().format().find("denied.txt") == std::string::npos && listed.error().format().find("DENIED_ALIAS_BYTES") == std::string::npos;
  bool const searches_filter_aliases =
      globbed && grepped && std::ranges::find(globbed->paths, visible_path) != globbed->paths.end() &&
      std::ranges::find(globbed->paths, secret_path) == globbed->paths.end() && std::ranges::find(globbed->paths, secret_hardlink) == globbed->paths.end() &&
      std::ranges::none_of(grepped->matches, [](ava::tools::GrepMatch const& match) { return match.line.find("DENIED_ALIAS_BYTES") != std::string::npos; }) &&
      std::ranges::any_of(grepped->matches, [&visible_path](ava::tools::GrepMatch const& match) { return match.path == visible_path; });
  bool const mutations_denied_without_diff =
      !written && !edited && !final_edited && !hardlink_written && !missing_written && written.error().format().find("EDIT_ALIAS_OLD") == std::string::npos &&
      edited.error().format().find("EDIT_ALIAS_OLD") == std::string::npos && final_edited.error().format().find("FINAL_EDIT_ALIAS_OLD") == std::string::npos &&
      hardlink_written.error().format().find("HARDLINK_EDIT_OLD") == std::string::npos &&
      missing_written.error().format().find("MISSING_ALIAS_REPLACEMENT") == std::string::npos && read_text(edit_path) == "EDIT_ALIAS_OLD\n" &&
      read_text(final_edit_path) == "FINAL_EDIT_ALIAS_OLD\n" && read_text(hardlink_edit_path) == "HARDLINK_EDIT_OLD\n" &&
      read_text(edit_hardlink) == "HARDLINK_EDIT_OLD\n" && !std::filesystem::exists(real_dir / "missing.txt");

  std::array<std::string, 6> const expected_rule_ids{read_deny->rule_id,       list_deny->rule_id,          edit_deny->rule_id,
                                                     final_edit_deny->rule_id, hardlink_edit_deny->rule_id, missing_edit_deny->rule_id};
  bool const each_rule_audited = std::ranges::all_of(expected_rule_ids, [&audits](std::string const& rule_id) {
    return std::ranges::any_of(audits, [&rule_id](ava::tools::PermissionAuditEvent const& event) {
      return event.action == ava::permissions::PermissionAction::Deny && event.resolution == "deny" && event.resolution_source == "persistent_rule" &&
             event.rule_id == rule_id;
    });
  });
  bool const deny_audits_are_authoritative = std::ranges::none_of(audits, [](ava::tools::PermissionAuditEvent const& event) {
    return event.action == ava::permissions::PermissionAction::Deny &&
           (event.resolution != "deny" || event.resolution_source != "persistent_rule" || event.rule_id.empty());
  });

  expect(reads_do_not_leak && list_does_not_leak && searches_filter_aliases && mutations_denied_without_diff && fallback_prompts == 0 && each_rule_audited &&
             deny_audits_are_authoritative,
         "read, list, glob, grep, write, and edit enforce physical Denies without bytes, names, diffs, fallback resolution, or side effects");
}

void test_exact_workspace_root_deny_matches_list_directory_spellings()
{
  auto const root = create_empty_root("permission-rules-root-directory-spellings");
  auto const store = test_store(root);
  auto const canary_path = store.workspace_dir / "canary.txt";
  std::filesystem::create_directories(store.workspace_dir / "nested");
  write_file_with_mode(canary_path, "ROOT_LIST_PROVIDER_CANARY\n", S_IRUSR | S_IWUSR);

  auto deny = add_exact_workspace_deny(store, ava::permissions::Operation::SearchFiles, store.workspace_dir, "list_directory", "deny exact workspace listing");
  expect(deny.has_value(), "workspace root list_directory spelling fixture writes an exact persistent deny");
  if (!deny)
    return;

  std::array<std::filesystem::path, 3> const spellings{store.workspace_dir / ".", std::filesystem::path(store.workspace_dir.string() + "/"),
                                                       store.workspace_dir / "nested" / ".." / "."};
  for (auto const& spelling : spellings)
  {
    auto matched =
        ava::permissions::match_persistent_permission_rule(store, ava::permissions::PermissionPrompt{.permission_request_id = "permreq_root_list_spelling",
                                                                                                     .operation = ava::permissions::Operation::SearchFiles,
                                                                                                     .mode = ava::core::Mode::Build,
                                                                                                     .workspace_dir = store.workspace_dir,
                                                                                                     .target_path = spelling,
                                                                                                     .command = "",
                                                                                                     .tool_name = "list_directory",
                                                                                                     .reason = "tool requires permission",
                                                                                                     .risk = ava::permissions::PermissionRisk::Low});
    expect(matched && *matched && (*matched)->rule_id == deny->rule_id,
           "exact workspace root rule matcher treats list_directory dot and trailing directory spellings identically");
  }

  int fallback_prompts = 0;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto context = persistent_rule_tool_context(store, fallback_prompts, audits);
  context.permission_tool_name = "list_directory";
  for (auto const& spelling : spellings)
  {
    auto listed = ava::tools::list_directory(context, spelling);
    expect(!listed && listed.error().format().find("ROOT_LIST_PROVIDER_CANARY") == std::string::npos,
           "exact workspace root deny blocks list_directory directory spelling without leaking content");
  }
  bool const exact_deny_audits = audits.size() == spellings.size() && std::ranges::all_of(audits, [&](ava::tools::PermissionAuditEvent const& event) {
                                   return event.tool_name == "list_directory" && event.action == ava::permissions::PermissionAction::Deny &&
                                          event.resolution == "deny" && event.resolution_source == "persistent_rule" && event.rule_id == deny->rule_id;
                                 });
  expect(fallback_prompts == 0 && exact_deny_audits, "equivalent workspace root list_directory denials remain authoritative and audit the matching rule id");
}

void test_policy_allowed_file_and_search_tools_honor_exact_persistent_denies()
{
  auto const root = create_empty_root("permission-rules-policy-allow-denies");
  auto const store = test_store(root);
  auto const read_path = store.workspace_dir / "read-canary.txt";
  auto const edit_path = store.workspace_dir / "edit-canary.txt";
  write_file_with_mode(read_path, "READ_PROVIDER_CANARY\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(edit_path, "EDIT_ORIGINAL_CANARY\n", S_IRUSR | S_IWUSR);

  auto read_deny = add_exact_workspace_deny(store, ava::permissions::Operation::ReadFile, read_path, "read_file", "deny exact read_file");
  auto list_deny =
      add_exact_workspace_deny(store, ava::permissions::Operation::SearchFiles, store.workspace_dir, "list_directory", "deny exact list_directory");
  auto glob_deny = add_exact_workspace_deny(store, ava::permissions::Operation::SearchFiles, store.workspace_dir, "glob", "deny exact glob");
  auto grep_deny = add_exact_workspace_deny(store, ava::permissions::Operation::SearchFiles, store.workspace_dir, "grep", "deny exact grep");
  auto edit_deny = add_exact_workspace_deny(store, ava::permissions::Operation::EditFile, edit_path, "write_file", "deny exact write_file");
  expect(read_deny && list_deny && glob_deny && grep_deny && edit_deny, "policy-Allow deny fixture writes exact persistent rules");
  if (!read_deny || !list_deny || !glob_deny || !grep_deny || !edit_deny)
    return;

  int fallback_prompts = 0;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto context = persistent_rule_tool_context(store, fallback_prompts, audits);

  context.permission_tool_name = "read_file";
  auto read = ava::tools::read_file(context, read_path);
  context.permission_tool_name = "list_directory";
  auto listed = ava::tools::list_directory(context, store.workspace_dir);
  context.permission_tool_name = "glob";
  auto globbed = ava::tools::glob_files(context, "**/*");
  context.permission_tool_name = "grep";
  auto grepped = ava::tools::grep_files(context, "PROVIDER_CANARY", "**/*");
  context.permission_tool_name = "write_file";
  auto written = ava::tools::write_file(context, edit_path, "EDIT_REPLACEMENT_CANARY\n");

  std::ifstream edit_file(edit_path, std::ios::binary);
  std::string const edit_content((std::istreambuf_iterator<char>(edit_file)), std::istreambuf_iterator<char>());
  std::array<std::pair<std::string, std::string>, 5> const expected_rules{{{"read_file", read_deny->rule_id},
                                                                           {"list_directory", list_deny->rule_id},
                                                                           {"glob", glob_deny->rule_id},
                                                                           {"grep", grep_deny->rule_id},
                                                                           {"write_file", edit_deny->rule_id}}};
  bool const exact_audits = audits.size() == expected_rules.size() && std::ranges::all_of(expected_rules, [&audits](auto const& expected) {
                              return std::ranges::any_of(audits, [&expected](ava::tools::PermissionAuditEvent const& event) {
                                return event.tool_name == expected.first && event.action == ava::permissions::PermissionAction::Deny &&
                                       event.resolution == "deny" && event.resolution_source == "persistent_rule" && event.rule_id == expected.second;
                              });
                            });
  expect(!read && !listed && !globbed && !grepped && !written && fallback_prompts == 0 && exact_audits &&
             read.error().format().find("READ_PROVIDER_CANARY") == std::string::npos &&
             listed.error().format().find("READ_PROVIDER_CANARY") == std::string::npos &&
             globbed.error().format().find("READ_PROVIDER_CANARY") == std::string::npos &&
             grepped.error().format().find("READ_PROVIDER_CANARY") == std::string::npos && edit_content == "EDIT_ORIGINAL_CANARY\n",
         "exact persistent denies override policy Allows for read_file, list_directory, glob, grep, and adjacent writes without prompting or execution");
}

void test_search_filters_exact_persistent_read_denies_without_leaks()
{
  auto const root = create_empty_root("permission-rules-search-match-filter");
  auto const store = test_store(root);
  auto const visible_path = store.workspace_dir / "visible.txt";
  auto const denied_path = store.workspace_dir / "denied-provider-canary.txt";
  write_file_with_mode(visible_path, "visible search marker\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(denied_path, "DENIED_CONTENT_CANARY search marker\n", S_IRUSR | S_IWUSR);
  auto read_deny = add_exact_workspace_deny(store, ava::permissions::Operation::ReadFile, denied_path, "", "deny search match read");
  expect(read_deny.has_value(), "search match filter fixture writes an exact persistent ReadFile deny");
  if (!read_deny)
    return;

  int fallback_prompts = 0;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto context = persistent_rule_tool_context(store, fallback_prompts, audits);
  auto const has_expected_deny_audit = [&] {
    return audits.size() == 2 && std::ranges::any_of(audits, [&](ava::tools::PermissionAuditEvent const& event) {
             return event.operation == ava::permissions::Operation::ReadFile && event.target_path == denied_path &&
                    event.action == ava::permissions::PermissionAction::Deny && event.resolution_source == "persistent_rule" &&
                    event.rule_id == read_deny->rule_id;
           });
  };

  context.permission_tool_name = "list_directory";
  auto listed = ava::tools::list_directory(context, store.workspace_dir);
  bool const list_omits_deny =
      listed && listed->entries.size() == 1 && listed->entries.front().name == visible_path.filename().string() && has_expected_deny_audit();

  audits.clear();
  context.permission_tool_name = "glob";
  auto globbed = ava::tools::glob_files(context, "**/*.txt");
  bool const glob_omits_deny = globbed && globbed->paths.size() == 1 && globbed->paths.front() == visible_path && has_expected_deny_audit();

  audits.clear();
  context.permission_tool_name = "grep";
  auto grepped = ava::tools::grep_files(context, "search marker", "**/*.txt");
  bool const grep_omits_deny = grepped && grepped->matches.size() == 1 && grepped->matches.front().path == visible_path &&
                               grepped->matches.front().line.find("DENIED_CONTENT_CANARY") == std::string::npos && has_expected_deny_audit();

  audits.clear();
  context.permission_tool_name = "write_file";
  auto written = ava::tools::write_file(context, denied_path, "replacement without denied preview\n");
  bool const write_omits_denied_preview = written && written->diff.empty() && has_expected_deny_audit();

  expect(list_omits_deny && glob_omits_deny && grep_omits_deny && write_omits_denied_preview && fallback_prompts == 0,
         "list_directory, glob, grep, and write previews omit exact persistently denied file data without resolver prompts or per-file Allow audits");
}

void test_explicit_write_preview_checks_persistent_read_denies_without_extra_prompt()
{
  auto const root = create_empty_root("permission-rules-explicit-write-preview");
  auto const store = test_store(root);
  auto const visible_path = store.workspace_dir / "visible-old-content.txt";
  auto const denied_path = store.workspace_dir / "denied-old-content.txt";
  write_file_with_mode(visible_path, "VISIBLE_OLD_CONTENT\n", S_IRUSR | S_IWUSR);
  write_file_with_mode(denied_path, "DENIED_OLD_CONTENT_CANARY\n", S_IRUSR | S_IWUSR);
  auto read_deny = add_exact_workspace_deny(store, ava::permissions::Operation::ReadFile, denied_path, "", "deny write preview read");
  expect(read_deny.has_value(), "explicit write preview fixture writes an exact persistent ReadFile deny");
  if (!read_deny)
    return;

  std::vector<ava::permissions::PermissionPrompt> prompts;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto fallback = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };
  auto context = ava::tools::ToolContext{
      .workspace_dir = store.workspace_dir,
      .mode = ava::core::Mode::Build,
      .permission_resolver = ava::permissions::build_persistent_permission_rule_resolver(store, std::move(fallback)),
      .auto_allow_deny_preflight = ava::permissions::build_persistent_permission_deny_preflight(store),
      .permission_audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        audits.push_back(event);
        return {};
      },
      .require_explicit_file_permissions = true,
  };
  context.permission_tool_name = "write_file";

  auto visible_write = ava::tools::write_file(context, visible_path, "visible replacement\n");
  auto denied_write = ava::tools::write_file(context, denied_path, "denied replacement\n");

  bool const edit_prompts_only = prompts.size() == 2 && std::ranges::all_of(prompts, [](ava::permissions::PermissionPrompt const& prompt) {
                                   return prompt.operation == ava::permissions::Operation::EditFile;
                                 });
  bool const visible_preview_in_write_prompt = edit_prompts_only && prompts.front().diff_preview.find("VISIBLE_OLD_CONTENT") != std::string::npos;
  bool const denied_content_never_prompted = std::ranges::none_of(
      prompts, [](ava::permissions::PermissionPrompt const& prompt) { return prompt.diff_preview.find("DENIED_OLD_CONTENT_CANARY") != std::string::npos; });
  bool const exact_deny_audit = std::ranges::any_of(audits, [&](ava::tools::PermissionAuditEvent const& event) {
    return event.operation == ava::permissions::Operation::ReadFile && event.target_path == denied_path &&
           event.action == ava::permissions::PermissionAction::Deny && event.resolution_source == "persistent_rule" && event.rule_id == read_deny->rule_id;
  });
  bool const no_read_allow_audit = std::ranges::none_of(audits, [](ava::tools::PermissionAuditEvent const& event) {
    return event.operation == ava::permissions::Operation::ReadFile && event.action == ava::permissions::PermissionAction::Allow;
  });
  expect(visible_write && denied_write && edit_prompts_only && visible_preview_in_write_prompt && denied_content_never_prompted && exact_deny_audit &&
             no_read_allow_audit && visible_write->diff.find("VISIBLE_OLD_CONTENT") != std::string::npos && denied_write->diff.empty(),
         "require-explicit writes request only mutation permission while persistent ReadFile deny omits and never leaks old preview content");
}

void test_policy_allowed_reads_fail_closed_on_malformed_global_and_workspace_rules()
{
  auto const root = create_empty_root("permission-rules-auto-allow-malformed");
  auto const store = test_store(root);
  auto const canary_path = store.workspace_dir / "provider-canary.txt";
  write_file_with_mode(canary_path, "MALFORMED_STORAGE_PROVIDER_CANARY\n", S_IRUSR | S_IWUSR);

  int fallback_prompts = 0;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto context = persistent_rule_tool_context(store, fallback_prompts, audits);
  context.permission_tool_name = "read_file";
  context.require_explicit_file_permissions = true;
  context.permission_resolver =
      [&fallback_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    ++fallback_prompts;
    return ava::permissions::PermissionResolution::Allow;
  };

  write_file_with_mode(store.global_rules_file, "{\"schema_version\":3,\"rules\":[]}", S_IRUSR | S_IWUSR);
  auto global_malformed = ava::tools::read_file(context, canary_path);

  write_file_with_mode(store.global_rules_file, "{\"schema_version\":2,\"rules\":[]}", S_IRUSR | S_IWUSR);
  auto const workspace_rules_file = ava::permissions::enforceable_permission_rules_file(store, ava::permissions::PermissionRuleScope::Workspace);
  write_file_with_mode(workspace_rules_file, "{\"schema_version\":3,\"rules\":[]}", S_IRUSR | S_IWUSR);
  auto workspace_malformed = ava::tools::read_file(context, canary_path);

  bool const error_audits = audits.size() == 2 && std::ranges::all_of(audits, [](ava::tools::PermissionAuditEvent const& event) {
                              return event.action == ava::permissions::PermissionAction::Deny && event.resolution == "deny" &&
                                     event.resolution_source == "persistent_rule_error" && event.rule_id.empty();
                            });
  expect(!global_malformed && !workspace_malformed && fallback_prompts == 0 && error_audits &&
             global_malformed.error().format().find("MALFORMED_STORAGE_PROVIDER_CANARY") == std::string::npos &&
             workspace_malformed.error().format().find("MALFORMED_STORAGE_PROVIDER_CANARY") == std::string::npos,
         "malformed protected global and workspace rule storage fails closed before policy-Allowed reads without fallback prompts or canary access");
}

ava::permissions::PermissionRuleDraft persistent_deny_draft(std::filesystem::path const& target)
{
  return ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Global,
                                               .action = ava::permissions::PermissionAction::Deny,
                                               .operation = ava::permissions::Operation::ReadFile,
                                               .mode = ava::permissions::PermissionRuleMode::Any,
                                               .tool_name = "",
                                               .target_path = target,
                                               .command = "",
                                               .command_recipe_key = {},
                                               .recipe_display = {},
                                               .critical_acknowledged = false,
                                               .reason = "persistent deny must survive unsafe storage paths",
                                               .actor = "test"};
}

void test_permission_rule_storage_rejects_unsafe_ancestor_without_dropping_denies()
{
  auto const root = temp_root() / "permission-rules-unsafe-ancestor";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto const target = root / "outside.txt";
  auto deny = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(target));
  auto const config_dir = store.global_rules_file.parent_path().parent_path();
  expect(deny && ::chmod(config_dir.c_str(), S_IRWXU | S_IRWXO) == 0,
         "unsafe ancestor fixture creates a persistent deny then makes its ancestor world writable without a root sticky namespace");

  auto anchored_load = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  auto anchored_write = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(root / "other.txt"));

  expect(::chmod(config_dir.c_str(), S_IRWXU) == 0, "unsafe ancestor fixture restores private ancestor mode");
  auto restored = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  expect(anchored_load && *anchored_load && anchored_write && restored && *restored && (*restored)->rule_id == deny->rule_id,
         "a pre-opened permission-rule anchor remains authoritative across later logical-ancestor mode changes without losing a persisted Deny");
}

void test_permission_rule_storage_allows_private_primary_group_ancestor_when_verified()
{
  auto const root = temp_root() / "permission-rules-private-primary-group-ancestor";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto const config_dir = store.global_rules_file.parent_path().parent_path();
  std::filesystem::create_directories(config_dir);
  if (::chown(config_dir.c_str(), ::geteuid(), ::getegid()) != 0 || ::chmod(config_dir.c_str(), S_IRWXU | S_IRWXG) != 0)
    return;

  struct stat status{};
  if (::stat(config_dir.c_str(), &status) != 0 || !ava::command::detail::is_current_user_private_primary_group_directory(status))
    return;

  auto const target = root / "outside.txt";
  auto deny = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(target));
  auto matched = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  expect(deny && matched && *matched && (*matched)->rule_id == deny->rule_id,
         "a verified private-primary-group group-writable ancestor is accepted while final rules storage remains private");
}

void test_permission_rule_storage_rejects_symlinked_or_replaced_directories()
{
  auto const root = temp_root() / "permission-rules-replaced-directories";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto const target = root / "outside.txt";
  auto deny = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(target));
  auto const rules_dir = store.global_rules_file.parent_path();
  auto const redirected_root = root / "redirected";
  std::filesystem::create_directories(redirected_root);
  expect(::chmod(redirected_root.c_str(), S_IRWXU) == 0, "replacement fixture creates private redirect destination");

  auto const parked_dir = rules_dir.string() + ".parked";
  std::error_code rename_error;
  std::filesystem::rename(rules_dir, parked_dir, rename_error);
  std::error_code link_error;
  std::filesystem::create_directory_symlink(redirected_root, rules_dir, link_error);
  auto symlink_load = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  auto symlink_write = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(root / "redirected-write.txt"));
  auto const redirect_untouched = !std::filesystem::exists(redirected_root / "permission-rules.json");

  std::filesystem::remove(rules_dir, remove_error);
  std::filesystem::rename(parked_dir, rules_dir, rename_error);

  auto const config_dir = rules_dir.parent_path();
  auto const parked_config = config_dir.string() + ".parked";
  std::filesystem::rename(config_dir, parked_config, rename_error);
  std::filesystem::create_directory_symlink(redirected_root, config_dir, link_error);
  auto ancestor_symlink_load = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  auto ancestor_symlink_write = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(root / "ancestor-redirected-write.txt"));
  std::filesystem::remove(config_dir, remove_error);
  std::filesystem::rename(parked_config, config_dir, rename_error);

  auto const redirect_still_untouched =
      !std::filesystem::exists(redirected_root / "permission-rules.json") && !std::filesystem::exists(redirected_root / "ava" / "permission-rules.json");
  auto restored = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  expect(deny && !rename_error && !link_error && symlink_load && *symlink_load && symlink_write && ancestor_symlink_load && *ancestor_symlink_load &&
             ancestor_symlink_write && redirect_untouched && redirect_still_untouched && restored && *restored && (*restored)->rule_id == deny->rule_id,
         "pre-opened permission-rule anchors ignore later pathname redirection, keep writes on the original inode tree, and retain the original Deny");
}

void test_permission_rule_storage_requires_owner_only_final_directory()
{
  auto const root = temp_root() / "permission-rules-final-directory-mode";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const store = test_store(root);
  auto const target = root / "outside.txt";
  auto deny = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(target));
  auto const rules_dir = store.global_rules_file.parent_path();
  expect(deny && ::chmod(rules_dir.c_str(), S_IRWXU | S_IRGRP | S_IXGRP) == 0, "final rules-directory fixture makes an existing Deny directory non-private");

  auto rejected_load = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  auto rejected_write = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(root / "other.txt"));
  expect(::chmod(rules_dir.c_str(), S_IRWXU) == 0, "final rules-directory fixture restores exact 0700 mode");
  auto restored = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  expect(!rejected_load && !rejected_write && restored && *restored && (*restored)->rule_id == deny->rule_id,
         "a non-0700 final rule directory fails closed and cannot drop or replace a persisted Deny");
}

void test_permission_rule_storage_allows_root_sticky_temp_ancestor()
{
  auto const temporary_root = std::filesystem::temp_directory_path();
  struct stat temporary_status{};
  // Direct child of the process temporary directory so the sticky, root-owned
  // ancestor property is the real TMPDIR/temp_directory_path(), not a nested
  // harness namespace.
  auto const root = ScopedTestDirectory::create_unique(temporary_root, "ava-permission-rules-sticky-");
  auto const temporary_is_root_sticky =
      ::stat(temporary_root.c_str(), &temporary_status) == 0 && temporary_status.st_uid == 0 && (temporary_status.st_mode & S_ISVTX) != 0;
  expect(root.path().parent_path() == temporary_root && ::chmod(root.path().c_str(), S_IRWXU) == 0,
         "root-sticky ancestor fixture creates an owned 0700 child that directly descends from the process temporary directory");

  auto const store = test_store(root);
  auto const target = root / "outside.txt";
  auto deny = ava::permissions::add_persistent_permission_rule(store, persistent_deny_draft(target));
  auto matched = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, target));
  expect(temporary_is_root_sticky && deny && matched && *matched && (*matched)->rule_id == deny->rule_id,
         "a root-owned sticky temporary namespace is accepted when the rule-storage child is owned and exact 0700");
}

void test_permission_rule_workspace_legacy_path_is_not_enforceable()
{
  auto const root = create_empty_root("permission-rules-legacy-workspace-path");

  auto const workspace = root / "workspace";
  auto const store = test_store(root);
  auto const outside = root / "outside.txt";
  ava::permissions::PersistentPermissionRule const forged{.rule_id = "permrule_forged",
                                                          .scope = ava::permissions::PermissionRuleScope::Workspace,
                                                          .workspace_dir = store.workspace_dir,
                                                          .action = ava::permissions::PermissionAction::Allow,
                                                          .operation = ava::permissions::Operation::ReadFile,
                                                          .mode = ava::permissions::PermissionRuleMode::Any,
                                                          .tool_name = "",
                                                          .target_path = outside,
                                                          .command = "",
                                                          .command_recipe_key = {},
                                                          .recipe_display = {},
                                                          .critical_acknowledged = false,
                                                          .reason = "forged workspace rule",
                                                          .actor = "model",
                                                          .created_at = "2026-05-07T00:00:00Z"};
  write_file_with_mode(store.workspace_rules_file, std::string("{\"schema_version\":1,\"rules\":[") + ava::permissions::permission_rule_json(forged) + "]}",
                       S_IRUSR | S_IWUSR);

  auto loaded = ava::permissions::load_persistent_permission_rules(store);
  auto matched = ava::permissions::match_persistent_permission_rule(store, read_prompt(store, outside));
  expect(loaded && loaded->empty() && matched && !*matched, "model-writable workspace permission-rules.json is ignored by enforceable rule loading");
}

void test_file_tools_reject_workspace_permission_rule_writes()
{
  auto const root = create_empty_root("permission-rules-file-tool-guard");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace / ".ava");

  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  auto written =
      ava::tools::write_file(context, workspace / ".ava" / "permission-rules.json", "{}", ava::tools::WriteOptions{.permission_already_checked = true});
  expect(!written && written.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "normal file tools cannot write legacy workspace permission rule files");
}

void test_file_tools_reject_enforceable_permission_rule_writes()
{
  auto const root = create_empty_root("permission-rules-enforceable-file-tool-guard");

  auto const workspace = root / "workspace";
  auto const config_dir = workspace / ".config" / "ava";
  std::filesystem::create_directories(config_dir);
  static_cast<void>(::chmod(root.c_str(), S_IRWXU));
  static_cast<void>(::chmod(workspace.c_str(), S_IRWXU));
  static_cast<void>(::chmod(config_dir.c_str(), S_IRWXU));
  auto anchors = ava::core::AnchorSet::open({workspace, config_dir});
  expect(anchors.has_value(), "enforceable file-tool guard opens its shared AnchorSet");
  if (!anchors)
    return;
  auto const store = ava::permissions::PermissionRuleStore{.global_rules_file = config_dir / "permission-rules.json",
                                                           .workspace_rules_file = workspace / ".ava" / "permission-rules.json",
                                                           .workspace_dir = workspace,
                                                           .anchor_set = *anchors};
  auto const outside = root / "outside.txt";

  auto added =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::ReadFile,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Any,
                                                                                                    .tool_name = "",
                                                                                                    .target_path = outside,
                                                                                                    .command = "",
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "register protected rule paths",
                                                                                                    .actor = "test"});
  expect(added.has_value(),
         added ? "permission rule storage registers protected paths" : "permission rule storage registers protected paths: " + added.error().format());

  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.mcp_global_config_file = store.global_rules_file.parent_path() / "mcp.json";
  auto global_write = ava::tools::write_file(context, store.global_rules_file, "{}", ava::tools::WriteOptions{.permission_already_checked = true});
  expect(!global_write && global_write.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "normal file tools cannot write enforceable global permission rule files inside the workspace");

  auto const workspace_rules_file = ava::permissions::enforceable_permission_rules_file(store, ava::permissions::PermissionRuleScope::Workspace);
  auto workspace_edit = ava::tools::edit_file(context, workspace_rules_file, "\"rules\"", "\"rules\"");
  expect(!workspace_edit && workspace_edit.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "normal file tools cannot edit workspace-keyed enforceable permission rule files");

  auto const config_link = workspace / "config-link";
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(store.global_rules_file.parent_path(), config_link, symlink_error);
  if (!symlink_error)
  {
    auto alias_write =
        ava::tools::write_file(context, config_link / "permission-rules.json", "{}", ava::tools::WriteOptions{.permission_already_checked = true});
    expect(!alias_write && alias_write.error().category() == ava::core::ErrorCategory::PermissionDenied,
           "normal file tools use inode comparison for enforceable permission rule file aliases");
  }
}

void test_file_tools_reject_enforceable_permission_rule_writes_before_registration()
{
  auto const root = create_empty_root("permission-rules-unregistered-file-tool-guard");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const config_dir = workspace / ".config" / "ava";
  auto const store = ava::permissions::PermissionRuleStore{.global_rules_file = config_dir / "permission-rules.json",
                                                           .workspace_rules_file = workspace / ".ava" / "permission-rules.json",
                                                           .workspace_dir = workspace};

  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.mcp_global_config_file = config_dir / "mcp.json";

  auto global_write = ava::tools::write_file(context, store.global_rules_file, "{}", ava::tools::WriteOptions{.permission_already_checked = true});
  expect(!global_write && global_write.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "normal file tools reject global permission rule writes before rule store registration");

  auto const workspace_rules_file = ava::permissions::enforceable_permission_rules_file(store, ava::permissions::PermissionRuleScope::Workspace);
  auto workspace_write = ava::tools::write_file(context, workspace_rules_file, "{}", ava::tools::WriteOptions{.permission_already_checked = true});
  expect(!workspace_write && workspace_write.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "normal file tools reject workspace-keyed permission rule writes before rule store registration");
}

void test_registered_permission_rule_paths_protect_agent_loop_context()
{
  auto const root = create_empty_root("permission-rules-agent-loop-file-tool-guard");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const store = ava::permissions::PermissionRuleStore{.global_rules_file = workspace / ".config" / "ava" / "permission-rules.json",
                                                           .workspace_rules_file = workspace / ".ava" / "permission-rules.json",
                                                           .workspace_dir = workspace};

  ava::permissions::register_enforceable_permission_rule_files(store);

  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  auto global_write = ava::tools::write_file(context, store.global_rules_file, "{}", ava::tools::WriteOptions{.permission_already_checked = true});
  expect(!global_write && global_write.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "registered enforceable permission rule paths protect agent-loop-style tool contexts");
}

}  // namespace

void run_permission_rules_tests()
{
  test_permission_rule_storage_add_list_remove();
  test_old_critical_acknowledgements_never_recover_authority();
  test_legacy_command_allows_are_removed_one_at_a_time();
  test_permission_rule_precedence_denies_win();
  test_permission_rule_precedence_prefers_specific_same_scope_rules();
  test_schema_v2_recipe_rules_bind_scope_and_deny_precedence();
  test_critical_command_allows_are_always_one_shot();
  test_critical_acknowledgements_cannot_authorize_repository_build_or_test_text();
  test_permission_rule_matches_command_operations_without_path_targets();
  test_repository_build_test_persistent_allows_are_rejected_but_denies_win();
  test_legacy_command_allows_do_not_authorize_sealed_critical_or_unverified_plans();
  test_permission_rule_storage_fail_closed();
  test_permission_rule_broad_permissions_rejected();
  test_deny_path_matching_follows_physical_aliases_without_broadening_allow();
  test_file_and_search_tools_enforce_denies_across_physical_aliases();
  test_exact_workspace_root_deny_matches_list_directory_spellings();
  test_policy_allowed_file_and_search_tools_honor_exact_persistent_denies();
  test_search_filters_exact_persistent_read_denies_without_leaks();
  test_explicit_write_preview_checks_persistent_read_denies_without_extra_prompt();
  test_policy_allowed_reads_fail_closed_on_malformed_global_and_workspace_rules();
  test_permission_rule_storage_rejects_unsafe_ancestor_without_dropping_denies();
  test_permission_rule_storage_allows_private_primary_group_ancestor_when_verified();
  test_permission_rule_storage_rejects_symlinked_or_replaced_directories();
  test_permission_rule_storage_requires_owner_only_final_directory();
  test_permission_rule_storage_allows_root_sticky_temp_ancestor();
  test_permission_rule_workspace_legacy_path_is_not_enforceable();
  test_file_tools_reject_workspace_permission_rule_writes();
  test_file_tools_reject_enforceable_permission_rule_writes();
  test_file_tools_reject_enforceable_permission_rule_writes_before_registration();
  test_registered_permission_rule_paths_protect_agent_loop_context();
}
