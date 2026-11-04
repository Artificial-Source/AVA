// Run editor and feature suites under one Application-owned terminal.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/tui_composer_test_suites.h"

// Check edit-time cursor validity and immediate interrupt publication.
void run_tui_composer_invariant_tests();

// Protect shared key policy and its idle versus active reset rules.
void run_tui_composer_policy_tests();

// Verify active controller precedence with a held worker and real feature owners.
void run_tui_active_input_route_tests();

// Verify narrow frontend access, stable binding borrows and Settings refresh timing.
void run_tui_feature_interface_tests();

// Direct selector/summary contracts use the same initialized test-thread terminal.
void run_tui_feature_owner_tests();
// Exercise timer discovery through the explicit application-service boundary.
void run_tui_branch_summary_timer_tests();
// Direct viewport contracts retire all borrowed owners before returning here.
void run_tui_transcript_view_tests();

// Run editor and feature regressions under the shared initialized terminal fixture.
void run_tui_composer_tests()
{
  ScopedEnvVar no_color_guard("NO_COLOR", "");
  ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
  ScopedEnvVar colorfgbg_guard("COLORFGBG", "");
  ScopedEnvVar tmux_hyperlinks_guard("AVA_TUI_TMUX_HYPERLINKS", "");
  run_tui_command_output_tests();
  run_tui_terminal_image_tests();
  run_tui_terminal_input_tests_part_1();
  run_tui_permission_tests_part_1();
  run_tui_modal_tests_part_1();
  run_tui_terminal_input_tests_part_2();
  run_tui_composer_rendering_tests_part_1();
  run_tui_markdown_tests();
  run_tui_mermaid_tests();
  run_tui_composer_rendering_tests_part_2();
  run_tui_completion_tests();
  run_tui_keybinding_tests();
  run_tui_composer_dispatch_tests();
  run_tui_composer_invariant_tests();
  run_tui_composer_policy_tests();
  run_tui_active_input_route_tests();
  run_tui_prompt_stash_tests();
  run_tui_modal_tests_part_2();
  run_tui_permission_tests_part_2();
  run_tui_composer_rendering_tests_part_3();
  run_tui_permission_tests_part_3();
  run_tui_modal_tests_part_3();
  run_tui_selector_tests();
  run_tui_feature_owner_tests();
  run_tui_feature_interface_tests();
  run_tui_branch_summary_timer_tests();
  run_tui_transcript_view_tests();
  run_tui_transcript_tests_part_1();
  run_tui_transcript_selection_tests();
  run_tui_transcript_search_tests();
  run_tui_tool_card_tests_part_1();
  run_tui_composer_rendering_tests_part_4();
  run_tui_session_grant_tests();
  run_tui_text_model_conversion_tests();
  run_tui_runtime_event_state_tests();
  run_tui_terminal_osc11_theme_tests();
  run_tui_large_render_performance_tests();
  run_tui_tool_card_detail_tests();
  run_tui_transcript_hierarchy_tests();
  run_tui_runtime_dispatch_tests();
  run_tui_transcript_cache_tests();
}
