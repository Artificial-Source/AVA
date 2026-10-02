#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/tui_composer_test_suites.h"

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
  run_tui_prompt_stash_tests();
  run_tui_modal_tests_part_2();
  run_tui_permission_tests_part_2();
  run_tui_composer_rendering_tests_part_3();
  run_tui_permission_tests_part_3();
  run_tui_modal_tests_part_3();
  run_tui_selector_tests();
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
  //  run_tui_prompt_search_race_tests();
}
