#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

void run_tui_completion_tests()
{
  tui_test_support::TuiFrontEndFixture frontend;
  auto const& slash_commands = tui_test_support::standard_slash_commands();
  auto const grep_commands = ava::tui::filter_slash_commands("/gr", slash_commands);
  expect(grep_commands.size() == 1 && grep_commands.front().command == "/grep", "tui slash palette filters commands by typed prefix");
  auto const fuzzy_grep_commands = ava::tui::filter_slash_commands("/gp", slash_commands);
  expect(fuzzy_grep_commands.size() == 1 && fuzzy_grep_commands.front().command == "/grep" && ava::tui::slash_palette_visible("/gp", slash_commands) &&
             ava::tui::slash_command_selection_text("/gp", slash_commands, 0) == "/grep ",
         "tui slash palette fuzzy-matches command names like Pi");
  expect(ava::tui::filter_slash_commands("hello", slash_commands).empty(), "tui slash palette stays hidden for normal chat input");
  expect(ava::tui::slash_palette_visible("/g", slash_commands), "tui slash palette is visible while filtering commands");
  expect(!ava::tui::slash_palette_visible("/help", slash_commands), "tui slash palette hides after an exact no-argument command");
  expect(ava::tui::slash_command_selection_text("/g", slash_commands, 1) == "/glob ",
         "tui slash selection fills argument-taking command with a trailing space");
  expect(ava::tui::slash_command_selection_text("/h", slash_commands, 0) == "/help", "tui slash selection fills no-argument command without submitting it");
  expect(ava::tui::clamp_slash_palette_selection("/g", slash_commands, 99) == 1, "tui clamps out-of-range slash palette selection to the last match");
  expect(ava::tui::previous_slash_palette_selection("/g", slash_commands, 0) == 1 && ava::tui::next_slash_palette_selection("/g", slash_commands, 1) == 0,
         "tui slash palette arrow selection wraps through filtered commands");

  std::vector<ava::tui::SlashCommandItem> const argument_slash_commands = {
      ava::tui::SlashCommandItem{
          .command = "/models",
          .description = "List models",
          .hint = "[query|provider/model]",
          .category = "Models",
          .aliases = {"/model"},
          .argument_completions = {ava::tui::SlashCommandArgumentCompletion{
                                       .value = "openai/gpt-5.5", .description = "GPT-5.5", .category = "Models", .argument_index = 0, .append_space = false},
                                   ava::tui::SlashCommandArgumentCompletion{.value = "anthropic/claude-sonnet-4-5",
                                                                            .description = "Claude Sonnet 4.5",
                                                                            .category = "Models",
                                                                            .argument_index = 0,
                                                                            .append_space = false}}},
      ava::tui::SlashCommandItem{.command = "/mcp",
                                 .description = "MCP",
                                 .hint = "<list|inspect|tools|restart> ...",
                                 .category = "Plugins",
                                 .argument_completions = {ava::tui::SlashCommandArgumentCompletion{
                                                              .value = "inspect", .description = "Inspect server", .category = "MCP", .argument_index = 0},
                                                          ava::tui::SlashCommandArgumentCompletion{.value = "fs",
                                                                                                   .description = "Filesystem server",
                                                                                                   .category = "MCP",
                                                                                                   .required_previous_args = {"inspect"},
                                                                                                   .argument_index = 1,
                                                                                                   .append_space = false}}}};
  // F3 red-first coverage: display labels remain renderer-only while matching and insertion use canonical values.
  std::vector<ava::tui::SlashCommandItem> const labelled_argument_commands = {
      ava::tui::SlashCommandItem{.command = "/models",
                                 .description = {},
                                 .hint = "<model>",
                                 .argument_completions = {ava::tui::SlashCommandArgumentCompletion{.value = "openai/gpt-5.5",
                                                                                                   .display_label = "GPT 5.5 Sol",
                                                                                                   .description = "available",
                                                                                                   .category = "Models",
                                                                                                   .argument_index = 0,
                                                                                                   .append_space = false}}}};
  auto const labelled_matches = ava::tui::filter_slash_commands("/models sol", labelled_argument_commands);
  auto const labelled_selection = ava::tui::slash_command_selection_text("/models sol", labelled_argument_commands, 0);
  auto const labelled_palette = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                          //.provider = "openai",
                                                                                                          //.model = "gpt-5.5",
                                                                                                          .session_id = "session_test",
                                                                                                          .input = "/models sol",
                                                                                                          .status = "ready",
                                                                                                          .transcript = {},
                                                                                                          .slash_commands = labelled_argument_commands,
                                                                                                          .width = 80,
                                                                                                          .height = 12});
  expect(labelled_matches.size() == 1 && labelled_matches.front().display_label == "GPT 5.5 Sol" && labelled_selection == "/models openai/gpt-5.5" &&
             std::ranges::any_of(labelled_palette, [](std::string const& line) { return strip_sgr(line).find("GPT 5.5 Sol") != std::string::npos; }),
         "F3 model completion labels are searchable and visible while canonical values are inserted");

  auto const premium_palette = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                         //.provider = "openai",
                                                                                                         //.model = "gpt-5.5",
                                                                                                         .session_id = "session_test",
                                                                                                         .input = "/models sol",
                                                                                                         .status = "ready",
                                                                                                         .transcript = {},
                                                                                                         .slash_commands = labelled_argument_commands,
                                                                                                         .width = 80,
                                                                                                         .height = 12});
  auto const compact_palette = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                         //.provider = "openai",
                                                                                                         //.model = "gpt-5.5",
                                                                                                         .session_id = "session_test",
                                                                                                         .input = "/models sol",
                                                                                                         .status = "ready",
                                                                                                         .transcript = {},
                                                                                                         .slash_commands = labelled_argument_commands,
                                                                                                         .width = 40,
                                                                                                         .height = 12});
  expect(std::ranges::any_of(premium_palette,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT 5.5 Sol") != std::string::npos && visible.find("available") != std::string::npos;
                             }) &&
             std::ranges::all_of(compact_palette, [](std::string const& line) { return visible_columns(line) <= 40; }) &&
             std::ranges::any_of(compact_palette, [](std::string const& line) { return strip_sgr(line).find("GPT") != std::string::npos; }),
         "F3 palette prioritizes a selected value then useful detail at 80 columns and bounds it at 40 columns");

  std::vector<ava::tui::FileReferenceItem> const disabled_references = {
      ava::tui::FileReferenceItem{.value = "private.txt", .description = "\x1b[2Jblocked\x01", .enabled = false, .disabled_reason = "outside workspace"}};
  auto const disabled_reference_palette = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                                    //.provider = "openai",
                                                                                                                    //.model = "gpt-5.5",
                                                                                                                    .session_id = "session_test",
                                                                                                                    .input = "see @p",
                                                                                                                    .status = "ready",
                                                                                                                    .transcript = {},
                                                                                                                    .file_references = disabled_references,
                                                                                                                    .width = 80,
                                                                                                                    .height = 12});
  expect(
      ava::tui::file_reference_selection_disabled_reason("see @p", std::string("see @p").size(), disabled_references, 0) == "outside workspace" &&
          ava::tui::path_completion_selection_disabled_reason("private", std::string("private").size(), disabled_references, 0, true) == "outside workspace" &&
          std::ranges::all_of(disabled_reference_palette,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find('\x1b') == std::string::npos && visible.find('\x01') == std::string::npos;
                              }),
      "F3 disabled reference and path queries preserve authoritative values while palette output sanitizes controls");

  // F3-R3: active Submit checks this pure decision before nonblocking-command callback dispatch; MessageFollowUp and queueing retain it at their mutation
  // boundary.
  auto const disabled_argument_commands = std::vector<ava::tui::SlashCommandItem>{
      ava::tui::SlashCommandItem{.command = "/share",
                                 .description = {},
                                 .hint = "<target>",
                                 .argument_completions = {ava::tui::SlashCommandArgumentCompletion{
                                     .value = "team", .argument_index = 0, .enabled = false, .disabled_reason = "sharing unavailable"}}}};
  auto const enabled_argument_commands = std::vector<ava::tui::SlashCommandItem>{
      ava::tui::SlashCommandItem{.command = "/share",
                                 .description = {},
                                 .hint = "<target>",
                                 .argument_completions = {ava::tui::SlashCommandArgumentCompletion{.value = "team", .argument_index = 0, .enabled = true}}}};
  auto disabled_argument_snapshot = ava::tui::ComposerSnapshot{};
  disabled_argument_snapshot.input = "/share team trailing";
  disabled_argument_snapshot.input_cursor = std::string("/share team").size();
  disabled_argument_snapshot.slash_commands = disabled_argument_commands;
  auto disabled_reference_snapshot = disabled_argument_snapshot;
  disabled_reference_snapshot.input = "see @private";
  disabled_reference_snapshot.input_cursor = disabled_reference_snapshot.input.size();
  disabled_reference_snapshot.slash_commands.clear();
  disabled_reference_snapshot.file_references = disabled_references;
  auto disabled_natural_path_snapshot = disabled_reference_snapshot;
  disabled_natural_path_snapshot.input = "inspect ./private";
  disabled_natural_path_snapshot.input_cursor = disabled_natural_path_snapshot.input.size();
  // This is a recognized active nonblocking slash draft; its forced path candidate must be rejected before Submit can dispatch its callback.
  auto disabled_forced_path_snapshot = disabled_reference_snapshot;
  disabled_forced_path_snapshot.input = "/jobs private";
  disabled_forced_path_snapshot.input_cursor = disabled_forced_path_snapshot.input.size();
  disabled_forced_path_snapshot.path_completion_force_active = true;
  auto suppressed_disabled_snapshot = disabled_argument_snapshot;
  suppressed_disabled_snapshot.slash_palette_suppressed = true;
  auto enabled_argument_snapshot = disabled_argument_snapshot;
  enabled_argument_snapshot.slash_commands = enabled_argument_commands;
  auto enabled_reference_snapshot = disabled_reference_snapshot;
  enabled_reference_snapshot.file_references.front().enabled = true;
  auto enabled_natural_path_snapshot = disabled_natural_path_snapshot;
  enabled_natural_path_snapshot.file_references.front().enabled = true;
  auto enabled_forced_path_snapshot = disabled_forced_path_snapshot;
  enabled_forced_path_snapshot.file_references.front().enabled = true;
  auto const disabled_argument_input = disabled_argument_snapshot.input;
  auto const disabled_argument_cursor = disabled_argument_snapshot.input_cursor;
  auto const disabled_forced_path_input = disabled_forced_path_snapshot.input;
  auto const disabled_forced_path_cursor = disabled_forced_path_snapshot.input_cursor;
  expect(ava::tui::detail::disabled_visible_completion_selection_status(disabled_argument_snapshot) == "command disabled: sharing unavailable" &&
             ava::tui::detail::disabled_visible_completion_selection_status(disabled_reference_snapshot) == "reference disabled: outside workspace" &&
             ava::tui::detail::disabled_visible_completion_selection_status(disabled_natural_path_snapshot) == "path disabled: outside workspace" &&
             ava::tui::detail::disabled_visible_completion_selection_status(disabled_forced_path_snapshot) == "path disabled: outside workspace" &&
             !ava::tui::detail::disabled_visible_completion_selection_status(suppressed_disabled_snapshot) &&
             !ava::tui::detail::disabled_visible_completion_selection_status(enabled_argument_snapshot) &&
             !ava::tui::detail::disabled_visible_completion_selection_status(enabled_reference_snapshot) &&
             !ava::tui::detail::disabled_visible_completion_selection_status(enabled_natural_path_snapshot) &&
             !ava::tui::detail::disabled_visible_completion_selection_status(enabled_forced_path_snapshot) &&
             disabled_argument_snapshot.input == disabled_argument_input && disabled_argument_snapshot.input_cursor == disabled_argument_cursor &&
             disabled_forced_path_snapshot.input == disabled_forced_path_input && disabled_forced_path_snapshot.input_cursor == disabled_forced_path_cursor,
         "F3 active route guard precedes callback dispatch and preserves input/cursor while classifying visible slash argument, reference, natural and forced "
         "path "
         "selections");

  auto dock_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                  //.provider = "openai",
                                                  //.model = "gpt-5.5",
                                                  .session_id = "session_test",
                                                  .input = "/m",
                                                  .status = "invalid_argument: alert one\nalert two",
                                                  .transcript = {},
                                                  .slash_commands = labelled_argument_commands,
                                                  .width = 100,
                                                  .height = 12,
                                                  .queued_messages = {ava::tui::QueuedMessageItem{.id = "q", .kind = "follow-up", .text = "queued"}},
                                                  .pending_attachments = {ava::tui::PendingAttachmentItem{.label = "image.png"}}};
  auto const dock_layout = ava::tui::composer_palette_screen_layout(dock_snapshot);
  expect(dock_layout && ava::tui::slash_palette_selection_for_screen_position(dock_snapshot, dock_layout->first_item_row, 1) == 0 &&
             !ava::tui::slash_palette_selection_for_screen_position(dock_snapshot, dock_layout->first_item_row + dock_layout->item_count, 1),
         "F3 palette hit testing uses the rendered dock layout and rejects adjacent dock rows");
  auto automatic_rail_dock_snapshot = dock_snapshot;
  automatic_rail_dock_snapshot.width = 176;
  automatic_rail_dock_snapshot.height = 36;
  automatic_rail_dock_snapshot.sidebar = ava::tui::SidebarSnapshot{};
  auto const automatic_rail_dock_layout = ava::tui::composer_palette_screen_layout(automatic_rail_dock_snapshot);
  expect(automatic_rail_dock_layout && ava::tui::composer_main_width(automatic_rail_dock_snapshot) == 137 &&
             ava::tui::slash_palette_selection_for_screen_position(automatic_rail_dock_snapshot, automatic_rail_dock_layout->first_item_row, 137) == 0 &&
             !ava::tui::slash_palette_selection_for_screen_position(automatic_rail_dock_snapshot, automatic_rail_dock_layout->first_item_row, 0) &&
             !ava::tui::slash_palette_selection_for_screen_position(automatic_rail_dock_snapshot, automatic_rail_dock_layout->first_item_row, 138) &&
             !ava::tui::slash_palette_selection_for_screen_position(automatic_rail_dock_snapshot, automatic_rail_dock_layout->first_item_row, 160),
         "F3 slash palette screen-position hit testing accepts the automatic-rail main pane and rejects column zero, divider, and sidebar while retaining dock "
         "mapping");
  auto centered_dock_snapshot = dock_snapshot;
  centered_dock_snapshot.width = 160;
  centered_dock_snapshot.height = 36;
  auto const centered_dock_layout = ava::tui::composer_palette_screen_layout(centered_dock_snapshot);
  expect(centered_dock_layout && ava::tui::composer_canvas_layout(centered_dock_snapshot).left == 20 &&
             ava::tui::slash_palette_selection_for_screen_position(centered_dock_snapshot, centered_dock_layout->first_item_row, 21) == 0 &&
             ava::tui::slash_palette_selection_for_screen_position(centered_dock_snapshot, centered_dock_layout->first_item_row, 140) == 0 &&
             !ava::tui::slash_palette_selection_for_screen_position(centered_dock_snapshot, centered_dock_layout->first_item_row, 20) &&
             !ava::tui::slash_palette_selection_for_screen_position(centered_dock_snapshot, centered_dock_layout->first_item_row, 141),
         "F3 centered slash palette accepts both canvas edges and rejects the exact left and right gutters");

  auto active_hint_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      //.provider = "openai",
      //.model = "gpt-5.5",
      .session_id = "session_test",
      .input = "follow up",
      .status = "invalid_argument: admitted",
      .processing = true,
      .active_run_hint = ava::tui::ActiveRunHint{.submit_or_queue = "Ctrl+Q", .follow_up = "Alt+F", .dequeue = "Alt+D", .interrupt = "Esc"},
      .transcript = {},
      .width = 80,
      .height = 12};
  auto const active_hint_lines = ava::tui::render_composer(frontend.frontend(), active_hint_snapshot);
  active_hint_snapshot.processing = false;
  auto const idle_hint_lines = ava::tui::render_composer(frontend.frontend(), active_hint_snapshot);
  expect(std::ranges::any_of(active_hint_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("Ctrl+Q queue") != std::string::npos && visible.find("Alt+F follow-up") != std::string::npos &&
                                      visible.find("Alt+D /restore") != std::string::npos;
                             }) &&
             std::ranges::none_of(idle_hint_lines, [](std::string const& line) { return strip_sgr(line).find("Ctrl+Q queue") != std::string::npos; }) &&
             std::ranges::any_of(active_hint_lines, [](std::string const& line) { return strip_sgr(line).starts_with("│  Esc stop"); }) &&
             std::ranges::any_of(active_hint_lines,
                                 [](std::string const& line) { return strip_sgr(line).find("! invalid_argument: admitted") != std::string::npos; }),
         "F3 contextual active-run discovery uses the shared composer gutter, configured keys, disappears idle, and stays above admitted errors");

  auto const model_argument_matches = ava::tui::filter_slash_commands("/model open", argument_slash_commands);
  expect(!model_argument_matches.empty() && model_argument_matches.front().argument_completion && model_argument_matches.front().command == "openai/gpt-5.5",
         "tui slash palette ranks the strongest backend-provided argument completion after a command alias first");
  auto const fuzzy_model_argument_matches = ava::tui::filter_slash_commands("/models sonnet", argument_slash_commands);
  expect(fuzzy_model_argument_matches.size() == 1 && fuzzy_model_argument_matches.front().argument_completion &&
             fuzzy_model_argument_matches.front().command == "anthropic/claude-sonnet-4-5" &&
             ava::tui::slash_palette_visible("/models sonnet", argument_slash_commands) &&
             ava::tui::slash_command_selection_text("/models sonnet", argument_slash_commands, 0) == "/models anthropic/claude-sonnet-4-5",
         "tui slash palette fuzzy-matches non-file argument completions like Pi model search");
  auto const swapped_model_argument_matches = ava::tui::filter_slash_commands("/models 4sonnet", argument_slash_commands);
  expect(swapped_model_argument_matches.size() == 1 && swapped_model_argument_matches.front().argument_completion &&
             swapped_model_argument_matches.front().command == "anthropic/claude-sonnet-4-5",
         "tui slash palette supports Pi-style swapped numeric/name fuzzy argument queries");
  expect(ava::tui::slash_palette_visible("/models open", argument_slash_commands) &&
             ava::tui::slash_command_selection_text("/models open", argument_slash_commands, 0) == "/models openai/gpt-5.5",
         "tui slash selection inserts explicit backend-provided argument completion text");
  expect(ava::tui::slash_command_selection_text("/mcp inspect f", argument_slash_commands, 0) == "/mcp inspect fs",
         "tui argument completion preserves required previous arguments for nested command forms");
  auto const command_cursor_input = std::string("/models open");
  auto const command_cursor = std::string("/models").size();
  auto const command_cursor_matches = ava::tui::filter_slash_commands(command_cursor_input, command_cursor, argument_slash_commands);
  expect(command_cursor_matches.size() == 1 && command_cursor_matches.front().command == "/models" && !command_cursor_matches.front().argument_completion &&
             ava::tui::slash_palette_visible(command_cursor_input, command_cursor, argument_slash_commands),
         "tui slash palette re-queries command-name completions when the cursor moves before slash arguments like Pi");
  auto const command_cursor_selection = ava::tui::slash_command_selection_text("/mod open", std::string("/mod").size(), argument_slash_commands, 0);
  expect(command_cursor_selection.text == "/models open" && command_cursor_selection.cursor == std::string("/models ").size(),
         "tui cursor-scoped slash selection preserves suffix text after the command-name cursor");
  std::vector<ava::tui::SlashCommandItem> const connect_slash_commands = {
      ava::tui::SlashCommandItem{.command = "/connect", .description = "Connect a provider", .category = "General"}};
  expect(!ava::tui::slash_palette_visible("/connect", connect_slash_commands) && !ava::tui::slash_palette_visible("/connect ", connect_slash_commands) &&
             !ava::tui::slash_palette_visible("/connect openai", connect_slash_commands) &&
             ava::tui::filter_slash_commands("/connect openai ", connect_slash_commands).empty(),
         "tui slash palette lets /connect submit directly so provider and method choices stay in the centered modal");
  auto const argument_palette = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                          //.provider = "openai",
                                                                                                          //.model = "gpt-5.5",
                                                                                                          .session_id = "session_test",
                                                                                                          .input = "/model open",
                                                                                                          .status = "ready",
                                                                                                          .transcript = {},
                                                                                                          .slash_commands = argument_slash_commands,
                                                                                                          .selected_slash_command_index = 0,
                                                                                                          .width = 96,
                                                                                                          .height = 10});
  expect(std::ranges::any_of(argument_palette,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("openai/gpt-5.5") != std::string::npos && visible.find("GPT-5.5") != std::string::npos &&
                                      visible.find("Models") == std::string::npos;
                             }) &&
             std::ranges::none_of(argument_palette, [](std::string const& line) { return strip_sgr(line).find("[complete]") != std::string::npos; }),
         "tui slash palette prioritizes argument value and description while suppressing the redundant category and the old [complete] hint");
  // append_space=false argument rows must not show a visual [complete] marker; bare /copy and /thinking exact-submit remain catalog-driven.
  auto const terminal_argument_matches = ava::tui::filter_slash_commands("/models open", argument_slash_commands);
  expect(!terminal_argument_matches.empty() && terminal_argument_matches.front().argument_completion && terminal_argument_matches.front().hint.empty() &&
             ava::tui::slash_command_selection_text("/models open", argument_slash_commands, 0) == "/models openai/gpt-5.5",
         "argument completion rows omit [complete] while still inserting exact values without a forced trailing space");
  std::vector<ava::tui::SlashCommandItem> const bare_exact_commands = {
      ava::tui::SlashCommandItem{
          .command = "/copy", .description = "Copy the latest AVA message, user turn, tool, or permission details", .category = "Session"},
      ava::tui::SlashCommandItem{
          .command = "/thinking", .description = "Toggle thinking visibility; /thinking details inspects the latest block", .category = "Session"}};
  expect(!ava::tui::slash_palette_visible("/copy", bare_exact_commands) && !ava::tui::slash_palette_visible("/thinking", bare_exact_commands),
         "bare /copy and /thinking remain exact one-Enter submissions without a completion palette");
  auto const cursor_scoped_argument_palette =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = command_cursor_input,
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .slash_commands = argument_slash_commands,
                                                                                .selected_slash_command_index = 0,
                                                                                .width = 96,
                                                                                .height = 10,
                                                                                .input_cursor = command_cursor});
  expect(std::ranges::any_of(cursor_scoped_argument_palette, [](std::string const& line) { return strip_sgr(line).find("/models") != std::string::npos; }) &&
             std::ranges::none_of(cursor_scoped_argument_palette,
                                  [](std::string const& line) { return strip_sgr(line).find("openai/gpt-5.5") != std::string::npos; }),
         "tui rendered slash palette drops stale argument suggestions after cursor movement into the command name");
  std::vector<ava::tui::SlashCommandItem> const path_slash_commands = {ava::tui::SlashCommandItem{
      .command = "/read",
      .description = "Read a file",
      .hint = "<path>",
      .category = "Files",
      .argument_completions = {ava::tui::SlashCommandArgumentCompletion{
                                   .value = "src/", .description = "directory", .category = "Files", .argument_index = 0, .append_space = false},
                               ava::tui::SlashCommandArgumentCompletion{
                                   .value = "src/main.cpp", .description = "file 24 bytes", .category = "Files", .argument_index = 0, .append_space = false}}}};
  expect(ava::tui::slash_command_selection_text("/read sr", path_slash_commands, 0) == "/read src/" &&
             ava::tui::slash_palette_visible("/read src/", path_slash_commands) &&
             ava::tui::slash_command_selection_text("/read src/", path_slash_commands, 1) == "/read src/main.cpp",
         "tui slash path completion keeps directory prefixes open for nested file completions");
  expect(!ava::tui::slash_palette_visible("/read /", path_slash_commands) && ava::tui::filter_slash_commands("/read /", path_slash_commands).empty(),
         "tui slash argument palette yields when the current prefix has no backend completion match");
  auto const path_palette = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                      //.provider = "openai",
                                                                                                      //.model = "gpt-5.5",
                                                                                                      .session_id = "session_test",
                                                                                                      .input = "/read src/",
                                                                                                      .status = "ready",
                                                                                                      .transcript = {},
                                                                                                      .slash_commands = path_slash_commands,
                                                                                                      .selected_slash_command_index = 1,
                                                                                                      .width = 96,
                                                                                                      .height = 10});
  expect(std::ranges::any_of(path_palette,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("src/main.cpp") != std::string::npos && visible.find("file 24 bytes") == std::string::npos &&
                                      visible.find("Files") == std::string::npos;
                             }),
         "tui slash palette renders canonical paths without duplicated file metadata or category");
  std::vector<ava::tui::FileReferenceItem> const file_references = {
      ava::tui::FileReferenceItem{.value = "src/", .description = "directory", .category = "Files", .directory = true},
      ava::tui::FileReferenceItem{.value = "src/main.cpp", .description = "file 24 bytes", .category = "Files"},
      ava::tui::FileReferenceItem{.value = "src/components/Button.tsx", .description = "file 42 bytes", .category = "Files"},
      ava::tui::FileReferenceItem{.value = "my folder/", .description = "directory", .category = "Files", .directory = true},
      ava::tui::FileReferenceItem{.value = "my folder/test file.txt", .description = "file 12 bytes", .category = "Files"}};
  auto const reference_matches = ava::tui::filter_file_references("review @sr", std::string("review @sr").size(), file_references);
  expect(reference_matches.size() >= 2 && reference_matches.front().value == "src/" &&
             std::ranges::any_of(reference_matches, [](auto const& item) { return item.value == "src/main.cpp"; }),
         "tui file reference palette filters @ prefixes against backend candidates");
  auto const fuzzy_reference_matches = ava::tui::filter_file_references("review @comp/but", std::string("review @comp/but").size(), file_references);
  expect(fuzzy_reference_matches.size() == 1 && fuzzy_reference_matches.front().value == "src/components/Button.tsx",
         "tui file reference palette fuzzy-matches slash-separated path tokens against nested candidates");
  auto const case_reference_matches = ava::tui::filter_file_references("review @MAIN", std::string("review @MAIN").size(), file_references);
  expect(case_reference_matches.size() == 1 && case_reference_matches.front().value == "src/main.cpp",
         "tui file reference palette matches @ queries case-insensitively");
  auto const spaced_reference_matches = ava::tui::filter_file_references("review @my", std::string("review @my").size(), file_references);
  expect(spaced_reference_matches.size() == 2 && spaced_reference_matches.front().value == "my folder/",
         "tui file reference palette includes paths with spaces and ranks matching directories first");
  auto const equals_reference_matches = ava::tui::filter_file_references("include=@sr", std::string("include=@sr").size(), file_references);
  expect(equals_reference_matches.size() >= 2 && equals_reference_matches.front().value == "src/",
         "tui file reference palette treats equals as a token delimiter like Pi");
  auto const single_quote_reference_matches = ava::tui::filter_file_references("include='@sr", std::string("include='@sr").size(), file_references);
  expect(single_quote_reference_matches.size() >= 2 && single_quote_reference_matches.front().value == "src/",
         "tui file reference palette treats single quotes as token delimiters like Pi");
  auto const parenthesized_reference_matches = ava::tui::filter_file_references("compare (@sr", std::string("compare (@sr").size(), file_references);
  expect(parenthesized_reference_matches.size() >= 2 && parenthesized_reference_matches.front().value == "src/",
         "tui file reference palette opens after parenthesized prose boundaries");
  auto const bracketed_reference_matches = ava::tui::filter_file_references("compare [@sr", std::string("compare [@sr").size(), file_references);
  expect(bracketed_reference_matches.size() >= 2 && bracketed_reference_matches.front().value == "src/",
         "tui file reference palette opens after bracketed prose boundaries");
  auto const reference_selection = ava::tui::file_reference_selection_text("review @main please", std::string("review @main").size(), file_references, 0);
  expect(reference_selection.text == "review @src/main.cpp please" && reference_selection.cursor == std::string("review @src/main.cpp").size(),
         "tui file reference selection replaces only the active @ token and leaves surrounding draft text intact");
  auto const spaced_directory_selection = ava::tui::file_reference_selection_text("review @my", std::string("review @my").size(), file_references, 0);
  expect(spaced_directory_selection.text == "review @\"my folder/\"" && spaced_directory_selection.cursor == std::string("review @\"my folder/").size(),
         "tui file reference selection quotes directories with spaces and leaves the cursor inside the quote");
  auto const spaced_file_selection = ava::tui::file_reference_selection_text("review @my", std::string("review @my").size(), file_references, 1);
  expect(spaced_file_selection.text == "review @\"my folder/test file.txt\" " && spaced_file_selection.cursor == spaced_file_selection.text.size(),
         "tui file reference selection quotes files with spaces and appends a safe separator");
  auto const quoted_file_selection =
      ava::tui::file_reference_selection_text("review @\"my folder/te\"", std::string("review @\"my folder/te").size(), file_references, 1);
  expect(quoted_file_selection.text == "review @\"my folder/test file.txt\" " && quoted_file_selection.cursor == quoted_file_selection.text.size(),
         "tui file reference selection completes inside quoted @ paths without duplicating the closing quote");
  auto const equals_reference_selection = ava::tui::file_reference_selection_text("include=@main", std::string("include=@main").size(), file_references, 0);
  expect(equals_reference_selection.text == "include=@src/main.cpp " && equals_reference_selection.cursor == equals_reference_selection.text.size(),
         "tui file reference selection replaces only the @ token after an equals delimiter");
  auto const single_quote_reference_selection =
      ava::tui::file_reference_selection_text("include='@main", std::string("include='@main").size(), file_references, 0);
  expect(single_quote_reference_selection.text == "include='@src/main.cpp " &&
             single_quote_reference_selection.cursor == single_quote_reference_selection.text.size(),
         "tui file reference selection replaces only the @ token after a single-quote delimiter");
  auto const parenthesized_reference_selection =
      ava::tui::file_reference_selection_text("compare (@main)", std::string("compare (@main").size(), file_references, 0);
  expect(parenthesized_reference_selection.text == "compare (@src/main.cpp)" &&
             parenthesized_reference_selection.cursor == std::string("compare (@src/main.cpp").size(),
         "tui file reference selection preserves closing punctuation without inserting an extra space");
  auto const reference_palette = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                           //.provider = "openai",
                                                                                                           //.model = "gpt-5.5",
                                                                                                           .session_id = "session_test",
                                                                                                           .input = "review @sr",
                                                                                                           .status = "ready",
                                                                                                           .transcript = {},
                                                                                                           .file_references = file_references,
                                                                                                           .selected_slash_command_index = 1,
                                                                                                           .width = 96,
                                                                                                           .height = 10,
                                                                                                           .input_cursor = std::string("review @sr").size()});
  auto const reference_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                             //.provider = "openai",
                                                             //.model = "gpt-5.5",
                                                             .session_id = "session_test",
                                                             .input = "review @sr",
                                                             .status = "ready",
                                                             .transcript = {},
                                                             .file_references = file_references,
                                                             .selected_slash_command_index = 1,
                                                             .width = 96,
                                                             .height = 10,
                                                             .input_cursor = std::string("review @sr").size()};
  auto const reference_hit_row =
      static_cast<std::size_t>(
          std::ranges::find_if(reference_palette, [](std::string const& line) { return strip_sgr(line).find("@src/main.cpp") != std::string::npos; }) -
          reference_palette.begin()) +
      1;
  auto const expected_reference_index = static_cast<std::size_t>(
      std::ranges::find_if(reference_matches, [](auto const& item) { return item.value == "src/main.cpp"; }) - reference_matches.begin());
  auto const clicked_reference = ava::tui::file_reference_palette_selection_for_screen_position(reference_snapshot, reference_hit_row, 1);
  expect(std::ranges::any_of(reference_palette,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("@src/main.cpp") != std::string::npos && visible.find("file 24 bytes") == std::string::npos &&
                                      visible.find("Files") == std::string::npos;
                             }) &&
             clicked_reference && *clicked_reference == expected_reference_index &&
             !ava::tui::file_reference_palette_selection_for_screen_position(reference_snapshot, 1, 1),
         "tui file reference palette renders @ candidates and hit-tests visible rows to candidate indices");
  auto automatic_rail_reference_snapshot = reference_snapshot;
  automatic_rail_reference_snapshot.width = 176;
  automatic_rail_reference_snapshot.height = 36;
  automatic_rail_reference_snapshot.sidebar = ava::tui::SidebarSnapshot{};
  auto const automatic_rail_reference_layout = ava::tui::composer_palette_screen_layout(automatic_rail_reference_snapshot);
  expect(automatic_rail_reference_layout &&
             ava::tui::file_reference_palette_selection_for_screen_position(automatic_rail_reference_snapshot, automatic_rail_reference_layout->first_item_row,
                                                                            137) == 0 &&
             !ava::tui::file_reference_palette_selection_for_screen_position(automatic_rail_reference_snapshot, automatic_rail_reference_layout->first_item_row,
                                                                             138) &&
             !ava::tui::file_reference_palette_selection_for_screen_position(automatic_rail_reference_snapshot, automatic_rail_reference_layout->first_item_row,
                                                                             160),
         "F3 @ palette screen-position hit testing rejects automatic-rail divider and sidebar columns");
  auto centered_reference_snapshot = reference_snapshot;
  centered_reference_snapshot.width = 160;
  centered_reference_snapshot.height = 36;
  auto const centered_reference_layout = ava::tui::composer_palette_screen_layout(centered_reference_snapshot);
  expect(centered_reference_layout &&
             ava::tui::file_reference_palette_selection_for_screen_position(centered_reference_snapshot, centered_reference_layout->first_item_row, 21) == 0 &&
             ava::tui::file_reference_palette_selection_for_screen_position(centered_reference_snapshot, centered_reference_layout->first_item_row, 140) == 0 &&
             !ava::tui::file_reference_palette_selection_for_screen_position(centered_reference_snapshot, centered_reference_layout->first_item_row, 20) &&
             !ava::tui::file_reference_palette_selection_for_screen_position(centered_reference_snapshot, centered_reference_layout->first_item_row, 141),
         "F3 centered @ palette accepts both canvas edges and rejects the exact left and right gutters");
  auto const spaced_reference_palette =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "review @my",
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .file_references = file_references,
                                                                                .selected_slash_command_index = 0,
                                                                                .width = 96,
                                                                                .height = 10,
                                                                                .input_cursor = std::string("review @my").size()});
  expect(std::ranges::any_of(spaced_reference_palette,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("@\"my folder/\"") != std::string::npos && visible.find("dir") != std::string::npos &&
                                      visible.find("directory") == std::string::npos;
                             }),
         "tui file reference palette renders quoted @ directories with one quiet type cue");
  auto const normal_path_matches = ava::tui::filter_path_completions("inspect src/", std::string("inspect src/").size(), file_references);
  expect(normal_path_matches.size() >= 2 && normal_path_matches.front().value == "src/" &&
             std::ranges::any_of(normal_path_matches, [](auto const& item) { return item.value == "src/main.cpp"; }),
         "tui normal path completion filters path-like prompt tokens against backend candidates");
  auto const equals_path_matches = ava::tui::filter_path_completions("inspect file=src/", std::string("inspect file=src/").size(), file_references);
  expect(equals_path_matches.size() >= 2 && equals_path_matches.front().value == "src/", "tui normal path completion treats equals as a path token delimiter");
  auto const single_quote_path_matches = ava::tui::filter_path_completions("inspect path='src/", std::string("inspect path='src/").size(), file_references);
  expect(single_quote_path_matches.size() >= 2 && single_quote_path_matches.front().value == "src/",
         "tui normal path completion treats single quotes as path token delimiters like Pi");
  auto const whitespace_path_matches = ava::tui::filter_path_completions("inspect ", std::string("inspect ").size(), file_references);
  expect(whitespace_path_matches.empty(), "tui normal path completion stays closed at a new token after whitespace");
  expect(!ava::tui::path_completion_palette_visible("inspect ", std::string("inspect ").size(), file_references),
         "tui normal path completion palette stays hidden at a new token after whitespace");
  auto const forced_whitespace_path_matches = ava::tui::filter_path_completions("inspect ", std::string("inspect ").size(), file_references, true);
  expect(forced_whitespace_path_matches.size() == file_references.size() && forced_whitespace_path_matches.front().directory,
         "tui forced path completion can open workspace suggestions at a new token after whitespace");
  expect(ava::tui::filter_path_completions("inspect file=", std::string("inspect file=").size(), file_references).empty(),
         "tui normal path completion does not open an empty token after equals without explicit Tab");
  expect(ava::tui::filter_path_completions("inspect main", std::string("inspect main").size(), file_references).empty(),
         "tui normal path completion does not open for ordinary prose tokens");
  auto const forced_path_matches = ava::tui::filter_path_completions("inspect main", std::string("inspect main").size(), file_references, true);
  expect(forced_path_matches.size() == 1 && forced_path_matches.front().value == "src/main.cpp", "tui forced path completion can match a bare prompt token");
  auto const forced_equals_path_matches =
      ava::tui::filter_path_completions("inspect file=main", std::string("inspect file=main").size(), file_references, true);
  expect(forced_equals_path_matches.size() == 1 && forced_equals_path_matches.front().value == "src/main.cpp",
         "tui forced path completion matches the token after an equals delimiter");
  auto const forced_empty_matches = ava::tui::filter_path_completions("", 0, file_references, true);
  expect(forced_empty_matches.size() == file_references.size() && forced_empty_matches.front().directory,
         "tui forced path completion can open workspace suggestions from an empty draft");
  expect(ava::tui::filter_path_completions("/read", std::string("/read").size(), file_references, true).empty(),
         "tui forced path completion leaves top-level slash command names to the slash palette");
  auto const forced_slash_argument_matches = ava::tui::filter_path_completions("/read main", std::string("/read main").size(), file_references, true);
  expect(forced_slash_argument_matches.size() == 1 && forced_slash_argument_matches.front().value == "src/main.cpp",
         "tui forced path completion can match the current slash-command argument token like Pi");
  expect(ava::tui::filter_path_completions("inspect @src/", std::string("inspect @src/").size(), file_references).empty(),
         "tui normal path completion does not steal @ file reference tokens");
  auto const dot_slash_path_selection = ava::tui::path_completion_selection_text("inspect ./sr", std::string("inspect ./sr").size(), file_references, 0);
  expect(dot_slash_path_selection.text == "inspect ./src/" && dot_slash_path_selection.cursor == std::string("inspect ./src/").size(),
         "tui normal path completion preserves a typed dot-slash prefix");
  auto const quoted_path_selection =
      ava::tui::path_completion_selection_text("inspect \"my folder/te\"", std::string("inspect \"my folder/te").size(), file_references, 0);
  expect(quoted_path_selection.text == "inspect \"my folder/test file.txt\"" && quoted_path_selection.cursor == quoted_path_selection.text.size(),
         "tui normal path completion continues inside quoted paths without duplicating the closing quote");
  auto const equals_path_selection =
      ava::tui::path_completion_selection_text("inspect file=src/ma", std::string("inspect file=src/ma").size(), file_references, 0);
  expect(equals_path_selection.text == "inspect file=src/main.cpp" && equals_path_selection.cursor == equals_path_selection.text.size(),
         "tui normal path completion preserves text before an equals delimiter");
  auto const single_quote_path_selection =
      ava::tui::path_completion_selection_text("inspect path='src/ma", std::string("inspect path='src/ma").size(), file_references, 0);
  expect(single_quote_path_selection.text == "inspect path='src/main.cpp" && single_quote_path_selection.cursor == single_quote_path_selection.text.size(),
         "tui normal path completion preserves text before a single-quote delimiter");
  auto const equals_quoted_path_selection =
      ava::tui::path_completion_selection_text("inspect path=\"my folder/te\"", std::string("inspect path=\"my folder/te").size(), file_references, 0);
  expect(equals_quoted_path_selection.text == "inspect path=\"my folder/test file.txt\"" &&
             equals_quoted_path_selection.cursor == equals_quoted_path_selection.text.size(),
         "tui normal path completion continues inside quoted paths after equals delimiters");
  auto const whitespace_path_selection = ava::tui::path_completion_selection_text("inspect ", std::string("inspect ").size(), file_references, 1);
  expect(whitespace_path_selection.text == "inspect " && whitespace_path_selection.cursor == std::string("inspect ").size(),
         "tui normal path completion selection leaves text and cursor unchanged at an empty token after whitespace");
  auto const forced_bare_path_selection =
      ava::tui::path_completion_selection_text("inspect main", std::string("inspect main").size(), file_references, 0, true);
  expect(forced_bare_path_selection.text == "inspect src/main.cpp" && forced_bare_path_selection.cursor == forced_bare_path_selection.text.size(),
         "tui forced path completion replaces a bare token with the selected relative path");
  auto const forced_slash_argument_selection =
      ava::tui::path_completion_selection_text("/read main", std::string("/read main").size(), file_references, 0, true);
  expect(forced_slash_argument_selection.text == "/read src/main.cpp" && forced_slash_argument_selection.cursor == forced_slash_argument_selection.text.size(),
         "tui forced path completion replaces only the active slash-command argument token");
  auto const normal_path_palette =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "inspect src/",
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .file_references = file_references,
                                                                                .selected_slash_command_index = 1,
                                                                                .width = 96,
                                                                                .height = 10,
                                                                                .input_cursor = std::string("inspect src/").size()});
  auto const normal_path_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                               //.provider = "openai",
                                                               //.model = "gpt-5.5",
                                                               .session_id = "session_test",
                                                               .input = "inspect src/",
                                                               .status = "ready",
                                                               .transcript = {},
                                                               .file_references = file_references,
                                                               .selected_slash_command_index = 1,
                                                               .width = 96,
                                                               .height = 10,
                                                               .input_cursor = std::string("inspect src/").size()};
  auto const path_hit_row = static_cast<std::size_t>(std::ranges::find_if(normal_path_palette,
                                                                          [](std::string const& line) {
                                                                            auto const visible = strip_sgr(line);
                                                                            return visible.find("src/main.cpp") != std::string::npos;
                                                                          }) -
                                                     normal_path_palette.begin()) +
                            1;
  auto const expected_path_index = static_cast<std::size_t>(
      std::ranges::find_if(normal_path_matches, [](auto const& item) { return item.value == "src/main.cpp"; }) - normal_path_matches.begin());
  auto const clicked_path = ava::tui::path_completion_palette_selection_for_screen_position(normal_path_snapshot, path_hit_row, 1);
  expect(std::ranges::any_of(normal_path_palette,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("src/main.cpp") != std::string::npos && visible.find("file 24 bytes") == std::string::npos &&
                                      visible.find("Files") == std::string::npos;
                             }) &&
             clicked_path && *clicked_path == expected_path_index &&
             !ava::tui::path_completion_palette_selection_for_screen_position(normal_path_snapshot, 1, 1),
         "tui normal path completion palette renders backend-provided path candidates and hit-tests visible rows");
  auto automatic_rail_path_snapshot = normal_path_snapshot;
  automatic_rail_path_snapshot.width = 176;
  automatic_rail_path_snapshot.height = 36;
  automatic_rail_path_snapshot.sidebar = ava::tui::SidebarSnapshot{};
  auto const automatic_rail_path_layout = ava::tui::composer_palette_screen_layout(automatic_rail_path_snapshot);
  expect(
      automatic_rail_path_layout &&
          ava::tui::path_completion_palette_selection_for_screen_position(automatic_rail_path_snapshot, automatic_rail_path_layout->first_item_row, 137) == 0 &&
          !ava::tui::path_completion_palette_selection_for_screen_position(automatic_rail_path_snapshot, automatic_rail_path_layout->first_item_row, 138) &&
          !ava::tui::path_completion_palette_selection_for_screen_position(automatic_rail_path_snapshot, automatic_rail_path_layout->first_item_row, 160),
      "F3 path palette screen-position hit testing rejects automatic-rail divider and sidebar columns");
  auto centered_path_snapshot = normal_path_snapshot;
  centered_path_snapshot.width = 160;
  centered_path_snapshot.height = 36;
  auto const centered_path_layout = ava::tui::composer_palette_screen_layout(centered_path_snapshot);
  expect(centered_path_layout &&
             ava::tui::path_completion_palette_selection_for_screen_position(centered_path_snapshot, centered_path_layout->first_item_row, 21) == 0 &&
             ava::tui::path_completion_palette_selection_for_screen_position(centered_path_snapshot, centered_path_layout->first_item_row, 140) == 0 &&
             !ava::tui::path_completion_palette_selection_for_screen_position(centered_path_snapshot, centered_path_layout->first_item_row, 20) &&
             !ava::tui::path_completion_palette_selection_for_screen_position(centered_path_snapshot, centered_path_layout->first_item_row, 141),
         "F3 centered path palette accepts both canvas edges and rejects the exact left and right gutters");
  auto const forced_path_palette =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "inspect src",
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .file_references = file_references,
                                                                                .selected_slash_command_index = 0,
                                                                                .path_completion_force_active = true,
                                                                                .width = 96,
                                                                                .height = 10,
                                                                                .input_cursor = std::string("inspect src").size()});
  expect(std::ranges::any_of(forced_path_palette,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("src/") != std::string::npos && visible.find("dir") != std::string::npos &&
                                      visible.find("directory") == std::string::npos;
                             }),
         "tui forced path completion palette renders canonical directories with one quiet type cue after Tab");
}
