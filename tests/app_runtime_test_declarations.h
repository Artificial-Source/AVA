#pragma once

#include "ava/app/command_catalog.h"
#include "ava/app/runtime/Session.h"
#include "ava/config/xdg_paths.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ava::tests::app_runtime_tests {

void test_command_classification();
void test_repository_build_test_headless_decision_matrix();
void test_app_event_serialization();
void test_extension_resource_policy_derives_synthetic_paths_and_trust();
void test_app_runtime_open_session_and_context_prompt();
void test_app_runtime_preserves_legacy_subagent_job_tree();
void test_app_active_context_status_format_semantics();
void test_app_active_context_status_tracks_compaction_projection();
void test_app_runtime_no_session_mode();
// Verify inherited runtime state and retained frontend policy in replacement contexts.
void test_app_runtime_replacement_open_context();
void test_app_runtime_process_scope_lifecycle();
void test_app_runtime_session_startup_options();
void test_app_runtime_recovers_torn_tail_before_resume_and_startup_fork();
void test_app_runtime_reconciles_committed_function_calls_on_resume();
void test_app_runtime_cli_prompt_overrides();
void test_app_runtime_project_trust_malformed_diagnostics();
void test_app_runtime_enabled_plugin_resources_autoload();
void test_app_runtime_project_plugin_resources_follow_trust_gate();
void test_app_runtime_enabled_plugin_resource_failures_are_context_visible();
void test_app_runtime_plugin_install_remove_commands();
void test_runtime_plugin_event_hook_uses_admitted_run_cancellation_scope();
void test_app_context_reports_lsp_config_load_errors();
void test_debug_session_mutex_tracks_current_thread();
void test_app_run_prompt_isolates_ambient_extensions();
void test_project_primary_revocation_removes_authority_without_broadening_tools();
void test_clear_trust_retires_only_for_effective_untrusted_state();
void test_untrusted_mutation_rejects_active_run_and_append_before_write();
void test_post_persistence_publication_failure_leaves_reopen_required();
void test_session_construction_linearizes_with_workspace_revocation();
void test_workspace_revocation_retires_retained_sessions_transactionally();
void test_app_run_prompt_sources_private_launch_display_from_runtime_invocation();
void test_app_run_prompt_emits_events();
void test_app_run_prompt_expands_file_references();
void test_app_run_prompt_sends_imported_image_attachment();
void test_app_clipboard_image_file_override_imports_attachment();
void test_app_run_prompt_emits_provider_retry_events_when_enabled();
void test_app_run_prompt_observation_shares_context_across_compaction_and_retry();
void test_app_run_prompt_emits_tool_progress_and_session_spill();
void test_app_first_run_auth_onboarding();
void test_app_run_prompt_event_sink_failure_cancels_before_next_provider_call();
void test_app_run_prompt_rejects_invalid_parallel_read_search_options();
void test_app_run_prompt_parallel_read_search_groups_eligible_tools_and_honors_barrier();
void test_tui_request_presentation_capture();
void test_app_command_dispatcher();
void test_startup_overview_snapshot_bounds_order_redaction();
void test_startup_overview_bounded_lower_bound_counts();
void test_app_session_jsonl_import_export_portable_attachments();
void test_app_session_jsonl_export_sanitizes_private_reasoning_replay_metadata();
void test_app_session_branch_commands();
void test_app_session_fork_from_entry_and_user_turns();
void test_app_session_new_resume_commands();
void test_app_session_metadata_commands();
void test_request_projection_validates_committed_v4_history();
void test_unlocked_data_access_and_notifications();
void test_runtime_model_switch_accepts_committed_openai_responses_reasoning();
void test_application_catalog_cache_reuses_workspace_and_session_indexes();
void test_application_catalog_coordinator_serializes_refresh_and_snapshot();
void test_application_catalog_current_session_incremental_refresh();
void test_app_runtime_model_switch_persists_and_reopens();
void test_app_runtime_model_switch_projects_incompatible_history_at_request_time();
void test_app_runtime_reasoning_selection_persists_and_requests();
void test_app_runtime_branch_construction_failure_rolls_back_created_file();
void test_app_runtime_initial_reasoning_level_option();

// Exercise UI command dispatch using unlocked_session and the supplied fixture paths, workspace, and custom hotkeys.
//
// The function acquires write access while commands inspect or update session-backed UI state.
void app_command_dispatcher_ui_part(ava::app::runtime::session_ts& unlocked_session, ava::config::XdgPaths const& paths, std::filesystem::path const& workspace,
                                    std::vector<ava::app::CommandHotkey> const& custom_hotkeys);
// Exercise command catalog and dispatcher behavior using unlocked_session, paths, workspace, and custom_hotkeys.
//
// The function acquires session access only around state-dependent work and releases it before waits that can join background workers.
void app_command_dispatcher_catalog_part(ava::app::runtime::session_ts& unlocked_session, ava::config::XdgPaths const& paths,
                                         std::filesystem::path const& workspace, std::vector<ava::app::CommandHotkey> const& custom_hotkeys);
// Exercise authentication command dispatch using unlocked_session and the original plan_system_prompt.
//
// The function acquires write access while commands update session mode, prompt, and credentials.
void app_command_dispatcher_auth_part(ava::app::runtime::session_ts& unlocked_session, std::string const& plan_system_prompt);
// Exercise tool command dispatch using unlocked_session and workspace.
//
// The function acquires write access while tool and compaction commands inspect or mutate session state.
void app_command_dispatcher_tool_part(ava::app::runtime::session_ts& unlocked_session, std::filesystem::path const& workspace);
// Exercise session command dispatch using unlocked_session and workspace.
//
// The function acquires write access while export, import, statistics, and lifecycle commands inspect or replace session state.
void app_command_dispatcher_session_part(ava::app::runtime::session_ts& unlocked_session, std::filesystem::path const& workspace);

}  // namespace ava::tests::app_runtime_tests
