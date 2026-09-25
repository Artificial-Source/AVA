#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/app/command_catalog.h"
#include "ava/app/command_palette.h"
#include "ava/app/command_sessions.h"
#include "ava/app/interactive_internal.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/session_title_coordinator.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/session/session_metadata.h"
#include "ava/session/session_store.h"
#include "ava/core/error.h"
#include "ava/core/result.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

void test_application_catalog_cache_reuses_workspace_and_session_indexes()
{
  auto const root = temp_root() / "application-catalog-cache";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "application catalog cache test opens a runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  std::size_t workspace_walks = 0;
  std::size_t session_tree_builds = 0;
  auto workspace_walker = [&](ava::app::runtime::session_ts const&) {
    ++workspace_walks;
    return std::vector<ava::app::WorkspacePathCandidate>{
        ava::app::WorkspacePathCandidate{.value = "src/", .description = "directory", .directory = true},
        ava::app::WorkspacePathCandidate{.value = "src/main.cpp", .description = "file 20 bytes", .directory = false},
        ava::app::WorkspacePathCandidate{.value = "my folder/space file.txt", .description = "file 5 bytes", .directory = false}};
  };
  auto tree_builder = [&](ava::app::runtime::session_ts const& unlocked_current) -> ava::core::Result<ava::session::SessionTreeIndex> {
    ++session_tree_builds;
    auto current_snapshot = [&] {
      SCOPED_CRITICAL_AREA_CR(current_r, unlocked_current);
      return std::tuple{current_r->store.session_id(), current_r->store.session_path().string()};
    }();
    auto const& [current_session_id, current_session_path] = current_snapshot;
    ava::session::SessionTreeIndex tree;
    tree.current_session_id = current_session_id;
    tree.roots = {current_session_id};
    tree.leaves = tree.roots;
    tree.current_path = tree.roots;
    tree.sessions.push_back(ava::session::SessionTreeNode{.summary = ava::session::SessionSummary{.session_id = current_session_id,
                                                                                                   .path = current_session_path,
                                                                                                  .last_updated = "2026-07-22T00:00:00Z",
                                                                                                  .entry_count = 1},
                                                          .metadata = {},
                                                          .children = {},
                                                          .current = true});
    return tree;
  };

  auto cache = ava::app::build_application_catalog_cache(unlocked_session, {}, workspace_walker, tree_builder);
  auto const session_id = ava::app::runtime::session_ts::rat(unlocked_session)->store.session_id();
  auto const* read_item = tui_test_support::find_slash_command_item(cache.slash_commands, "/read");
  auto const* sessions_item = tui_test_support::find_slash_command_item(cache.slash_commands, "/sessions");
  auto const* resume_item = tui_test_support::find_slash_command_item(cache.slash_commands, "/resume");
  expect(workspace_walks == 1 && session_tree_builds == 1 && cache.operations.workspace_walks == 1 && cache.operations.session_tree_builds == 1 &&
             tui_test_support::has_slash_argument_completion(read_item, 0, "src/main.cpp") &&
             !tui_test_support::has_slash_argument_completion(read_item, 0, "my folder/space file.txt") &&
             std::ranges::any_of(cache.file_references, [](auto const& item) { return item.value == "my folder/space file.txt"; }) &&
              tui_test_support::has_slash_argument_completion(sessions_item, 0, session_id) &&
              tui_test_support::has_slash_argument_completion(resume_item, 0, session_id),
         "one application catalog build walks the workspace and session tree once while feeding both completion surfaces");

  for (int pass = 0; pass < 100; ++pass)
  {
    auto slash_snapshot = cache.slash_commands;
    auto reference_snapshot = cache.file_references;
    auto model_view = ava::app::model_selector_view_1(unlocked_session, {});
    static_cast<void>(slash_snapshot);
    static_cast<void>(reference_snapshot);
    static_cast<void>(model_view);
  }
  auto recent = ava::app::session_selector_view(*cache.session_tree, ava::app::SessionSelectorSort::Recent, {});
  auto named = ava::app::session_selector_view(*cache.session_tree, ava::app::SessionSelectorSort::Name, {}, true, false, true, true);
  auto path = ava::app::session_selector_view(*cache.session_tree, ava::app::SessionSelectorSort::Path, {}, false, true, false, false);
  expect(workspace_walks == 1 && session_tree_builds == 1 && !recent.items.empty() && !named.items.empty() && !path.items.empty(),
         "state snapshots and every selector filter or toggle reuse cached catalogs without filesystem enumeration");

  auto const operation_counts_before_retarget = cache.operations;
  ava::app::retarget_application_session(cache, session_id);
  expect(cache.operations.workspace_walks == operation_counts_before_retarget.workspace_walks &&
             cache.operations.session_tree_builds == operation_counts_before_retarget.session_tree_builds &&
             cache.operations.value_refreshes == operation_counts_before_retarget.value_refreshes,
         "current-session retarget changes only cached tree values and performs no catalog rebuild");

  ava::app::refresh_application_catalog_values(cache, unlocked_session, {});
  expect(workspace_walks == 1 && session_tree_builds == 1, "display and model-equivalent catalog refreshes do not walk workspace or session metadata");
  ava::app::refresh_application_workspace_catalog(cache, unlocked_session, {}, workspace_walker);
  expect(workspace_walks == 2 && session_tree_builds == 1 && cache.operations.workspace_walks == 2,
         "explicit workspace mutation invalidation performs exactly one fresh walk");
  ava::app::refresh_application_session_tree(cache, unlocked_session, {}, tree_builder);
  expect(workspace_walks == 2 && session_tree_builds == 2 && cache.operations.session_tree_builds == 2,
         "explicit session metadata mutation invalidation performs exactly one fresh tree build");

  auto parent_hint_cache = cache;
  auto& parent_hint_tree = *parent_hint_cache.session_tree;
  auto& current_node = parent_hint_tree.sessions.front();
  current_node.metadata.parent_session_id = "opaque-parent";
  parent_hint_tree.roots = {"opaque-parent"};
  parent_hint_tree.leaves = {current_node.summary.session_id};
  parent_hint_tree.current_path = {"opaque-parent", current_node.summary.session_id};
  parent_hint_tree.sessions.push_back(ava::session::SessionTreeNode{.summary = ava::session::SessionSummary{.session_id = "opaque-parent",
                                                                                                            .path = "/private/raw-parent.jsonl",
                                                                                                            .last_updated = "2026-07-21T00:00:00Z",
                                                                                                            .entry_count = 4,
                                                                                                            .title = "Résumé parent"},
                                                                    .metadata = {},
                                                                    .children = {current_node.summary.session_id},
                                                                    .current = false});
  ava::app::ApplicationCatalogCoordinator parent_hint_catalog(std::move(parent_hint_cache));
  auto const bound_parent_view = parent_hint_catalog.session_view(ava::app::SessionSelectorSort::Recent, {}, false, false, false, false, "F8");
  auto const unbound_parent_view = parent_hint_catalog.session_view();
  auto const bound_parent = std::ranges::find_if(bound_parent_view.items, [](auto const& item) { return item.value == "opaque-parent"; });
  auto const unbound_parent = std::ranges::find_if(unbound_parent_view.items, [](auto const& item) { return item.value == "opaque-parent"; });
  auto const source_summary = parent_hint_catalog.session_summary("opaque-parent");
  auto const missing_summary = parent_hint_catalog.session_summary("not-present");
  expect(bound_parent != bound_parent_view.items.end() && bound_parent->detail.find("F8 summarize abandoned parent") != std::string::npos &&
             bound_parent->detail.find("/private/raw-parent.jsonl") == std::string::npos && unbound_parent != unbound_parent_view.items.end() &&
             unbound_parent->detail.find("bind app.sessions.summarizeParent") != std::string::npos && source_summary && *source_summary &&
             (**source_summary).path == "/private/raw-parent.jsonl" && missing_summary && !*missing_summary,
         "application catalog marks only the current direct parent with bound/unbound summary hints while retaining source authority behind the app seam");

  auto const mapped_branch_snapshot = ava::app::interactive_internal::tui_branch_summary_snapshot(
      ava::app::BranchSummarySnapshot{.generation = 42,
                                      .phase = ava::app::BranchSummaryPhase::Failed,
                                      .source_label = "Résumé parent",
                                      .model_label = "Model β",
                                      .eligibility_code = ava::app::BranchSummaryEligibilityCode::NotDirectSource,
                                      .failure_code = ava::app::BranchSummaryFailureCode::AppendFailed,
                                      .reason = "/private/raw-parent.jsonl provider-secret generated-summary",
                                      .append_commit_state = "partial_or_unknown",
                                      .refresh_required = true});
  auto const mapped_branch_view = ava::tui::runtime_views::branch_summary_operation_view(mapped_branch_snapshot);
  auto const mapped_branch_text = tui_test_support::join_visible_lines(ava::tui::detail::render_select_list_modal(mapped_branch_view, 72, 8));
  expect(mapped_branch_snapshot.generation == 42 && mapped_branch_snapshot.phase == ava::tui::TuiBranchSummaryPhase::Failed &&
             mapped_branch_snapshot.eligibility_code == ava::tui::TuiBranchSummaryEligibilityCode::NotDirectSource &&
             mapped_branch_snapshot.failure_code == ava::tui::TuiBranchSummaryFailureCode::AppendFailed &&
             mapped_branch_snapshot.append_commit_state == ava::tui::TuiBranchSummaryAppendCommitState::PartialOrUnknown &&
             mapped_branch_snapshot.refresh_required && mapped_branch_text.find("/private/raw-parent.jsonl") == std::string::npos &&
             mapped_branch_text.find("provider-secret") == std::string::npos && mapped_branch_text.find("generated-summary") == std::string::npos,
         "application branch-summary mapping forwards exact fixed enums and labels but drops backend reason, path, provider, and generated payload text");
}

void test_application_catalog_coordinator_serializes_refresh_and_snapshot()
{
  auto const root = temp_root() / "application-catalog-coordinator";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "application catalog coordinator test opens a runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  auto cache = ava::app::build_application_catalog_cache(unlocked_session, {}, [](ava::app::runtime::session_ts const&) {
    return std::vector<ava::app::WorkspacePathCandidate>{ava::app::WorkspacePathCandidate{.value = "old.cpp", .description = "old file", .directory = false}};
  });
  ava::app::ApplicationCatalogCoordinator catalog(std::move(cache));
  auto const initial_delivery = catalog.delivery_snapshot();
  expect(!initial_delivery.slash_commands.empty() && initial_delivery.file_references.size() == 1 &&
             initial_delivery.file_references.front().value == "old.cpp" && initial_delivery.slash_catalog_generation != 0 &&
             initial_delivery.workspace_catalog_generation != 0,
         "catalog coordinator delivers the complete initial completion generations exactly once");

  std::mutex mutex;
  std::condition_variable changed;
  bool walker_started = false;
  bool display_refresh_started = false;
  bool snapshot_started = false;
  bool release_walker = false;
  bool refresh_finished = false;
  std::thread refresh([&]() {
    catalog.refresh_workspace(unlocked_session, {}, [&](ava::app::runtime::session_ts const&) {
      std::unique_lock lock(mutex);
      walker_started = true;
      changed.notify_all();
      if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return release_walker; }))
        return std::vector<ava::app::WorkspacePathCandidate>{};
      return std::vector<ava::app::WorkspacePathCandidate>{ava::app::WorkspacePathCandidate{.value = "new.cpp", .description = "new file", .directory = false}};
    });
    std::lock_guard lock(mutex);
    refresh_finished = true;
    changed.notify_all();
  });

  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return walker_started; }), "catalog refresh reaches the deterministic blocked workspace walk");
  }
  auto display_refresh = std::async(std::launch::async, [&] {
    {
      std::lock_guard lock(mutex);
      display_refresh_started = true;
      changed.notify_all();
    }
    catalog.refresh_values(unlocked_session, {});
  });
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return display_refresh_started; }),
           "overlapping display refresh reaches the catalog operation boundary");
  }
  auto const display_refresh_serialized = display_refresh.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
  auto snapshot_future = std::async(std::launch::async, [&] {
    {
      std::lock_guard lock(mutex);
      snapshot_started = true;
      changed.notify_all();
    }
    return catalog.snapshot();
  });
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return snapshot_started; }), "overlapping snapshot reaches the catalog operation boundary");
  }
  auto const snapshot_serialized = snapshot_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;

  {
    std::lock_guard lock(mutex);
    release_walker = true;
    changed.notify_all();
  }
  {
    std::unique_lock lock(mutex);
    expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return refresh_finished; }), "catalog refresh completes before its finite deadline");
  }
  refresh.join();
  auto const display_refresh_finished = display_refresh.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  if (display_refresh_finished)
    display_refresh.get();
  auto const snapshot_finished = snapshot_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  auto during = snapshot_finished ? snapshot_future.get() : ava::app::ApplicationCatalogCache{};
  auto after = catalog.snapshot();
  expect(display_refresh_serialized && snapshot_serialized && display_refresh_finished && snapshot_finished && during.workspace_path_candidates.size() == 1 &&
             during.workspace_path_candidates.front().value == "new.cpp" && during.file_references.size() == 1 &&
             during.file_references.front().value == "new.cpp" && after.workspace_path_candidates.size() == 1 &&
             after.workspace_path_candidates.front().value == "new.cpp" && after.file_references.size() == 1 &&
             after.file_references.front().value == "new.cpp" && after.operations.workspace_walks == 2 && after.operations.value_refreshes == 3,
         "submit, display refresh, and snapshot operations serialize before one coherent locked catalog generation is published");
}

void test_application_catalog_current_session_incremental_refresh()
{
  auto const root = temp_root() / "application-catalog-current-session";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  auto other = ava::session::SessionStore::create(workspace, paths.sessions_dir);
  expect(unlocked_session_result.has_value() && other.has_value(), "incremental catalog refresh creates current and comparison sessions");
  if (!unlocked_session_result || !other)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  auto const session_id = ava::app::runtime::session_ts::rat(unlocked_session)->store.session_id();
  auto const append_target = ava::app::runtime::session_ts::rat(unlocked_session)->append_target();

  std::size_t workspace_walks = 0;
  auto cache = ava::app::build_application_catalog_cache(unlocked_session, {}, [&](ava::app::runtime::session_ts const&) {
    ++workspace_walks;
    return std::vector<ava::app::WorkspacePathCandidate>{};
  });
  ava::app::ApplicationCatalogCoordinator catalog(std::move(cache));
  auto const initial = catalog.snapshot();
  auto const initial_current =
      std::ranges::find_if(initial.session_tree->sessions, [&](auto const& node) { return node.summary.session_id == session_id; });
  auto const initial_current_entry_count = initial_current == initial.session_tree->sessions.end() ? std::size_t{0} : initial_current->summary.entry_count;

  ava::session::SessionMetadataUpdate fallback_update;
  fallback_update.actor = "auto-title";
  fallback_update.generated_title = "Fallback catalog title";
  auto fallback = ava::session::make_session_metadata_entry(std::move(fallback_update));
  auto activity = ava::session::SessionEntry{.id = "catalog_activity_1",
                                             .parent_id = fallback ? fallback->id : std::string{},
                                             .type = ava::session::EntryType::UserMessage,
                                             .timestamp = "2099-01-01T00:00:00Z",
                                             .data_json = R"({"text":"ordinary turn"})"};
  auto fallback_appended = fallback ? append_target->append(*fallback) : ava::core::VoidResult(std::unexpected(fallback.error()));
  auto activity_appended = fallback_appended ? append_target->append(activity) : fallback_appended;
  auto refreshed = activity_appended ? catalog.refresh_current_session(unlocked_session, {}) : ava::core::Result<bool>(std::unexpected(activity_appended.error()));
  auto after_fallback = catalog.snapshot();
  auto fallback_view = catalog.session_view(ava::app::SessionSelectorSort::Recent, {});
  auto current =
      std::ranges::find_if(after_fallback.session_tree->sessions, [&](auto const& node) { return node.summary.session_id == session_id; });
  expect(refreshed && *refreshed && current != after_fallback.session_tree->sessions.end() && current->summary.title == "Fallback catalog title" &&
             current->summary.entry_count == initial_current_entry_count + 2 && current->summary.last_updated == "2099-01-01T00:00:00Z" &&
              !fallback_view.items.empty() && fallback_view.items.front().value == session_id && workspace_walks == 1 &&
             after_fallback.operations.workspace_walks == initial.operations.workspace_walks &&
             after_fallback.operations.session_tree_builds == initial.operations.session_tree_builds &&
             after_fallback.operations.session_node_refreshes == initial.operations.session_node_refreshes + 1,
         "ordinary-turn incremental refresh exposes fallback title, current summary, and Recent ordering without workspace or tree walks");

  ava::session::SessionMetadataUpdate refined_update;
  refined_update.actor = "auto-title";
  refined_update.generated_title = "Refined catalog title";
  auto refined = ava::session::make_session_metadata_entry(std::move(refined_update), activity.id);
  auto refined_appended = refined ? append_target->append(*refined) : ava::core::VoidResult(std::unexpected(refined.error()));
  auto const title_changes = ava::app::SessionTitleCatalogChanges{.cursor = 7, .dirty_session_ids = {session_id}};
  auto generation_refresh =
      refined_appended ? catalog.refresh_title_changes(unlocked_session, title_changes) : ava::core::Result<bool>(std::unexpected(refined_appended.error()));
  auto duplicate_generation = generation_refresh ? catalog.refresh_title_changes(unlocked_session, title_changes) : generation_refresh;
  auto after_refinement = catalog.snapshot();
  auto refined_current =
      std::ranges::find_if(after_refinement.session_tree->sessions, [&](auto const& node) { return node.summary.session_id == session_id; });
  expect(generation_refresh && *generation_refresh && duplicate_generation && !*duplicate_generation &&
             refined_current != after_refinement.session_tree->sessions.end() && refined_current->summary.title == "Refined catalog title" &&
             after_refinement.operations.session_node_refreshes == after_fallback.operations.session_node_refreshes + 1 && workspace_walks == 1 &&
             after_refinement.operations.session_tree_builds == initial.operations.session_tree_builds,
         "asynchronous title generation is consumed once at catalog access and updates the cached selector without broad rescans");

  std::size_t topology_build_calls = 0;
  auto successful_tree_builder = [&](ava::app::runtime::session_ts const& unlocked_current) {
    ++topology_build_calls;
    SCOPED_CRITICAL_AREA_CR(current_r, unlocked_current);
    return ava::session::build_session_tree(current_r->workspace_dir(), current_r->paths().sessions_dir, current_r->store.session_id());
  };
  auto const captured_before_topology = ava::app::SessionTitleCatalogChanges{.cursor = 8, .dirty_session_ids = {"old_session_dirty_before_switch"}};
  auto topology_refresh = catalog.refresh_session_tree_and_consume_title_changes(unlocked_session, captured_before_topology, {}, successful_tree_builder);
  auto duplicate_after_topology = topology_refresh ? catalog.refresh_title_changes(unlocked_session, captured_before_topology, {}, successful_tree_builder)
                                                   : ava::core::Result<bool>(std::unexpected(topology_refresh.error()));
  auto const late_notification = ava::app::SessionTitleCatalogChanges{.cursor = 9, .dirty_session_ids = {session_id}};
  auto late_refresh = duplicate_after_topology ? catalog.refresh_title_changes(unlocked_session, late_notification, {}, successful_tree_builder)
                                               : ava::core::Result<bool>(std::unexpected(duplicate_after_topology.error()));

  auto const failed_capture = ava::app::SessionTitleCatalogChanges{.cursor = 10, .dirty_session_ids = {"old_session_dirty_before_failed_rebuild"}};
  auto failed_refresh = catalog.refresh_session_tree_and_consume_title_changes(
      unlocked_session, failed_capture, {}, [&](ava::app::runtime::session_ts const&) -> ava::core::Result<ava::session::SessionTreeIndex> {
        ++topology_build_calls;
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "deterministic topology rebuild failure"));
      });
  auto const cursor_after_failure = catalog.title_catalog_cursor();
  auto retry_refresh = catalog.refresh_session_tree_and_consume_title_changes(unlocked_session, failed_capture, {}, successful_tree_builder);
  auto const after_topology_retry = catalog.snapshot();
  auto retry_current = std::ranges::find_if(after_topology_retry.session_tree->sessions,
                                             [&](auto const& node) { return node.summary.session_id == session_id; });
  expect(topology_refresh && *topology_refresh && duplicate_after_topology && !*duplicate_after_topology && catalog.title_catalog_cursor() == 10 &&
             late_refresh && *late_refresh && cursor_after_failure == 9 && !failed_refresh && retry_refresh && *retry_refresh && topology_build_calls == 3 &&
             workspace_walks == 1 && after_topology_retry.operations.session_tree_builds == initial.operations.session_tree_builds + 3 &&
             retry_current != after_topology_retry.session_tree->sessions.end() && retry_current->summary.title == "Refined catalog title",
         "captured topology refreshes consume only their cursor, leave later notifications pending, avoid duplicate builds, and preserve failed cursors for "
         "retry");
}

}  // namespace ava::tests::app_runtime_tests
