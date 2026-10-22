#include "sys.h"
#include "tests/app_rpc_test_cases.h"
#include "tests/support/test_env.h"

void run_app_rpc_tests()
{
  ava::tests::HermeticTestEnvironment environment("ava-app-rpc-");
  ava::tests::app_rpc_test::test_app_rpc_protocol_bounds();
  ava::tests::app_rpc_test::test_app_rpc_message_entry_count_boundary();
  ava::tests::app_rpc_test::test_app_rpc_prompt_payload_serialization();
  ava::tests::app_rpc_test::test_app_rpc_prompt_result_tool_timeline_golden_payloads();
  ava::tests::app_rpc_test::test_app_rpc_parsing_and_response_serialization();
  ava::tests::app_rpc_test::test_app_rpc_identifier_validation();
  ava::tests::app_rpc_test::test_app_rpc_prompt_with_fake_transport_streams_events();
  ava::tests::app_rpc_test::test_app_rpc_offline_allows_local_protocol_and_rejects_prompt_before_provider_request();
  ava::tests::app_rpc_test::test_app_rpc_prompt_imports_image_attachments();
  ava::tests::app_rpc_test::test_app_rpc_prompt_imports_inline_image_uploads();
  ava::tests::app_rpc_test::test_app_rpc_prompt_rejects_inline_image_upload_mime_mismatch();
  ava::tests::app_rpc_test::test_app_rpc_prompt_streams_provider_deltas_before_final_response();
  ava::tests::app_rpc_test::test_app_rpc_prompt_retry_transport_cancellation_is_canceled_event();
  ava::tests::app_rpc_test::test_app_rpc_prompt_after_idle_cancel_clears_cancel_flag();
  ava::tests::app_rpc_test::test_app_rpc_prompt_refreshes_expired_oauth_before_provider_request();
  ava::tests::app_rpc_test::test_app_rpc_prompt_uses_pinned_catalog_credential_policy();
  ava::tests::app_rpc_test::test_app_rpc_malformed_line_recovery_and_unknown_command();
  ava::tests::app_rpc_test::test_app_rpc_plugin_command_has_no_tui_authority();
  ava::tests::app_rpc_test::test_app_rpc_state_list_sessions_and_open_session();
  ava::tests::app_rpc_test::test_app_rpc_job_controls_are_active_safe_and_redacted();
  ava::tests::app_rpc_test::test_app_rpc_current_session_reads_reject_path_replacement();
  ava::tests::app_rpc_test::test_app_rpc_session_metadata_name_and_labels();
  ava::tests::app_rpc_test::test_app_rpc_session_tree_command_and_switch_navigation();
  ava::tests::app_rpc_test::test_app_rpc_session_fork_and_clone_commands();
  ava::tests::app_rpc_test::test_app_rpc_branch_construction_failure_rolls_back_created_file();
  ava::tests::app_rpc_test::test_app_rpc_noncurrent_branch_source_recovers_torn_tail();
  ava::tests::app_rpc_test::test_app_rpc_maintenance_reservation_blocks_generic_mutation();
  ava::tests::app_rpc_test::test_app_rpc_summarize_branch_appends_to_source_session();
  ava::tests::app_rpc_test::test_app_rpc_model_commands();
  ava::tests::app_rpc_test::test_app_rpc_reasoning_commands();
  ava::tests::app_rpc_test::test_app_rpc_reasoning_model_serialization_exposes_resolved_maps();
  ava::tests::app_rpc_test::test_app_rpc_protocol_version_and_session_commands();
  ava::tests::app_rpc_test::test_app_rpc_messages_keep_v1_payloads_when_ordered_output_does_not_fit();
  ava::tests::app_rpc_test::test_app_rpc_protocol_version_and_resolver_reply_errors();
  ava::tests::app_rpc_test::test_app_rpc_mcp_command_responses();
  ava::tests::app_rpc_test::test_app_rpc_command_responses_for_context_compact_export();
  ava::tests::app_rpc_test::test_app_rpc_direct_run_command_permission_reply_executes_and_audits();
  ava::tests::app_rpc_test::test_app_rpc_direct_run_command_permission_denial_blocks_execution();
  ava::tests::app_rpc_test::test_app_rpc_direct_run_command_active_rejects_and_cancels_process();
  ava::tests::app_rpc_test::test_app_rpc_compact_provider_failure_is_error_response();
  ava::tests::app_rpc_test::test_app_rpc_production_catalog_and_golden_contract();
  ava::tests::app_rpc_test::test_app_rpc_contract_validation_regressions();
  ava::tests::app_rpc_test::test_app_rpc_utf8_recovery_and_framing();
  ava::tests::app_rpc_test::test_app_rpc_terminal_publication_gates_prompt_id_reuse();
  ava::tests::app_rpc_test::test_app_rpc_parent_terminal_precedes_queued_follow_up_start();
  ava::tests::app_rpc_test::test_app_rpc_eof_during_blocked_parent_publication_skips_follow_up();
  ava::tests::app_rpc_test::test_app_rpc_terminal_publication_gates_direct_and_compaction_runs();
  ava::tests::app_rpc_test::test_app_rpc_worker_output_failure_wakes_blocked_input();
  ava::tests::app_rpc_test::test_app_rpc_mode_forwards_nonstdin_wake();
  ava::tests::app_rpc_test::test_app_rpc_posix_line_reader_wake_eof_and_fd_lifetime();
  ava::tests::app_rpc_test::test_app_rpc_stream_line_reader_terminal_outcomes();
  ava::tests::app_rpc_test::test_app_rpc_stream_reader_wake_badbit_is_canceled();
  ava::tests::app_rpc_test::test_app_rpc_unterminated_final_command_executes();
  ava::tests::app_rpc_test::test_app_rpc_newline_terminated_oversized_line_recovers();
  ava::tests::app_rpc_test::test_app_rpc_compact_cancellation_is_error_response_without_provider_request();
}
