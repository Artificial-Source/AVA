#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/session/attachments.h"
#include "ava/session/export.h"
#include "ava/session/session_branch.h"
#include "ava/session/session_tree.h"
#include "ava/session/Stats.h"
#include "ava/session/validation.h"
#include "ava/core/error.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace session_tests {
void test_session_tree_metadata_entries_validate_and_export()
{
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "entry_metadata",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::SessionMetadata,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"schema_version\":1,\"name\":\"Investigate auth flow\","
                                              "\"labels\":[\"bug\",\"auth\"],\"archived\":true,\"branch_origin\":\"root\","
                                              "\"actor\":\"auditor\"}"},
      ava::session::SessionEntry{.id = "entry_branch_summary",
                                 .parent_id = "entry_metadata",
                                 .type = ava::session::EntryType::BranchSummary,
                                 .timestamp = "2026-04-27T00:00:01Z",
                                 .data_json = "{\"schema_version\":1,\"source_session_id\":\"session_parent\","
                                              "\"branch_root_entry_id\":\"entry_metadata\","
                                              "\"branch_tip_entry_id\":\"entry_metadata\","
                                              "\"summary\":\"Branch tested the auth hypothesis.\","
                                              "\"provider\":\"openai\",\"model\":\"gpt-test\","
                                              "\"reason\":\"test\"}"},
  };

  auto const validation = ava::session::validate_session_replay(entries);
  expect(validation.ok(), "session tree metadata and branch summary entries are replay-valid");
  auto const metadata = ava::session::session_metadata_from_entries({}, entries);
  expect(metadata && metadata->actor == "auditor" && metadata->archived && metadata->labels_updated == "2026-04-27T00:00:00Z",
         "session metadata read-back exposes persisted actor, archive state, and label update time");

  auto const metadata_type = ava::session::parse_entry_type("session_metadata");
  auto const summary_type = ava::session::parse_entry_type("branch_summary");
  expect(metadata_type && *metadata_type == ava::session::EntryType::SessionMetadata && ava::session::to_string(*metadata_type) == "session_metadata" &&
             summary_type && *summary_type == ava::session::EntryType::BranchSummary && ava::session::to_string(*summary_type) == "branch_summary",
         "session tree entry types parse and serialize by stable names");

  auto const exported = ava::session::format_session_markdown(entries, ava::session::ExportOptions{});
  expect(exported.find("Session Metadata") != std::string::npos && exported.find("Branch Summary") != std::string::npos &&
             exported.find("Branch tested the auth hypothesis.") != std::string::npos,
         "session export includes tree metadata and branch summaries");

  auto invalid_metadata = entries;
  invalid_metadata[0].data_json = "{\"schema_version\":1,\"labels\":[\"dup\",\"dup\"]}";
  auto const invalid_metadata_validation = ava::session::validate_session_replay(invalid_metadata);
  expect(!invalid_metadata_validation.ok() &&
             ava::tests::session_replay_has_issue(invalid_metadata_validation, ava::session::SessionReplayIssueKind::InvalidSessionMetadataEntry),
         "session replay validator rejects malformed tree metadata entries");

  auto invalid_summary = entries;
  invalid_summary[1].data_json = "{\"schema_version\":1,\"summary\":\"\"}";
  auto const invalid_summary_validation = ava::session::validate_session_replay(invalid_summary);
  expect(!invalid_summary_validation.ok() &&
             ava::tests::session_replay_has_issue(invalid_summary_validation, ava::session::SessionReplayIssueKind::InvalidBranchSummaryEntry),
         "session replay validator rejects malformed branch summary entries");

  auto expect_invalid_branch_summary = [&](std::string data_json, std::string const& message) {
    auto invalid = entries;
    invalid[1].data_json = std::move(data_json);
    auto const validation = ava::session::validate_session_replay(invalid);
    expect(!validation.ok() && ava::tests::session_replay_has_issue(validation, ava::session::SessionReplayIssueKind::InvalidBranchSummaryEntry), message);
  };
  expect_invalid_branch_summary("{\"schema_version\":1,\"summary\":\"Missing provider\",\"model\":\"gpt-test\",\"reason\":\"test\"}",
                                "session replay validator rejects branch summaries missing provider");
  expect_invalid_branch_summary("{\"schema_version\":1,\"summary\":\"Missing model\",\"provider\":\"openai\",\"reason\":\"test\"}",
                                "session replay validator rejects branch summaries missing model");
  expect_invalid_branch_summary("{\"schema_version\":1,\"summary\":\"Missing reason\",\"provider\":\"openai\",\"model\":\"gpt-test\"}",
                                "session replay validator rejects branch summaries missing reason");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Missing source\",\"branch_root_entry_id\":\"entry_metadata\","
      "\"branch_tip_entry_id\":\"entry_metadata\",\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}",
      "session replay validator rejects branch summaries missing source_session_id");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Missing root\",\"source_session_id\":\"session_parent\","
      "\"branch_tip_entry_id\":\"entry_metadata\",\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}",
      "session replay validator rejects branch summaries missing branch_root_entry_id");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Missing tip\",\"source_session_id\":\"session_parent\","
      "\"branch_root_entry_id\":\"entry_metadata\",\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}",
      "session replay validator rejects branch summaries missing branch_tip_entry_id");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Missing referenced tip\",\"source_session_id\":\"session_parent\","
      "\"branch_root_entry_id\":\"entry_metadata\",\"branch_tip_entry_id\":\"missing\","
      "\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}",
      "session replay validator rejects branch summaries with dangling tip references");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Self referenced tip\",\"source_session_id\":\"session_parent\","
      "\"branch_root_entry_id\":\"entry_metadata\",\"branch_tip_entry_id\":\"entry_branch_summary\","
      "\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}",
      "session replay validator rejects branch summaries that point at the summary entry itself");
  auto inverted_summary_entries = std::vector<ava::session::SessionEntry>{
      ava::session::SessionEntry{.id = "entry_first",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"text\":\"first\"}"},
      ava::session::SessionEntry{.id = "entry_second",
                                 .parent_id = "entry_first",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-27T00:00:01Z",
                                 .data_json = "{\"text\":\"second\"}"},
      ava::session::SessionEntry{.id = "entry_inverted_summary",
                                 .parent_id = "entry_second",
                                 .type = ava::session::EntryType::BranchSummary,
                                 .timestamp = "2026-04-27T00:00:02Z",
                                 .data_json = "{\"schema_version\":1,\"summary\":\"Inverted range\",\"source_session_id\":\"session_parent\","
                                              "\"branch_root_entry_id\":\"entry_second\",\"branch_tip_entry_id\":\"entry_first\","
                                              "\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}"}};
  auto const inverted_summary_validation = ava::session::validate_session_replay(inverted_summary_entries);
  expect(!inverted_summary_validation.ok() &&
             ava::tests::session_replay_has_issue(inverted_summary_validation, ava::session::SessionReplayIssueKind::InvalidBranchSummaryEntry),
         "session replay validator rejects branch summaries with inverted root/tip ranges");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Bad actor\",\"source_session_id\":\"session_parent\","
      "\"branch_root_entry_id\":\"entry_metadata\",\"branch_tip_entry_id\":\"entry_metadata\","
      "\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\",\"actor\":123}",
      "session replay validator rejects malformed branch summary actor");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Bad provider\",\"source_session_id\":\"session_parent\","
      "\"branch_root_entry_id\":\"entry_metadata\",\"branch_tip_entry_id\":\"entry_metadata\","
      "\"provider\":\"bad\\u001b\",\"model\":\"gpt-test\",\"reason\":\"test\"}",
      "session replay validator rejects malformed branch summary provider text");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Bad\\u001bsummary\",\"source_session_id\":\"session_parent\","
      "\"branch_root_entry_id\":\"entry_metadata\",\"branch_tip_entry_id\":\"entry_metadata\","
      "\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"test\"}",
      "session replay validator rejects malformed branch summary text");
  expect_invalid_branch_summary(
      "{\"schema_version\":1,\"summary\":\"Bad reason\",\"source_session_id\":\"session_parent\","
      "\"branch_root_entry_id\":\"entry_metadata\",\"branch_tip_entry_id\":\"entry_metadata\","
      "\"provider\":\"openai\",\"model\":\"gpt-test\",\"reason\":\"" +
          std::string(1025, 'x') + "\"}",
      "session replay validator rejects oversized branch summary reason");

  auto empty_origin_with_name = entries;
  empty_origin_with_name[0].data_json = "{\"schema_version\":1,\"name\":\"Named\",\"branch_origin\":\"\"}";
  auto const empty_origin_validation = ava::session::validate_session_replay(empty_origin_with_name);
  expect(empty_origin_validation.ok(), "session replay validator treats empty branch_origin as absent");

  auto non_string_origin = entries;
  non_string_origin[0].data_json = "{\"schema_version\":1,\"name\":\"Named\",\"branch_origin\":123}";
  auto const non_string_origin_validation = ava::session::validate_session_replay(non_string_origin);
  expect(!non_string_origin_validation.ok() &&
             ava::tests::session_replay_has_issue(non_string_origin_validation, ava::session::SessionReplayIssueKind::InvalidSessionMetadataEntry),
         "session replay validator rejects non-string branch_origin values");

  auto non_bool_archived = entries;
  non_bool_archived[0].data_json = "{\"schema_version\":1,\"name\":\"Named\",\"archived\":\"yes\"}";
  auto const non_bool_archived_validation = ava::session::validate_session_replay(non_bool_archived);
  expect(!non_bool_archived_validation.ok() &&
             ava::tests::session_replay_has_issue(non_bool_archived_validation, ava::session::SessionReplayIssueKind::InvalidSessionMetadataEntry),
         "session replay validator rejects non-boolean archived metadata values");

  auto only_empty_origin = entries;
  only_empty_origin[0].data_json = "{\"schema_version\":1,\"branch_origin\":\"\"}";
  auto const only_empty_origin_validation = ava::session::validate_session_replay(only_empty_origin);
  expect(!only_empty_origin_validation.ok() &&
             ava::tests::session_replay_has_issue(only_empty_origin_validation, ava::session::SessionReplayIssueKind::InvalidSessionMetadataEntry),
         "session replay validator rejects metadata entries with no meaningful fields");

  auto unsupported_metadata = entries;
  unsupported_metadata[0].data_json = "{\"schema_version\":2,\"name\":\"future\"}";
  auto const unsupported_metadata_validation = ava::session::validate_session_replay(unsupported_metadata);
  expect(!unsupported_metadata_validation.ok() &&
             ava::tests::session_replay_has_issue(unsupported_metadata_validation, ava::session::SessionReplayIssueKind::InvalidSessionMetadataEntry),
         "session replay validator rejects unsupported session_metadata schema_version values");

  auto unsupported_summary = entries;
  unsupported_summary[1].data_json =
      "{\"schema_version\":2,\"summary\":\"future\","
      "\"source_session_id\":\"session_parent\",\"branch_root_entry_id\":\"entry_metadata\","
      "\"branch_tip_entry_id\":\"entry_metadata\",\"provider\":\"openai\",\"model\":\"gpt-test\","
      "\"reason\":\"test\"}";
  auto const unsupported_summary_validation = ava::session::validate_session_replay(unsupported_summary);
  expect(!unsupported_summary_validation.ok() &&
             ava::tests::session_replay_has_issue(unsupported_summary_validation, ava::session::SessionReplayIssueKind::InvalidBranchSummaryEntry),
         "session replay validator rejects unsupported branch_summary schema_version values");
}

void test_session_tree_index_derives_branches()
{
  auto const root = create_empty_root("session-tree-index");

  auto const workspace = root / "workspace";
  auto const sessions_dir = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto make_store = [&](std::string session_id) {
    return ava::session::SessionStore(
        ava::session::SessionStoreOptions{.root_dir = sessions_dir, .workspace_dir = workspace, .session_id = std::move(session_id)});
  };
  auto append_start = [](ava::session::SessionStore& store, std::string_view entry_id) {
    return append_session_entry_for_test(store, ava::session::SessionEntry{.id = std::string(entry_id),
                                                                           .parent_id = "",
                                                                           .type = ava::session::EntryType::SessionStart,
                                                                           .timestamp = "2026-04-27T00:00:00Z",
                                                                           .data_json = "{\"mode\":\"build\"}"});
  };

  auto root_store = make_store("session_root");
  auto child_store = make_store("session_child");
  auto grandchild_store = make_store("session_grandchild");
  auto orphan_store = make_store("session_orphan");
  auto corrupt_metadata_store = make_store("session_corrupt_metadata");
  expect(append_start(root_store, "entry_root_start").has_value() && append_start(child_store, "entry_child_start").has_value() &&
             append_start(grandchild_store, "entry_grandchild_start").has_value() && append_start(orphan_store, "entry_orphan_start").has_value() &&
             append_start(corrupt_metadata_store, "entry_corrupt_start").has_value(),
         "session tree index test creates session files");

  ava::session::SessionMetadataUpdate root_metadata;
  root_metadata.name = "Root";
  root_metadata.labels = std::vector<std::string>{"root"};
  root_metadata.branch_origin = "root";
  root_metadata.actor = "test";
  auto root_meta = append_session_metadata_for_test(root_store, std::move(root_metadata));

  ava::session::SessionMetadataUpdate child_metadata;
  child_metadata.name = "Child";
  child_metadata.labels = std::vector<std::string>{"branch"};
  child_metadata.parent_session_id = "session_root";
  child_metadata.source_session_id = "session_root";
  child_metadata.branch_from_entry_id = "entry_root_start";
  child_metadata.branch_origin = "fork";
  child_metadata.actor = "test";
  auto child_meta = append_session_metadata_for_test(child_store, std::move(child_metadata));

  ava::session::SessionMetadataUpdate grandchild_metadata;
  grandchild_metadata.name = "Grandchild";
  grandchild_metadata.parent_session_id = "session_child";
  grandchild_metadata.source_session_id = "session_child";
  grandchild_metadata.branch_from_entry_id = "entry_child_start";
  grandchild_metadata.branch_origin = "clone";
  grandchild_metadata.actor = "test";
  auto grandchild_meta = append_session_metadata_for_test(grandchild_store, std::move(grandchild_metadata));

  ava::session::SessionMetadataUpdate orphan_metadata;
  orphan_metadata.name = "Orphan";
  orphan_metadata.parent_session_id = "session_missing";
  orphan_metadata.branch_origin = "manual";
  orphan_metadata.actor = "test";
  auto orphan_meta = append_session_metadata_for_test(orphan_store, std::move(orphan_metadata));
  auto corrupt_meta = append_session_entry_for_test(corrupt_metadata_store, ava::session::SessionEntry{.id = "entry_corrupt_metadata",
                                                                                                       .parent_id = "entry_corrupt_start",
                                                                                                       .type = ava::session::EntryType::SessionMetadata,
                                                                                                       .timestamp = "2026-04-27T00:00:01Z",
                                                                                                       .data_json = "{\"schema_version\":1,\"name\":123}"});
  expect(root_meta && child_meta && grandchild_meta && orphan_meta && corrupt_meta, "session tree index test persists branch metadata");

  auto tree = ava::session::build_session_tree(workspace, sessions_dir, "session_grandchild");
  expect(tree.has_value(), tree ? "session tree index builds" : "session tree index builds: " + tree.error().format());
  if (!tree)
    return;

  auto contains = [](std::vector<std::string> const& values, std::string_view value) {
    return std::ranges::any_of(values, [value](std::string const& item) { return item == value; });
  };
  auto find_node = [&](std::string_view session_id) -> ava::session::SessionTreeNode const* {
    auto const found =
        std::ranges::find_if(tree->sessions, [session_id](ava::session::SessionTreeNode const& node) { return node.summary.session_id == session_id; });
    return found == tree->sessions.end() ? nullptr : &*found;
  };

  auto const* root_node = find_node("session_root");
  auto const* child_node = find_node("session_child");
  auto const* grandchild_node = find_node("session_grandchild");
  auto const* orphan_node = find_node("session_orphan");
  auto const* corrupt_node = find_node("session_corrupt_metadata");
  expect(root_node != nullptr && child_node != nullptr && grandchild_node != nullptr && orphan_node != nullptr && tree->sessions.size() == 4,
         "session tree index includes valid session summaries and skips malformed metadata sessions");
  expect(corrupt_node == nullptr, "session tree index skips sessions with malformed metadata");
  expect(root_node && root_node->metadata.name == "Root" && contains(root_node->metadata.labels, "root") && root_node->metadata.actor == "test" &&
             contains(root_node->children, "session_child"),
         "session tree index folds root metadata, actor, and direct children");
  expect(child_node && child_node->metadata.parent_session_id == "session_root" && child_node->metadata.source_session_id == "session_root" &&
             child_node->metadata.branch_from_entry_id == "entry_root_start" && child_node->metadata.branch_origin == "fork" &&
             contains(child_node->children, "session_grandchild"),
         "session tree index preserves child provenance metadata");
  expect(grandchild_node && grandchild_node->current && grandchild_node->children.empty() && orphan_node && orphan_node->children.empty(),
         "session tree index marks current and leaf sessions");
  expect(contains(tree->roots, "session_root") && contains(tree->roots, "session_orphan") && !contains(tree->roots, "session_child") &&
             contains(tree->leaves, "session_grandchild") && contains(tree->leaves, "session_orphan") && !contains(tree->leaves, "session_root"),
         "session tree index derives roots and leaves from parent metadata");
  expect(tree->current_path == std::vector<std::string>({"session_root", "session_child", "session_grandchild"}),
         "session tree index derives current branch path without scanning child lists");

  auto retargeted = *tree;
  ava::session::retarget_session_tree(retargeted, "session_child");
  auto const retargeted_child = std::ranges::find_if(retargeted.sessions, [](auto const& node) { return node.summary.session_id == "session_child"; });
  auto const retargeted_grandchild =
      std::ranges::find_if(retargeted.sessions, [](auto const& node) { return node.summary.session_id == "session_grandchild"; });
  expect(retargeted.current_session_id == "session_child" && retargeted.current_path == std::vector<std::string>({"session_root", "session_child"}) &&
             retargeted_child != retargeted.sessions.end() && retargeted_child->current && retargeted_grandchild != retargeted.sessions.end() &&
             !retargeted_grandchild->current && retargeted_child->children == std::vector<std::string>({"session_grandchild"}),
         "session tree current-session retarget is pure and preserves parent and child topology");
  expect(tree->current_session_id == "session_grandchild" && grandchild_node && grandchild_node->current,
         "session tree retarget leaves the original authoritative index unchanged");
}

void test_session_tree_index_handles_parent_cycles()
{
  auto const root = create_empty_root("session-tree-cycle");

  auto const workspace = root / "workspace";
  auto const sessions_dir = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto make_store = [&](std::string session_id) {
    return ava::session::SessionStore(
        ava::session::SessionStoreOptions{.root_dir = sessions_dir, .workspace_dir = workspace, .session_id = std::move(session_id)});
  };
  auto append_start = [](ava::session::SessionStore& store, std::string_view entry_id) {
    return append_session_entry_for_test(store, ava::session::SessionEntry{.id = std::string(entry_id),
                                                                           .parent_id = "",
                                                                           .type = ava::session::EntryType::SessionStart,
                                                                           .timestamp = "2026-04-27T00:00:00Z",
                                                                           .data_json = "{\"mode\":\"build\"}"});
  };

  auto first_store = make_store("session_cycle_a");
  auto second_store = make_store("session_cycle_b");
  expect(append_start(first_store, "entry_cycle_a_start").has_value() && append_start(second_store, "entry_cycle_b_start").has_value(),
         "session tree cycle test creates session files");

  ava::session::SessionMetadataUpdate first_metadata;
  first_metadata.parent_session_id = "session_cycle_b";
  first_metadata.branch_origin = "manual";
  first_metadata.actor = "test";
  auto first_meta = append_session_metadata_for_test(first_store, std::move(first_metadata));

  ava::session::SessionMetadataUpdate second_metadata;
  second_metadata.parent_session_id = "session_cycle_a";
  second_metadata.branch_origin = "manual";
  second_metadata.actor = "test";
  auto second_meta = append_session_metadata_for_test(second_store, std::move(second_metadata));
  expect(first_meta && second_meta, "session tree cycle test persists cyclic parent metadata");

  auto tree = ava::session::build_session_tree(workspace, sessions_dir, "session_cycle_a");
  expect(tree.has_value(), tree ? "session tree index builds with cycles" : "session tree index builds with cycles: " + tree.error().format());
  if (!tree)
    return;

  auto contains = [](std::vector<std::string> const& values, std::string_view value) {
    return std::ranges::any_of(values, [value](std::string const& item) { return item == value; });
  };
  auto find_node = [&](std::string_view session_id) -> ava::session::SessionTreeNode const* {
    auto const found =
        std::ranges::find_if(tree->sessions, [session_id](ava::session::SessionTreeNode const& node) { return node.summary.session_id == session_id; });
    return found == tree->sessions.end() ? nullptr : &*found;
  };

  auto const* first_node = find_node("session_cycle_a");
  auto const* second_node = find_node("session_cycle_b");
  expect(first_node && second_node && first_node->children.empty() && second_node->children.empty() && contains(tree->roots, "session_cycle_a") &&
             contains(tree->roots, "session_cycle_b") && contains(tree->leaves, "session_cycle_a") && contains(tree->leaves, "session_cycle_b") &&
             tree->current_path == std::vector<std::string>({"session_cycle_a"}),
         "session tree index treats parent cycles as usable root/leaf nodes");
}

void test_session_branch_fork_and_clone_copy_source_safely()
{
  auto const root = create_empty_root("session-branch-copy");

  auto const workspace = root / "workspace";
  auto const sessions_dir = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto source_store =
      ava::session::SessionStore(ava::session::SessionStoreOptions{.root_dir = sessions_dir, .workspace_dir = workspace, .session_id = "session_source"});
  expect(append_session_entry_for_test(
             source_store, ava::session::SessionEntry{.id = "entry_start",
                                                      .parent_id = "",
                                                      .type = ava::session::EntryType::SessionStart,
                                                      .timestamp = "2026-04-27T00:00:00Z",
                                                      .data_json = "{\"mode\":\"build\",\"provider\":\"openai\",\"model\":\"gpt-test\","
                                                                   "\"context_sources\":0,\"context_window_tokens\":128000,\"max_output_tokens\":4096,"
                                                                   "\"prompt_override\":false,\"supports_tools\":true,\"supports_streaming\":true,"
                                                                   "\"supports_reasoning\":true,\"reports_usage\":true}",
                                                      .version = 0}) &&
             append_session_entry_for_test(
                 source_store, ava::session::SessionEntry{.id = "entry_user",
                                                          .parent_id = "entry_start",
                                                          .type = ava::session::EntryType::UserMessage,
                                                          .timestamp = "2026-04-27T00:00:01Z",
                                                          .data_json = "{\"text\":\"question\",\"attachments\":[{\"id\":\"branch_img\","
                                                                       "\"type\":\"image\",\"mime_type\":\"image/png\",\"byte_size\":5,"
                                                                       "\"sha256\":\"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\","
                                                                       "\"storage_path\":\"attachments/branch_img.txt\"}]}",
                                                          .version = 2}) &&
             append_session_entry_for_test(source_store, ava::session::SessionEntry{.id = "entry_assistant",
                                                                                    .parent_id = "entry_user",
                                                                                    .type = ava::session::EntryType::AssistantMessage,
                                                                                    .timestamp = "2026-04-27T00:00:02Z",
                                                                                    .data_json = "{\"text\":\"answer\"}"}),
         "session branch test creates source entries");

  ava::session::SessionMetadataUpdate source_metadata;
  source_metadata.name = "Source";
  source_metadata.labels = std::vector<std::string>{"source"};
  source_metadata.branch_origin = "root";
  source_metadata.actor = "test";
  auto source_meta = append_session_metadata_for_test(source_store, std::move(source_metadata));
  auto source_entries_before = source_store.load();
  expect(source_meta && source_entries_before && source_entries_before->size() == 4, "session branch test source metadata is append-only");
  auto const source_attachment_path = ava::session::attachment_storage_root(source_store) / "attachments" / "branch_img.txt";
  std::filesystem::create_directories(source_attachment_path.parent_path());
  {
    std::ofstream file(source_attachment_path, std::ios::binary);
    file << "hello";
  }

  auto limited_branch = ava::session::create_session_branch(ava::session::SessionBranchOptions{
      .workspace_dir = workspace,
      .root_dir = sessions_dir,
      .source_session_id = "session_source",
      .branch_from_entry_id = "entry_user",
      .name = std::nullopt,
      .labels = std::nullopt,
      .read_limits = ava::session::SessionReadLimits{.max_file_bytes = 1024U * 1024U, .max_line_bytes = 1024U * 1024U, .max_entries = 1},
      .mode = ava::session::SessionBranchMode::Fork,
      .actor = "test"});
  expect(!limited_branch && limited_branch.error().message().find("entry count") != std::string::npos,
         "an explicit tiny branch read limit rejects the source before creating a destination");

  auto forked = ava::session::create_session_branch(
      ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                         .root_dir = sessions_dir,
                                         .source_session_id = "session_source",
                                         .branch_from_entry_id = "entry_user",
                                         .name = std::optional<std::string>{"Forked"},
                                         .labels = std::optional<std::vector<std::string>>{std::vector<std::string>{"forked"}},
                                         .mode = ava::session::SessionBranchMode::Fork,
                                         .actor = "test"});
  expect(forked.has_value(), forked ? "default branch reads the same source with legacy-unbounded limits"
                                    : "default branch reads the same source with legacy-unbounded limits: " + forked.error().format());
  if (!forked)
    return;
  auto fork_contender = ava::session::SessionLease::acquire(forked->store.session_path());
  expect(!fork_contender && fork_contender.error().message().find("already owned") != std::string::npos,
         "session branch retains destination ownership after all copied records and metadata are published");
  auto fork_entries = forked->store.load();
  auto const fork_bytes = ava::tests::read_session_test_binary_file(forked->store.session_path());
  expect(fork_entries && forked->copied_entry_count == 2 && fork_entries->size() == 3 && (*fork_entries)[0].version == 0 && (*fork_entries)[1].version == 2 &&
             fork_bytes.starts_with("{\"id\":\"entry_start\"") && fork_entries->back().type == ava::session::EntryType::SessionMetadata &&
             forked->metadata.name == "Forked" && forked->metadata.labels.size() == 1 && forked->metadata.labels[0] == "forked" &&
             forked->metadata.parent_session_id == "session_source" && forked->metadata.source_session_id == "session_source" &&
             forked->metadata.branch_from_entry_id == "entry_user" && forked->metadata.branch_origin == "fork",
         "session fork preserves copied entry versions and appends provenance metadata");
  auto fork_attachment = ava::session::load_image_attachment(
      forked->store, ava::session::ImageAttachmentRef{.id = "branch_img",
                                                      .mime_type = "image/png",
                                                      .storage_path = "attachments/branch_img.txt",
                                                      .sha256 = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
                                                      .byte_size = 5});
  expect(fork_attachment && fork_attachment->bytes == "hello", "session fork copies verified image attachment storage for copied entries");

  auto clone_with_branch_from = ava::session::create_session_branch(ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                                                                                       .root_dir = sessions_dir,
                                                                                                       .source_session_id = "session_source",
                                                                                                       .branch_from_entry_id = "entry_user",
                                                                                                       .name = std::nullopt,
                                                                                                       .labels = std::nullopt,
                                                                                                       .mode = ava::session::SessionBranchMode::Clone,
                                                                                                       .actor = "test"});
  expect(!clone_with_branch_from && clone_with_branch_from.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "backend session clone rejects explicit branch_from_entry_id");

  auto cloned = ava::session::create_session_branch(ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                                                                       .root_dir = sessions_dir,
                                                                                       .source_session_id = "session_source",
                                                                                       .branch_from_entry_id = "",
                                                                                       .name = std::optional<std::string>{"Cloned"},
                                                                                       .labels = std::nullopt,
                                                                                       .mode = ava::session::SessionBranchMode::Clone,
                                                                                       .actor = "test"});
  expect(cloned.has_value(), cloned ? "session clone creates a new branch" : "session clone creates a new branch: " + cloned.error().format());
  if (!cloned)
    return;
  auto clone_entries = cloned->store.load();
  auto source_entries_after = source_store.load();
  expect(clone_entries && source_entries_after && source_entries_after->size() == source_entries_before->size() &&
             cloned->copied_entry_count == source_entries_before->size() && clone_entries->size() == source_entries_before->size() + 1 &&
             (*clone_entries)[0].version == 0 && cloned->metadata.name == "Cloned" && cloned->metadata.labels.size() == 1 &&
             cloned->metadata.labels[0] == "source" && cloned->metadata.parent_session_id == "session_source" &&
             cloned->metadata.branch_from_entry_id == source_entries_before->back().id && cloned->metadata.branch_origin == "clone" &&
             cloned->metadata.actor == "test",
         "session clone copies the full source session without modifying the source file and exposes actor");
  auto clone_attachment = ava::session::load_image_attachment(
      cloned->store, ava::session::ImageAttachmentRef{.id = "branch_img",
                                                      .mime_type = "image/png",
                                                      .storage_path = "attachments/branch_img.txt",
                                                      .sha256 = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
                                                      .byte_size = 5});
  expect(clone_attachment && clone_attachment->bytes == "hello", "session clone copies verified image attachment storage for copied entries");

  auto missing = ava::session::create_session_branch(ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                                                                        .root_dir = sessions_dir,
                                                                                        .source_session_id = "session_source",
                                                                                        .branch_from_entry_id = "missing",
                                                                                        .name = std::nullopt,
                                                                                        .labels = std::nullopt,
                                                                                        .mode = ava::session::SessionBranchMode::Fork,
                                                                                        .actor = "test"});
  expect(!missing && missing.error().category() == ava::core::ErrorCategory::NotFound, "session fork rejects missing branch source entries");

  auto title_source =
      ava::session::SessionStore(ava::session::SessionStoreOptions{.root_dir = sessions_dir, .workspace_dir = workspace, .session_id = "session_title_source"});
  expect(
      append_session_entry_for_test(title_source, ava::session::SessionEntry{.id = "title_start",
                                                                             .parent_id = "",
                                                                             .type = ava::session::EntryType::SessionStart,
                                                                             .timestamp = "2026-07-21T00:00:00Z",
                                                                             .data_json = "{\"mode\":\"build\",\"provider\":\"openai\",\"model\":\"gpt-test\","
                                                                                          "\"context_sources\":0,\"prompt_override\":false}",
                                                                             .version = 0}) &&
          append_session_entry_for_test(title_source, ava::session::SessionEntry{.id = "title_user",
                                                                                 .parent_id = "title_start",
                                                                                 .type = ava::session::EntryType::UserMessage,
                                                                                 .timestamp = "2026-07-21T00:00:01Z",
                                                                                 .data_json = "{\"text\":\"title source\"}"}) &&
          append_session_entry_for_test(title_source, ava::session::SessionEntry{.id = "title_assistant",
                                                                                 .parent_id = "title_user",
                                                                                 .type = ava::session::EntryType::AssistantMessage,
                                                                                 .timestamp = "2026-07-21T00:00:02Z",
                                                                                 .data_json = "{\"text\":\"answer\"}"}),
      "branch title inheritance source entries append");
  ava::session::SessionMetadataUpdate generated_title;
  generated_title.generated_title = "Generated Source Session Title";
  generated_title.actor = "auto-title";
  auto generated_title_metadata = append_session_metadata_for_test(title_source, std::move(generated_title));
  auto inherit_options = [&](ava::session::SessionBranchMode mode = ava::session::SessionBranchMode::Fork) {
    return ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                              .root_dir = sessions_dir,
                                              .source_session_id = "session_title_source",
                                              .branch_from_entry_id = mode == ava::session::SessionBranchMode::Fork ? "title_user" : "",
                                              .name = std::nullopt,
                                              .labels = std::nullopt,
                                              .mode = mode,
                                              .actor = "test"};
  };
  auto generated_title_fork = ava::session::create_session_branch(inherit_options());
  expect(generated_title_metadata && generated_title_fork && generated_title_fork->copied_entry_count == 2 &&
             generated_title_fork->metadata.generated_title == "Generated Source Session Title" && !generated_title_fork->metadata.has_manual_name &&
             generated_title_fork->metadata.effective_title() == "Generated Source Session Title",
         "a fork from before generated metadata inherits the full source generated title independently of its copied prefix");

  auto empty_manual_metadata = append_session_metadata_for_test(title_source, {.name = std::string{}, .actor = "test"});
  auto suppressed_title_fork = ava::session::create_session_branch(inherit_options());
  auto suppressed_title_clone = ava::session::create_session_branch(inherit_options(ava::session::SessionBranchMode::Clone));
  auto explicit_title_options = inherit_options(ava::session::SessionBranchMode::Clone);
  explicit_title_options.name = "Explicit Branch Name";
  auto explicitly_named_clone = ava::session::create_session_branch(std::move(explicit_title_options));
  expect(empty_manual_metadata && suppressed_title_fork && suppressed_title_clone && explicitly_named_clone &&
             suppressed_title_fork->metadata.has_manual_name && suppressed_title_fork->metadata.name.empty() &&
             suppressed_title_fork->metadata.generated_title == "Generated Source Session Title" && suppressed_title_fork->metadata.effective_title().empty() &&
             suppressed_title_clone->metadata.has_manual_name && suppressed_title_clone->metadata.name.empty() &&
             suppressed_title_clone->metadata.generated_title == "Generated Source Session Title" &&
             suppressed_title_clone->metadata.effective_title().empty() && explicitly_named_clone->metadata.name == "Explicit Branch Name" &&
             explicitly_named_clone->metadata.generated_title == "Generated Source Session Title" &&
             explicitly_named_clone->metadata.effective_title() == "Explicit Branch Name",
         "fork and clone inherit later manual-empty suppression and generated metadata while an explicit branch name wins");

  auto v4_source =
      ava::session::SessionStore(ava::session::SessionStoreOptions{.root_dir = sessions_dir, .workspace_dir = workspace, .session_id = "session_v4_prefix"});
  expect(append_session_entry_for_test(v4_source, ava::session::SessionEntry{.id = "v4_safe_prefix",
                                                                             .parent_id = "",
                                                                             .type = ava::session::EntryType::UserMessage,
                                                                             .timestamp = "2026-07-18T00:00:00Z",
                                                                             .data_json = "{\"text\":\"safe\"}",
                                                                             .version = 3}) &&
             append_session_entry_for_test(
                 v4_source, ava::session::SessionEntry{.id = "v4_committed_item",
                                                       .parent_id = "v4_safe_prefix",
                                                       .type = ava::session::EntryType::AssistantOutputItem,
                                                       .timestamp = "2026-07-18T00:00:01Z",
                                                       .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_committed\",\"sequence\":0,\"kind\":"
                                                                    "\"text\",\"text\":\"committed\",\"assistant_phase\":\"commentary\"}"}) &&
             append_session_entry_for_test(
                 v4_source, ava::session::SessionEntry{.id = "v4_committed_commit",
                                                       .parent_id = "v4_committed_item",
                                                       .type = ava::session::EntryType::AssistantTurnCommit,
                                                       .timestamp = "2026-07-18T00:00:02Z",
                                                       .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_committed\",\"item_count\":1,"
                                                                    "\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"finish_reason\":\"completed\"}"}) &&
             append_session_entry_for_test(
                 v4_source, ava::session::SessionEntry{.id = "v4_staged_item",
                                                       .parent_id = "v4_committed_commit",
                                                       .type = ava::session::EntryType::AssistantOutputItem,
                                                       .timestamp = "2026-07-18T00:00:03Z",
                                                       .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_staged\",\"sequence\":0,\"kind\":"
                                                                    "\"text\",\"text\":\"staged\",\"assistant_phase\":\"final_answer\"}"}),
         "session branch test writes committed and staged v4 prefix fixtures");
  auto branch_options = [&](std::string branch_from_entry_id, ava::session::SessionBranchMode mode = ava::session::SessionBranchMode::Fork) {
    return ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                              .root_dir = sessions_dir,
                                              .source_session_id = "session_v4_prefix",
                                              .branch_from_entry_id = std::move(branch_from_entry_id),
                                              .name = std::nullopt,
                                              .labels = std::nullopt,
                                              .mode = mode,
                                              .actor = "test"};
  };
  auto before_staged = ava::session::create_session_branch(branch_options("v4_safe_prefix"));
  auto inside_committed = ava::session::create_session_branch(branch_options("v4_committed_item"));
  auto at_committed_boundary = ava::session::create_session_branch(branch_options("v4_committed_commit"));
  auto inside_staged = ava::session::create_session_branch(branch_options("v4_staged_item"));
  auto clone_with_staged_suffix = ava::session::create_session_branch(branch_options({}, ava::session::SessionBranchMode::Clone));
  expect(before_staged && before_staged->copied_entry_count == 1 && at_committed_boundary && at_committed_boundary->copied_entry_count == 3 &&
             !inside_committed && !inside_staged && !clone_with_staged_suffix &&
             inside_committed.error().message() ==
                 "branch prefix contains an assistant-output diagnostic; choose a committed boundary or recover the source session" &&
             inside_staged.error().message() ==
                 "branch prefix contains an assistant-output diagnostic; choose a committed boundary or recover the source session" &&
             clone_with_staged_suffix.error().message() ==
                 "branch prefix contains an assistant-output diagnostic; choose a committed boundary or recover the source session",
         "fork and clone accept only v4 logical boundaries and allow an explicit target before a later staged suffix");
}

void test_session_branch_summary_appends_to_source_session()
{
  auto const root = create_empty_root("session-branch-summary");

  auto const workspace = root / "workspace";
  auto const sessions_dir = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto source_store =
      ava::session::SessionStore(ava::session::SessionStoreOptions{.root_dir = sessions_dir, .workspace_dir = workspace, .session_id = "session_source"});
  expect(append_session_entry_for_test(
             source_store, ava::session::SessionEntry{.id = "entry_start",
                                                      .parent_id = "",
                                                      .type = ava::session::EntryType::SessionStart,
                                                      .timestamp = "2026-04-27T00:00:00Z",
                                                      .data_json = "{\"mode\":\"build\",\"provider\":\"openai\",\"model\":\"gpt-test\","
                                                                   "\"context_sources\":0,\"context_window_tokens\":128000,\"max_output_tokens\":4096,"
                                                                   "\"prompt_override\":false,\"supports_tools\":true,\"supports_streaming\":true,"
                                                                   "\"supports_reasoning\":true,\"reports_usage\":true}"}) &&
             append_session_entry_for_test(source_store, ava::session::SessionEntry{.id = "entry_user",
                                                                                    .parent_id = "entry_start",
                                                                                    .type = ava::session::EntryType::UserMessage,
                                                                                    .timestamp = "2026-04-27T00:00:01Z",
                                                                                    .data_json = "{\"text\":\"question\"}"}) &&
             append_session_entry_for_test(source_store, ava::session::SessionEntry{.id = "entry_assistant",
                                                                                    .parent_id = "entry_user",
                                                                                    .type = ava::session::EntryType::AssistantMessage,
                                                                                    .timestamp = "2026-04-27T00:00:02Z",
                                                                                    .data_json = "{\"text\":\"answer\"}"}),
         "branch summary test creates source entries");
  auto const source_entries_before = source_store.load();
  expect(source_entries_before && source_entries_before->size() == 3, "branch summary test loads source before append");
  if (!source_entries_before)
    return;

  auto summary = ava::session::append_branch_summary(ava::session::BranchSummaryOptions{.workspace_dir = workspace,
                                                                                        .root_dir = sessions_dir,
                                                                                        .source_session_id = "session_source",
                                                                                        .branch_root_entry_id = "entry_user",
                                                                                        .branch_tip_entry_id = "entry_assistant",
                                                                                        .summary = "Abandoned branch explored the alternate answer.",
                                                                                        .provider = "openai",
                                                                                        .model = "gpt-test",
                                                                                        .reason = "test",
                                                                                        .read_limits = ava::session::SessionReadLimits{},
                                                                                        .actor = "test"});
  expect(summary.has_value(), summary ? "branch summary appends to source session" : "branch summary appends to source session: " + summary.error().format());
  if (!summary)
    return;
  auto source_entries_after = source_store.load();
  expect(source_entries_after && source_entries_after->size() == source_entries_before->size() + 1 &&
             source_entries_after->back().type == ava::session::EntryType::BranchSummary &&
             source_entries_after->back().parent_id == source_entries_before->back().id && summary->entry.id == source_entries_after->back().id,
         "branch summary is append-only at the source session tip");
  if (!source_entries_after)
    return;

  auto const validation = ava::session::validate_session_replay(*source_entries_after);
  auto const stats = ava::session::compute_session_stats(*source_entries_after);
  auto const exported = ava::session::format_session_markdown(*source_entries_after, ava::session::ExportOptions{});
  auto validation_message = std::string("branch summary entries validate from the source session");
  if (!validation.ok() && !validation.issues.empty())
  {
    validation_message += ": ";
    validation_message += validation.issues.front().message;
  }
  expect(validation.ok(), validation_message);
  expect(stats->counts.branch_summary == 1, "branch summary entries count in source session stats");
  expect(exported.find("Abandoned branch explored the alternate answer.") != std::string::npos, "branch summary entries export from the source session");

  auto root_after_tip = ava::session::append_branch_summary(ava::session::BranchSummaryOptions{.workspace_dir = workspace,
                                                                                               .root_dir = sessions_dir,
                                                                                               .source_session_id = "session_source",
                                                                                               .branch_root_entry_id = "entry_assistant",
                                                                                               .branch_tip_entry_id = "entry_user",
                                                                                               .summary = "bad range",
                                                                                               .provider = "openai",
                                                                                               .model = "gpt-test",
                                                                                               .reason = "test",
                                                                                               .read_limits = ava::session::SessionReadLimits{},
                                                                                               .actor = "test"});
  expect(!root_after_tip && root_after_tip.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "branch summary rejects root entries after tip entries");

  auto missing_tip = ava::session::append_branch_summary(ava::session::BranchSummaryOptions{.workspace_dir = workspace,
                                                                                            .root_dir = sessions_dir,
                                                                                            .source_session_id = "session_source",
                                                                                            .branch_root_entry_id = "entry_user",
                                                                                            .branch_tip_entry_id = "missing",
                                                                                            .summary = "missing tip",
                                                                                            .provider = "openai",
                                                                                            .model = "gpt-test",
                                                                                            .reason = "test",
                                                                                            .read_limits = ava::session::SessionReadLimits{},
                                                                                            .actor = "test"});
  expect(!missing_tip && missing_tip.error().category() == ava::core::ErrorCategory::NotFound, "branch summary rejects missing tip entries");

  auto bad_provider = ava::session::append_branch_summary(ava::session::BranchSummaryOptions{.workspace_dir = workspace,
                                                                                             .root_dir = sessions_dir,
                                                                                             .source_session_id = "session_source",
                                                                                             .branch_root_entry_id = "entry_user",
                                                                                             .branch_tip_entry_id = "entry_assistant",
                                                                                             .summary = "summary with\nallowed whitespace",
                                                                                             .provider = "open\nai",
                                                                                             .model = "gpt-test",
                                                                                             .reason = "test",
                                                                                             .read_limits = ava::session::SessionReadLimits{},
                                                                                             .actor = "test"});
  expect(!bad_provider && bad_provider.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "branch summary rejects control bytes in provider metadata while allowing summary newlines");

  auto staged = append_session_entry_for_test(
      source_store, ava::session::SessionEntry{
                        .id = "entry_staged",
                        .parent_id = source_entries_after->back().id,
                        .type = ava::session::EntryType::AssistantOutputItem,
                        .timestamp = "2026-04-27T00:00:03Z",
                        .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"summary_staged\",\"sequence\":0,\"kind\":\"text\",\"text\":\"staged\","
                                     "\"assistant_phase\":\"commentary\"}"});
  auto incomplete_source = ava::session::append_branch_summary(ava::session::BranchSummaryOptions{.workspace_dir = workspace,
                                                                                                  .root_dir = sessions_dir,
                                                                                                  .source_session_id = "session_source",
                                                                                                  .branch_root_entry_id = "entry_user",
                                                                                                  .branch_tip_entry_id = "entry_assistant",
                                                                                                  .summary = "must not append after a staged output",
                                                                                                  .provider = "openai",
                                                                                                  .model = "gpt-test",
                                                                                                  .reason = "test",
                                                                                                  .read_limits = ava::session::SessionReadLimits{},
                                                                                                  .actor = "test"});
  auto after_incomplete = source_store.load();
  auto incomplete_summary_message =
      std::string("direct branch summary appends refuse even an incomplete assistant-output suffix without adding an interior record");
  if (!staged)
    incomplete_summary_message += ": staged fixture append failed: " + staged.error().format();
  else if (incomplete_source)
    incomplete_summary_message += ": branch summary unexpectedly appended";
  else if (incomplete_source.error().category() != ava::core::ErrorCategory::Session)
    incomplete_summary_message += ": branch summary error: " + incomplete_source.error().format();
  else if (!after_incomplete)
    incomplete_summary_message += ": source reload failed: " + after_incomplete.error().format();
  expect(staged && !incomplete_source && incomplete_source.error().category() == ava::core::ErrorCategory::Session && after_incomplete &&
             after_incomplete->size() == source_entries_after->size() + 1 && after_incomplete->back().id == "entry_staged",
         incomplete_summary_message);
}

void test_session_branch_summary_coverage_bounds_and_duplicates()
{
  auto const root = create_empty_root("session-branch-summary-coverage");
  auto const workspace = root / "workspace";
  auto const sessions_dir = root / "sessions";
  std::filesystem::create_directories(workspace);
  auto store =
      ava::session::SessionStore(ava::session::SessionStoreOptions{.root_dir = sessions_dir, .workspace_dir = workspace, .session_id = "session_coverage"});

  auto ordinary = [](std::string id, std::string parent, ava::session::EntryType type, std::string text) {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = std::move(parent),
                                      .type = type,
                                      .timestamp = "2026-08-04T00:00:00Z",
                                      .data_json = "{\"text\":\"" + std::move(text) + "\"}"};
  };
  auto start = ordinary("fork", "", ava::session::EntryType::SessionStart, "start");
  start.data_json =
      "{\"mode\":\"build\",\"provider\":\"openai\",\"model\":\"gpt-test\",\"context_sources\":0,"
      "\"context_window_tokens\":128000,\"max_output_tokens\":4096,\"prompt_override\":false,\"supports_tools\":true,"
      "\"supports_streaming\":true,\"supports_reasoning\":true,\"reports_usage\":true}";
  expect(append_session_entry_for_test(store, start) &&
             append_session_entry_for_test(store, ordinary("root", "fork", ava::session::EntryType::UserMessage, "question")) &&
             append_session_entry_for_test(store, ordinary("tip", "root", ava::session::EntryType::AssistantMessage, "answer")),
         "branch summary coverage fixture creates a substantive suffix");

  auto options = [&](std::string tip, std::string summary) {
    return ava::session::BranchSummaryOptions{.workspace_dir = workspace,
                                              .root_dir = sessions_dir,
                                              .source_session_id = "session_coverage",
                                              .branch_root_entry_id = "root",
                                              .branch_tip_entry_id = std::move(tip),
                                              .summary = std::move(summary),
                                              .provider = "openai",
                                              .model = "gpt-test",
                                              .reason = "test",
                                              .read_limits = ava::session::SessionReadLimits{},
                                              .actor = "test"};
  };
  auto read_bytes = [&] {
    std::ifstream input(store.session_path(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };

  auto missing_options = options("tip", "missing limits");
  missing_options.read_limits.reset();
  auto missing_prepare = ava::session::prepare_branch_summary(missing_options);
  auto missing_append = ava::session::append_branch_summary(std::move(missing_options));
  auto tiny_options = options("tip", "tiny bounds");
  tiny_options.read_limits = ava::session::SessionReadLimits{.max_file_bytes = 1, .max_line_bytes = 1, .max_entries = 1};
  auto tiny_prepare = ava::session::prepare_branch_summary(std::move(tiny_options));
  auto canceled_options = options("tip", "canceled");
  canceled_options.cancel_requested = [] { return true; };
  auto canceled = ava::session::prepare_branch_summary(std::move(canceled_options));
  expect(!missing_prepare && !missing_append && missing_prepare.error().message() == "branch summary requires explicit session read limits" && !tiny_prepare &&
             !canceled,
         "branch summary preparation and append require explicit bounded limits and honor bounded-read cancellation");

  auto lease = ava::session::SessionLease::acquire(store.session_path());
  auto tiny_target = lease ? ava::session::SessionAppendTarget::create_persistent(
                                 store, *lease, ava::session::SessionReadLimits{.max_file_bytes = 1, .max_line_bytes = 1, .max_entries = 1})
                           : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(lease.error()));
  expect(!tiny_target, "persistent append-target initialization uses its explicit bounded read policy");
  lease = ava::session::SessionLease{};

  auto initial = store.load();
  if (!initial)
  {
    expect(false, "branch summary coverage fixture loads");
    return;
  }
  auto coverage = ava::session::inspect_branch_summary_coverage(*initial, "session_coverage", "fork");
  auto prompt = ava::session::extract_branch_summary_prompt_range(*initial, coverage);
  auto missing_fork = ava::session::inspect_branch_summary_coverage(*initial, "session_coverage", "missing");
  expect(coverage.eligible() && coverage.branch_root_entry_id == "root" && coverage.branch_tip_entry_id == "tip" && prompt && prompt->size() == 2 &&
             prompt->front().id == "root" && prompt->back().id == "tip" &&
             missing_fork.reason == ava::session::BranchSummaryEligibilityReason::ForkEntryNotFound,
         "coverage is fork-exclusive, substantive-tip-inclusive, and reports a typed missing-fork reason");

  auto first = ava::session::append_branch_summary(options("tip", "first summary"));
  auto const bytes_after_first = read_bytes();
  auto duplicate = ava::session::append_branch_summary(options("tip", "replacement must be ignored"));
  auto after_duplicate = store.load();
  auto trailing =
      after_duplicate ? ava::session::inspect_branch_summary_coverage(*after_duplicate, "session_coverage", "fork") : ava::session::BranchSummaryCoverage{};
  auto trailing_prompt = after_duplicate ? ava::session::extract_branch_summary_prompt_range(*after_duplicate, trailing)
                                         : ava::core::Result<std::vector<ava::session::SessionEntry>>(
                                               std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture load failed")));
  expect(first && first->disposition == ava::session::BranchSummaryDisposition::Appended && duplicate &&
             duplicate->disposition == ava::session::BranchSummaryDisposition::Existing && duplicate->entry.id == first->entry.id &&
             read_bytes() == bytes_after_first && trailing.reason == ava::session::BranchSummaryEligibilityReason::ExistingSummary &&
             trailing.branch_tip_entry_id == "tip" && trailing.existing_summary && trailing_prompt && trailing_prompt->size() == 2,
         "exact duplicates are byte-identical and trailing summary markers preserve the same Existing coverage");

  if (!first)
    return;
  expect(append_session_entry_for_test(store, ordinary("advanced_tip", first->entry.id, ava::session::EntryType::UserMessage, "later work")).has_value(),
         "branch summary coverage fixture appends later substantive work");
  auto advanced_entries = store.load();
  auto advanced =
      advanced_entries ? ava::session::inspect_branch_summary_coverage(*advanced_entries, "session_coverage", "fork") : ava::session::BranchSummaryCoverage{};
  auto advanced_prompt = advanced_entries ? ava::session::extract_branch_summary_prompt_range(*advanced_entries, advanced)
                                          : ava::core::Result<std::vector<ava::session::SessionEntry>>(
                                                std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture load failed")));
  expect(advanced.eligible() && advanced.branch_root_entry_id == "root" && advanced.branch_tip_entry_id == "advanced_tip" && advanced_prompt &&
             advanced_prompt->size() == 3 &&
             std::ranges::none_of(*advanced_prompt, [](auto const& entry) { return entry.type == ava::session::EntryType::BranchSummary; }),
         "interspersed summaries never enter the prompt range and later substantive work advances the candidate tip");

  auto prepared = ava::session::prepare_branch_summary(options("advanced_tip", "advanced summary"));
  auto const bytes_before_failure = read_bytes();
  int write_attempts = 0;
  store.set_append_write_for_test([&](int, std::string_view) {
    ++write_attempts;
    errno = EIO;
    return static_cast<ssize_t>(-1);
  });
  auto failure_lease = ava::session::SessionLease::acquire(store.session_path());
  auto failure_target = failure_lease ? ava::session::SessionAppendTarget::create_persistent(store, *failure_lease, ava::session::SessionReadLimits{})
                                      : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(failure_lease.error()));
  auto failed_append = prepared && failure_target ? (*failure_target)->append_branch_summary_if_absent(prepared->entry)
                                                  : ava::core::Result<ava::session::SessionConditionalAppendResult>(
                                                        std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture setup failed")));
  store.set_append_write_for_test({});
  if (failure_target)
    failure_target->reset();
  failure_lease = ava::session::SessionLease{};
  expect(!failed_append && failed_append.error().format().find("append_commit_state: not_started") != std::string::npos && write_attempts == 1 &&
             read_bytes() == bytes_before_failure,
         "conditional branch summary append preserves commit state, leaves bytes unchanged, and never retries a failed write");

  auto appended = ava::session::append_branch_summary(options("advanced_tip", "advanced summary"));
  auto const bytes_after_advanced = read_bytes();
  auto append_lease = ava::session::SessionLease::acquire(store.session_path());
  auto target = append_lease ? ava::session::SessionAppendTarget::create_persistent(store, *append_lease, ava::session::SessionReadLimits{})
                             : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(append_lease.error()));
  auto rechecked = prepared && target ? (*target)->append_branch_summary_if_absent(prepared->entry)
                                      : ava::core::Result<ava::session::SessionConditionalAppendResult>(
                                            std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture setup failed")));
  auto final_entries = store.load();
  auto no_suffix =
      final_entries ? ava::session::inspect_branch_summary_coverage(*final_entries, "session_coverage", "advanced_tip") : ava::session::BranchSummaryCoverage{};
  expect(prepared && prepared->disposition == ava::session::BranchSummaryDisposition::Appended && appended &&
             appended->disposition == ava::session::BranchSummaryDisposition::Appended && rechecked &&
             rechecked->disposition == ava::session::SessionAppendDisposition::Existing && rechecked->entry.id == appended->entry.id &&
             read_bytes() == bytes_after_advanced && no_suffix.reason == ava::session::BranchSummaryEligibilityReason::NoSubstantiveEntriesAfterFork &&
             ava::session::branch_summary_eligibility_reason_text(no_suffix.reason).find('/') == std::string_view::npos,
         "append-time revalidation suppresses a cross-caller duplicate and summary-only suffixes have a typed path-free reason");
}

}  // namespace session_tests
